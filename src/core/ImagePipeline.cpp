#include "core/ImagePipeline.hpp"
#include <algorithm>
#include <set>
#include <unordered_set>
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>

namespace UltraImageViewer {
namespace Core {

ImagePipeline::ImagePipeline() = default;

ImagePipeline::~ImagePipeline()
{
    Shutdown();
}

void ImagePipeline::Initialize(ImageDecoder* decoder, CacheManager* cache,
                               Rendering::Direct2DRenderer* renderer)
{
    decoder_ = decoder;
    cache_ = cache;
    renderer_ = renderer;

    shutdownRequested_ = false;
    loadThread_ = std::jthread([this](std::stop_token) { LoadThreadFunc(); });
}

void ImagePipeline::Shutdown()
{
    shutdownRequested_ = true;
    queueCV_.notify_all();

    if (loadThread_.joinable()) {
        loadThread_.request_stop();
        loadThread_.join();
    }

    std::lock_guard lock(cacheMutex_);
    thumbnailCache_.clear();
    fullImageCache_.clear();
}

Microsoft::WRL::ComPtr<ID2D1Bitmap> ImagePipeline::GetBitmap(const std::filesystem::path& path)
{
    {
        std::lock_guard lock(cacheMutex_);
        auto it = fullImageCache_.find(path);
        if (it != fullImageCache_.end()) {
            return it->second;
        }
    }

    auto bitmap = DecodeAndCreateBitmap(path);
    if (bitmap) {
        std::lock_guard lock(cacheMutex_);
        fullImageCache_[path] = bitmap;
    }
    return bitmap;
}

void ImagePipeline::GetBitmapAsync(const std::filesystem::path& path, BitmapCallback callback)
{
    // Check cache first
    {
        std::lock_guard lock(cacheMutex_);
        auto it = fullImageCache_.find(path);
        if (it != fullImageCache_.end()) {
            if (callback) callback(it->second);
            return;
        }
    }

    LoadRequest req;
    req.path = path;
    req.callback = std::move(callback);
    req.isThumbnail = false;

    {
        std::lock_guard lock(queueMutex_);
        loadQueue_.push(std::move(req));
    }
    queueCV_.notify_one();
}

Microsoft::WRL::ComPtr<ID2D1Bitmap> ImagePipeline::GetThumbnail(const std::filesystem::path& path,
                                                                  uint32_t maxSize)
{
    {
        std::lock_guard lock(cacheMutex_);
        auto it = thumbnailCache_.find(path);
        if (it != thumbnailCache_.end()) {
            return it->second;
        }
    }

    auto bitmap = DecodeAndCreateThumbnail(path, maxSize);
    if (bitmap) {
        std::lock_guard lock(cacheMutex_);
        thumbnailCache_[path] = bitmap;
    }
    return bitmap;
}

void ImagePipeline::PrefetchAround(const std::vector<std::filesystem::path>& allPaths,
                                    size_t currentIndex, size_t radius)
{
    if (allPaths.empty()) return;

    for (size_t offset = 1; offset <= radius; ++offset) {
        // Forward
        if (currentIndex + offset < allPaths.size()) {
            const auto& path = allPaths[currentIndex + offset];
            if (!HasThumbnail(path)) {
                LoadRequest req;
                req.path = path;
                req.isThumbnail = true;
                req.maxSize = 256;
                std::lock_guard lock(queueMutex_);
                loadQueue_.push(std::move(req));
            }
        }
        // Backward
        if (currentIndex >= offset) {
            const auto& path = allPaths[currentIndex - offset];
            if (!HasThumbnail(path)) {
                LoadRequest req;
                req.path = path;
                req.isThumbnail = true;
                req.maxSize = 256;
                std::lock_guard lock(queueMutex_);
                loadQueue_.push(std::move(req));
            }
        }
    }
    queueCV_.notify_one();
}

std::vector<std::filesystem::path> ImagePipeline::ScanDirectory(const std::filesystem::path& dir)
{
    std::vector<std::filesystem::path> result;

    if (!std::filesystem::is_directory(dir)) {
        return result;
    }

    static const std::set<std::wstring> supportedExts = {
        L".jpg", L".jpeg", L".png", L".bmp", L".gif",
        L".tif", L".tiff", L".webp", L".ico", L".jxr"
    };

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().wstring();
        // Convert to lowercase
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (supportedExts.contains(ext)) {
            result.push_back(entry.path());
        }
    }

    // Sort by filename
    std::sort(result.begin(), result.end(),
        [](const std::filesystem::path& a, const std::filesystem::path& b) {
            return a.filename() < b.filename();
        });

    return result;
}

std::vector<ScannedImage> ImagePipeline::ScanFolders(
    const std::vector<std::filesystem::path>& folders,
    std::atomic<bool>& cancelFlag,
    std::atomic<size_t>& outCount,
    ScanFlushCallback flushCallback)
{
    std::vector<ScannedImage> result;
    std::unordered_set<std::wstring> seen;
    size_t lastFlushCount = 0;
    constexpr size_t kFlushInterval = 200;

    static const std::set<std::wstring> supportedExts = {
        L".jpg", L".jpeg", L".png", L".bmp", L".gif",
        L".tif", L".tiff", L".webp", L".ico", L".jxr",
        L".heic", L".heif", L".avif"
    };

    // Folder names to skip during recursive scan
    static const std::set<std::wstring> skipDirs = {
        // VCS / dev tooling
        L".git", L".svn", L".hg", L".vs", L".vscode", L".idea",
        L"node_modules", L"__pycache__", L".tox", L".mypy_cache",
        // Build artifacts
        L"Debug", L"Release", L"x64", L"x86", L"obj", L"bin",
        L"build", L"out", L"dist", L"target",
        // System / temp
        L"AppData", L"Temp", L"tmp",
        L"Cache", L"cache", L"CachedData",
        L"$RECYCLE.BIN", L"System Volume Information",
        // Icons / thumbnails / UI assets
        L"icons", L"icon", L"ico",
        L"thumbnails", L"thumbnail", L"thumb", L"thumbs",
        L"assets", L"Resources", L"resource", L"res",
        L"sprites", L"textures", L"drawable", L"drawable-hdpi",
        L"drawable-mdpi", L"drawable-xhdpi", L"drawable-xxhdpi",
        L"favicon", L"favicons", L"emoji", L"emojis", L"stickers",
        // Fonts / cursors
        L"fonts", L"font", L"cursors",
        // Package / library internals
        L"vendor", L"packages", L"lib", L"libs",
        L".nuget", L".npm", L".yarn",
        // Windows special
        L"Windows", L"ProgramData",
        L"Program Files", L"Program Files (x86)",
    };

    // Minimum file size to include (filter out icons, favicons, UI assets)
    constexpr ULONGLONG kMinImageSize = 100 * 1024;  // 100KB

    // Helper: sort current results and invoke flush callback
    auto doFlush = [&]() {
        if (!flushCallback) return;
        // Sort a copy for the callback (main result stays unsorted until final)
        auto sorted = result;
        std::sort(sorted.begin(), sorted.end(),
            [](const ScannedImage& a, const ScannedImage& b) {
                if (a.year != b.year) return a.year > b.year;
                if (a.month != b.month) return a.month > b.month;
                return a.path.filename() < b.path.filename();
            });
        flushCallback(sorted);
        lastFlushCount = result.size();
    };

    for (const auto& dir : folders) {
        if (cancelFlag) break;

        if (!std::filesystem::exists(dir)) continue;

        OutputDebugStringW((L"[UIV] Scanning: " + dir.wstring() + L"\n").c_str());

        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(dir,
                 std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); ) {

            if (cancelFlag) break;

            try {
                const auto& entry = *it;

                // Skip certain directories
                if (entry.is_directory(ec)) {
                    auto dirName = entry.path().filename().wstring();
                    bool shouldSkip = false;
                    if (!dirName.empty() && dirName[0] == L'.') {
                        shouldSkip = true;
                    }
                    if (skipDirs.contains(dirName)) {
                        shouldSkip = true;
                    }
                    if (shouldSkip) {
                        it.disable_recursion_pending();
                    }
                } else if (entry.is_regular_file(ec)) {
                    auto ext = entry.path().extension().wstring();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

                    if (supportedExts.contains(ext)) {
                        // Deduplicate by lowercase path
                        std::wstring lowerPath = entry.path().wstring();
                        std::transform(lowerPath.begin(), lowerPath.end(),
                                       lowerPath.begin(), ::towlower);

                        if (!seen.contains(lowerPath)) {
                            seen.insert(lowerPath);

                            ScannedImage img;
                            img.path = entry.path();

                            // Get modification date via Win32 API
                            WIN32_FILE_ATTRIBUTE_DATA fad;
                            if (GetFileAttributesExW(img.path.c_str(),
                                                      GetFileExInfoStandard, &fad)) {
                                // Check file size — skip small files (icons, favicons, etc.)
                                ULONGLONG fileSize = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
                                if (fileSize < kMinImageSize) {
                                    std::error_code iterEc;
                                    it.increment(iterEc);
                                    continue;
                                }

                                SYSTEMTIME st;
                                FileTimeToSystemTime(&fad.ftLastWriteTime, &st);
                                img.year = st.wYear;
                                img.month = st.wMonth;
                            }

                            img.sourceFolder = dir;
                            result.push_back(std::move(img));
                            outCount = result.size();

                            // Flush every kFlushInterval new images
                            if (result.size() - lastFlushCount >= kFlushInterval) {
                                doFlush();
                            }
                        }
                    }
                }

                std::error_code iterEc;
                it.increment(iterEc);
            } catch (...) {
                // Skip entries that cause exceptions
                std::error_code iterEc;
                it.increment(iterEc);
            }
        }

        // Flush after each top-level folder (if new images were added)
        if (!cancelFlag && result.size() > lastFlushCount) {
            doFlush();
        }
    }

    if (cancelFlag) return result;

    // Sort by date descending (newest first)
    std::sort(result.begin(), result.end(),
        [](const ScannedImage& a, const ScannedImage& b) {
            if (a.year != b.year) return a.year > b.year;
            if (a.month != b.month) return a.month > b.month;
            return a.path.filename() < b.path.filename();
        });

    OutputDebugStringW((L"[UIV] Scan complete: " +
        std::to_wstring(result.size()) + L" images found\n").c_str());

    return result;
}

std::vector<ScannedImage> ImagePipeline::ScanSystemImages(
    std::atomic<bool>& cancelFlag,
    std::atomic<size_t>& outCount)
{
    // Resolve system known folders to paths, then delegate to ScanFolders
    struct FolderEntry {
        KNOWNFOLDERID id;
        const wchar_t* name;
    };
    FolderEntry knownFolders[] = {
        {FOLDERID_Pictures,       L"Pictures"},
        {FOLDERID_Desktop,        L"Desktop"},
        {FOLDERID_Downloads,      L"Downloads"},
        {FOLDERID_CameraRoll,     L"CameraRoll"},
        {FOLDERID_SavedPictures,  L"SavedPictures"},
    };

    std::vector<std::filesystem::path> folders;
    for (const auto& folder : knownFolders) {
        PWSTR folderPath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(folder.id, 0, nullptr, &folderPath))) {
            folders.emplace_back(folderPath);
            CoTaskMemFree(folderPath);
        }
    }

    return ScanFolders(folders, cancelFlag, outCount, nullptr);
}

bool ImagePipeline::HasThumbnail(const std::filesystem::path& path) const
{
    std::lock_guard lock(cacheMutex_);
    return thumbnailCache_.contains(path);
}

bool ImagePipeline::HasFullImage(const std::filesystem::path& path) const
{
    std::lock_guard lock(cacheMutex_);
    return fullImageCache_.contains(path);
}

Microsoft::WRL::ComPtr<ID2D1Bitmap> ImagePipeline::DecodeAndCreateBitmap(
    const std::filesystem::path& path)
{
    if (!decoder_ || !renderer_) return nullptr;

    auto image = decoder_->Decode(path, DecoderFlags::SIMD);
    if (!image || !image->data) return nullptr;

    auto bitmap = renderer_->CreateBitmap(
        image->info.width, image->info.height, image->data.get());

    return bitmap;
}

Microsoft::WRL::ComPtr<ID2D1Bitmap> ImagePipeline::DecodeAndCreateThumbnail(
    const std::filesystem::path& path, uint32_t maxSize)
{
    if (!decoder_ || !renderer_) return nullptr;

    auto image = decoder_->GenerateThumbnail(path, maxSize);
    if (!image || !image->data) {
        // Fall back to full decode
        image = decoder_->Decode(path, DecoderFlags::SIMD);
    }
    if (!image || !image->data) return nullptr;

    auto bitmap = renderer_->CreateBitmap(
        image->info.width, image->info.height, image->data.get());

    return bitmap;
}

void ImagePipeline::LoadThreadFunc()
{
    while (!shutdownRequested_) {
        LoadRequest req;
        {
            std::unique_lock lock(queueMutex_);
            queueCV_.wait(lock, [this] {
                return !loadQueue_.empty() || shutdownRequested_;
            });

            if (shutdownRequested_) break;
            if (loadQueue_.empty()) continue;

            req = std::move(loadQueue_.front());
            loadQueue_.pop();
        }

        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        if (req.isThumbnail) {
            bitmap = DecodeAndCreateThumbnail(req.path, req.maxSize);
            if (bitmap) {
                std::lock_guard lock(cacheMutex_);
                thumbnailCache_[req.path] = bitmap;
            }
        } else {
            bitmap = DecodeAndCreateBitmap(req.path);
            if (bitmap) {
                std::lock_guard lock(cacheMutex_);
                fullImageCache_[req.path] = bitmap;
            }
        }

        if (req.callback) {
            req.callback(bitmap);
        }
    }
}

} // namespace Core
} // namespace UltraImageViewer
