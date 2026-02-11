#include "core/ImagePipeline.hpp"
#include "ui/Theme.hpp"
#include <algorithm>
#include <set>
#include <unordered_set>
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <compressapi.h>
#pragma comment(lib, "cabinet.lib")

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
    threadPool_ = std::make_unique<ThreadPool>();  // auto thread count
}

void ImagePipeline::Shutdown()
{
    shutdownRequested_ = true;

    if (threadPool_) {
        threadPool_->PurgeAll();
    }
    threadPool_.reset();  // destructor joins all workers

    ClosePersistentMapping();

    {
        std::lock_guard lock(thumbSaveMutex_);
        thumbSaveBuffer_.clear();
    }

    std::lock_guard lock(cacheMutex_);
    thumbnailCache_.clear();
    thumbnailCacheBytes_ = 0;
    tier2Cache_.clear();
    tier2Bytes_ = 0;
    fullImageCache_.clear();
    pendingRequests_.clear();
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

    if (!threadPool_) return;

    auto pathCopy = path;
    threadPool_->Submit([this, pathCopy, cb = std::move(callback)] {
        auto bitmap = DecodeAndCreateBitmap(pathCopy);
        if (bitmap) {
            std::lock_guard lock(cacheMutex_);
            fullImageCache_[pathCopy] = bitmap;
        }
        if (cb) cb(bitmap);
    }, TaskPriority::Normal);
}

Microsoft::WRL::ComPtr<ID2D1Bitmap> ImagePipeline::GetThumbnail(const std::filesystem::path& path,
                                                                  uint32_t maxSize)
{
    {
        std::lock_guard lock(cacheMutex_);
        auto it = thumbnailCache_.find(path);
        if (it != thumbnailCache_.end()) {
            it->second.lastAccess = std::chrono::steady_clock::now();
            return it->second.bitmap;
        }
    }

    auto bitmap = DecodeAndCreateThumbnail(path, maxSize);
    if (bitmap) {
        auto bmpSize = bitmap->GetPixelSize();
        std::lock_guard lock(cacheMutex_);
        ThumbnailCacheEntry entry;
        entry.bitmap = bitmap;
        entry.width = bmpSize.width;
        entry.height = bmpSize.height;
        entry.lastAccess = std::chrono::steady_clock::now();
        thumbnailCacheBytes_ += static_cast<size_t>(bmpSize.width) * bmpSize.height * 4;
        thumbnailCache_[path] = std::move(entry);
    }
    return bitmap;
}

void ImagePipeline::PrefetchAround(const std::vector<std::filesystem::path>& allPaths,
                                    size_t currentIndex, size_t radius)
{
    if (allPaths.empty() || !threadPool_) return;

    uint64_t gen = generation_.load();
    std::vector<std::function<void()>> batch;

    for (size_t offset = 1; offset <= radius; ++offset) {
        // Forward
        if (currentIndex + offset < allPaths.size()) {
            auto p = allPaths[currentIndex + offset];
            if (!HasThumbnail(p)) {
                batch.push_back([this, p, gen] {
                    ThumbnailDecodeTask(p, 256, gen);
                });
            }
        }
        // Backward
        if (currentIndex >= offset) {
            auto p = allPaths[currentIndex - offset];
            if (!HasThumbnail(p)) {
                batch.push_back([this, p, gen] {
                    ThumbnailDecodeTask(p, 256, gen);
                });
            }
        }
    }

    if (!batch.empty()) {
        threadPool_->SubmitBatch(batch, TaskPriority::Low);
    }
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

Microsoft::WRL::ComPtr<ID2D1Bitmap> ImagePipeline::GetCachedThumbnail(
    const std::filesystem::path& path)
{
    // Check GPU cache first
    {
        std::lock_guard lock(cacheMutex_);
        auto it = thumbnailCache_.find(path);
        if (it != thumbnailCache_.end()) {
            it->second.lastAccess = std::chrono::steady_clock::now();
            return it->second.bitmap;
        }
    }

    // Fall through to persistent disk cache (even during fast scroll)
    if (persistSyncBudget_ > 0 && renderer_) {
        uint16_t w = 0, h = 0;
        const uint8_t* pixelPtr = nullptr;
        {
            std::shared_lock plock(persistMutex_);
            auto it = persistIndex_.find(path);
            if (it != persistIndex_.end()) {
                w = it->second.width;
                h = it->second.height;
                pixelPtr = it->second.pixelData;
            }
        }
        if (pixelPtr && w > 0 && h > 0) {
            auto bitmap = renderer_->CreateBitmap(w, h, pixelPtr);
            if (bitmap) {
                --persistSyncBudget_;
                std::lock_guard lock(cacheMutex_);
                ThumbnailCacheEntry entry;
                entry.bitmap = bitmap;
                entry.width = w;
                entry.height = h;
                entry.lastAccess = std::chrono::steady_clock::now();
                thumbnailCacheBytes_ += static_cast<size_t>(w) * h * 4;
                thumbnailCache_[path] = std::move(entry);
                return bitmap;
            }
        }
    }

    return nullptr;
}

bool ImagePipeline::HasThumbnail(const std::filesystem::path& path) const
{
    std::lock_guard lock(cacheMutex_);
    auto it = thumbnailCache_.find(path);
    return it != thumbnailCache_.end() && it->second.bitmap;
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

// --- Async Thumbnail Pipeline Implementation ---

Microsoft::WRL::ComPtr<ID2D1Bitmap> ImagePipeline::RequestThumbnail(
    const std::filesystem::path& path, uint32_t targetSize)
{
    // Check in-memory cache + pending dedup under single lock
    {
        std::lock_guard lock(cacheMutex_);
        auto it = thumbnailCache_.find(path);
        if (it != thumbnailCache_.end()) {
            it->second.lastAccess = std::chrono::steady_clock::now();
            return it->second.bitmap;
        }
    }

    // Synchronous path: create D2D bitmap directly from persistent cache
    // on the render thread. Zero-frame latency — identical to iOS behavior.
    if (persistSyncBudget_ > 0 && renderer_) {
        uint16_t w = 0, h = 0;
        const uint8_t* pixelPtr = nullptr;

        {
            std::shared_lock plock(persistMutex_);
            auto it = persistIndex_.find(path);
            if (it != persistIndex_.end()) {
                w = it->second.width;
                h = it->second.height;
                pixelPtr = it->second.pixelData;
            }
        }

        if (pixelPtr && w > 0 && h > 0) {
            auto bitmap = renderer_->CreateBitmap(w, h, pixelPtr);
            if (bitmap) {
                --persistSyncBudget_;
                std::lock_guard lock(cacheMutex_);
                ThumbnailCacheEntry entry;
                entry.bitmap = bitmap;
                entry.width = w;
                entry.height = h;
                entry.lastAccess = std::chrono::steady_clock::now();
                thumbnailCacheBytes_ += static_cast<size_t>(w) * h * 4;
                thumbnailCache_[path] = std::move(entry);
                return bitmap;
            }
        }
    }

    if (!threadPool_) return nullptr;

    // Queue a decode request if not already pending (single mutex path)
    uint64_t gen = generation_.load();
    bool isVis = false;
    {
        std::lock_guard lock(cacheMutex_);
        auto pendIt = pendingRequests_.find(path);
        if (pendIt != pendingRequests_.end() && pendIt->second == gen) {
            return nullptr;  // already pending
        }
        isVis = visiblePaths_.contains(path);
        pendingRequests_[path] = gen;
    }

    auto pathCopy = path;
    if (isVis) {
        threadPool_->SubmitFront([this, pathCopy, targetSize, gen] {
            ThumbnailDecodeTask(pathCopy, targetSize, gen);
        }, TaskPriority::High);
    } else {
        threadPool_->Submit([this, pathCopy, targetSize, gen] {
            ThumbnailDecodeTask(pathCopy, targetSize, gen);
        }, TaskPriority::Normal);
    }

    return nullptr;  // Not ready yet
}

int ImagePipeline::FlushReadyThumbnails(int maxCount)
{
    // Reset per-frame budget for synchronous persistent cache loads
    persistSyncBudget_ = UI::Theme::PersistSyncBudgetPerFrame;

    std::vector<ReadyThumbnail> batch;
    {
        std::lock_guard lock(readyMutex_);
        int count = std::min(maxCount, static_cast<int>(readyQueue_.size()));
        if (count == 0) return 0;

        batch.reserve(count);
        for (int i = 0; i < count; ++i) {
            batch.push_back(std::move(readyQueue_.front()));
            readyQueue_.pop_front();  // O(1) deque pop vs O(n) vector erase
        }
    }

    int created = 0;
    for (auto& ready : batch) {
        if (!renderer_ || !ready.pixels || ready.width == 0 || ready.height == 0) continue;

        // Create D2D bitmap (copies pixels to GPU internally)
        auto bitmap = renderer_->CreateBitmap(ready.width, ready.height, ready.pixels.get());
        if (bitmap) {
            // Save raw pixels for persistent cache AFTER GPU copy, BEFORE moving
            {
                std::lock_guard lock(thumbSaveMutex_);
                if (!thumbSaveBuffer_.contains(ready.path)) {
                    ThumbSaveEntry save;
                    save.width = static_cast<uint16_t>(ready.width);
                    save.height = static_cast<uint16_t>(ready.height);
                    save.pixelSize = ready.width * ready.height * 4;
                    save.pixels = std::move(ready.pixels);  // zero-copy transfer
                    thumbSaveBuffer_[ready.path] = std::move(save);
                }
            }

            std::lock_guard lock(cacheMutex_);
            ThumbnailCacheEntry entry;
            entry.bitmap = bitmap;
            entry.width = ready.width;
            entry.height = ready.height;
            entry.lastAccess = std::chrono::steady_clock::now();
            thumbnailCacheBytes_ += static_cast<size_t>(ready.width) * ready.height * 4;
            thumbnailCache_[ready.path] = std::move(entry);
            ++created;
        }
    }

    // Evict if over budget
    if (created > 0) {
        EvictThumbnailsIfNeeded();
    }

    return created;
}

void ImagePipeline::InvalidateRequests()
{
    generation_.fetch_add(1);

    // Purge non-high-priority pending tasks from the thread pool
    if (threadPool_) {
        threadPool_->PurgePriority(TaskPriority::Normal);
        threadPool_->PurgePriority(TaskPriority::Low);
    }

    // Clear pending tracking so new requests can be queued
    std::lock_guard lock(cacheMutex_);
    pendingRequests_.clear();
}

void ImagePipeline::SetVisibleRange(const std::vector<std::filesystem::path>& paths)
{
    std::lock_guard lock(cacheMutex_);
    visiblePaths_.clear();
    for (const auto& p : paths) {
        visiblePaths_[p] = true;
    }
}

bool ImagePipeline::HasPendingThumbnails() const
{
    std::lock_guard lock(readyMutex_);
    return !readyQueue_.empty();
}

void ImagePipeline::ThumbnailDecodeTask(const std::filesystem::path& path,
                                         uint32_t targetSize, uint64_t generation)
{
    // Check generation — skip stale requests
    if (generation < generation_.load()) return;

    // Check if already cached (another worker may have finished it)
    {
        std::lock_guard lock(cacheMutex_);
        if (thumbnailCache_.contains(path)) return;
    }

    // I/O priority: Low-priority (prefetch) tasks enter background mode,
    // reducing both I/O and memory priority so they don't compete with
    // visible thumbnail decodes for disk bandwidth.
    bool lowIoPriority = (ThreadPool::CurrentLane() == static_cast<int>(TaskPriority::Low));
    if (lowIoPriority) {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    }

    // Tier 2: check CPU-RAM compressed cache first (~0.3ms decompress vs ~5ms disk)
    std::unique_ptr<uint8_t[]> pixels;
    uint32_t imgWidth = 0, imgHeight = 0;

    {
        std::lock_guard lock(cacheMutex_);
        auto t2it = tier2Cache_.find(path);
        if (t2it != tier2Cache_.end()) {
            imgWidth = t2it->second.width;
            imgHeight = t2it->second.height;
            uint32_t rawSize = t2it->second.rawSize;
            pixels = std::make_unique<uint8_t[]>(rawSize);
            if (DecompressPixels(t2it->second.data.get(), t2it->second.compressedSize,
                                 pixels.get(), rawSize)) {
                // Hit — remove from Tier 2 (will be promoted back to Tier 1)
                tier2Bytes_ -= t2it->second.compressedSize;
                tier2Cache_.erase(t2it);
            } else {
                pixels.reset();
                imgWidth = imgHeight = 0;
            }
        }
    }

    // Tier 3: try persistent thumbnail cache (memcpy vs JPEG decode = 100x faster)
    if (!pixels) {
        std::shared_lock plock(persistMutex_);
        auto it = persistIndex_.find(path);
        if (it != persistIndex_.end()) {
            imgWidth = it->second.width;
            imgHeight = it->second.height;
            uint32_t pixelSize = imgWidth * imgHeight * 4;
            pixels = std::make_unique<uint8_t[]>(pixelSize);
            memcpy(pixels.get(), it->second.pixelData, pixelSize);
        }
    }

    // Fall back to JPEG decode if not in persistent cache
    if (!pixels) {
        if (!decoder_) {
            if (lowIoPriority) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
            return;
        }

        auto image = decoder_->GenerateThumbnail(path, targetSize);
        if (!image || !image->data) {
            image = decoder_->Decode(path, DecoderFlags::SIMD);
        }
        if (!image || !image->data) {
            if (lowIoPriority) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
            return;
        }

        pixels = std::move(image->data);
        imgWidth = image->info.width;
        imgHeight = image->info.height;
    }

    // End background I/O mode before pushing to ready queue
    if (lowIoPriority) {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    }

    // Check generation again after decode
    if (generation < generation_.load()) return;

    // Push to ready queue for render thread to create D2D bitmap
    ReadyThumbnail ready;
    ready.path = path;
    ready.pixels = std::move(pixels);
    ready.width = imgWidth;
    ready.height = imgHeight;

    {
        std::lock_guard lock(readyMutex_);
        readyQueue_.push_back(std::move(ready));
    }
}

// --- Tier 2 compressed cache: compress/decompress helpers ---

bool ImagePipeline::CompressPixels(const uint8_t* src, uint32_t srcSize,
                                    std::unique_ptr<uint8_t[]>& outBuf, size_t& outSize)
{
    COMPRESSOR_HANDLE compressor = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &compressor))
        return false;

    SIZE_T compressedSize = 0;
    Compress(compressor, src, srcSize, nullptr, 0, &compressedSize);
    if (compressedSize == 0) {
        CloseCompressor(compressor);
        return false;
    }

    outBuf = std::make_unique<uint8_t[]>(compressedSize);
    BOOL ok = Compress(compressor, src, srcSize, outBuf.get(), compressedSize, &compressedSize);
    CloseCompressor(compressor);

    if (!ok) return false;
    outSize = static_cast<size_t>(compressedSize);
    return true;
}

bool ImagePipeline::DecompressPixels(const uint8_t* src, size_t srcSize,
                                      uint8_t* dst, uint32_t dstSize)
{
    DECOMPRESSOR_HANDLE decompressor = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &decompressor))
        return false;

    SIZE_T decompressedSize = 0;
    BOOL ok = Decompress(decompressor, src, srcSize, dst, dstSize, &decompressedSize);
    CloseDecompressor(decompressor);
    return ok && decompressedSize == dstSize;
}

void ImagePipeline::EvictThumbnailsIfNeeded()
{
    std::lock_guard lock(cacheMutex_);

    if (thumbnailCacheBytes_ <= UI::Theme::ThumbnailCacheMaxBytes) return;

    // Build a list sorted by last access time (oldest first)
    struct EvictCandidate {
        std::filesystem::path path;
        std::chrono::steady_clock::time_point lastAccess;
        size_t bytes;
        uint32_t width;
        uint32_t height;
    };

    std::vector<EvictCandidate> candidates;
    candidates.reserve(thumbnailCache_.size());
    for (const auto& [path, entry] : thumbnailCache_) {
        // Never evict visible thumbnails
        if (visiblePaths_.contains(path)) continue;
        size_t bytes = static_cast<size_t>(entry.width) * entry.height * 4;
        candidates.push_back({path, entry.lastAccess, bytes, entry.width, entry.height});
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const EvictCandidate& a, const EvictCandidate& b) {
            return a.lastAccess < b.lastAccess;
        });

    // Collect evicted bitmaps for Tier 2 demotion (readback pixels before releasing)
    struct DemoteEntry {
        std::filesystem::path path;
        uint32_t width, height;
        size_t rawBytes;
    };
    std::vector<DemoteEntry> demoteList;

    // Evict to 75% of budget to avoid thrashing
    size_t targetBytes = UI::Theme::ThumbnailCacheMaxBytes * 3 / 4;
    for (const auto& c : candidates) {
        if (thumbnailCacheBytes_ <= targetBytes) break;

        // Try to demote to Tier 2 (if not already there and Tier 2 has space)
        if (!tier2Cache_.contains(c.path) && tier2Bytes_ < kTier2MaxBytes) {
            demoteList.push_back({c.path, c.width, c.height, c.bytes});
        }

        thumbnailCache_.erase(c.path);
        if (thumbnailCacheBytes_ >= c.bytes) {
            thumbnailCacheBytes_ -= c.bytes;
        } else {
            thumbnailCacheBytes_ = 0;
        }
    }

    // Tier 2 demotion: read pixel data back from D2D bitmaps and compress.
    // Note: D2D1Bitmap doesn't support CPU readback directly, so we use the
    // thumbSaveBuffer_ which already has raw pixels from FlushReadyThumbnails.
    {
        std::lock_guard saveLock(thumbSaveMutex_);
        for (const auto& d : demoteList) {
            auto saveIt = thumbSaveBuffer_.find(d.path);
            if (saveIt == thumbSaveBuffer_.end() || !saveIt->second.pixels) continue;

            uint32_t rawSize = d.width * d.height * 4;
            std::unique_ptr<uint8_t[]> compressed;
            size_t compressedSize = 0;

            if (CompressPixels(saveIt->second.pixels.get(), rawSize, compressed, compressedSize)) {
                CompressedThumbnail ct;
                ct.data = std::move(compressed);
                ct.compressedSize = compressedSize;
                ct.rawSize = rawSize;
                ct.width = static_cast<uint16_t>(d.width);
                ct.height = static_cast<uint16_t>(d.height);
                tier2Bytes_ += compressedSize;
                tier2Cache_[d.path] = std::move(ct);
            }
        }
    }
}

// --- Persistent thumbnail cache (memory-mapped binary file) ---
//
// File format: sequential variable-size entries
//   Header (32 bytes): "UIVT" + version(4) + entry_count(4) + reserved(20)
//   Per entry: path_len(2) + width(2) + height(2) + reserved(2) + path(wchar_t[]) + pixels(BGRA[])

void ImagePipeline::ClosePersistentMapping()
{
    std::unique_lock plock(persistMutex_);
    persistIndex_.clear();

    if (persistData_) {
        UnmapViewOfFile(persistData_);
        persistData_ = nullptr;
    }
    if (persistMapH_) {
        CloseHandle(static_cast<HANDLE>(persistMapH_));
        persistMapH_ = nullptr;
    }
    if (persistFileH_) {
        CloseHandle(static_cast<HANDLE>(persistFileH_));
        persistFileH_ = nullptr;
    }
    persistSize_ = 0;
}

void ImagePipeline::LoadPersistentThumbs(const std::filesystem::path& cachePath)
{
    std::error_code ec;
    if (!std::filesystem::exists(cachePath, ec)) return;

    HANDLE hFile = CreateFileW(cachePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart < 32) {
        CloseHandle(hFile);
        return;
    }

    HANDLE hMapping = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMapping) {
        CloseHandle(hFile);
        return;
    }

    const uint8_t* data = static_cast<const uint8_t*>(
        MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0));
    if (!data) {
        CloseHandle(hMapping);
        CloseHandle(hFile);
        return;
    }

    size_t size = static_cast<size_t>(fileSize.QuadPart);

    // Validate header
    if (memcmp(data, "UIVT", 4) != 0) {
        UnmapViewOfFile(data);
        CloseHandle(hMapping);
        CloseHandle(hFile);
        return;
    }

    uint32_t version, entryCount;
    memcpy(&version, data + 4, 4);
    memcpy(&entryCount, data + 8, 4);
    if (version != 1) {
        UnmapViewOfFile(data);
        CloseHandle(hMapping);
        CloseHandle(hFile);
        return;
    }

    // Parse sequential entries and build index
    std::unique_lock plock(persistMutex_);

    size_t offset = 32;
    for (uint32_t i = 0; i < entryCount; ++i) {
        if (offset + 8 > size) break;

        uint16_t pathLen, w, h, reserved;
        memcpy(&pathLen, data + offset, 2);
        memcpy(&w, data + offset + 2, 2);
        memcpy(&h, data + offset + 4, 2);
        memcpy(&reserved, data + offset + 6, 2);
        offset += 8;

        size_t pathBytes = static_cast<size_t>(pathLen) * sizeof(wchar_t);
        if (offset + pathBytes > size) break;

        const wchar_t* pathChars = reinterpret_cast<const wchar_t*>(data + offset);
        std::filesystem::path path(std::wstring(pathChars, pathLen));
        offset += pathBytes;

        uint32_t pixelSize = static_cast<uint32_t>(w) * h * 4;
        if (offset + pixelSize > size) break;

        PersistThumbInfo info;
        info.pixelData = data + offset;
        info.width = w;
        info.height = h;
        persistIndex_[std::move(path)] = info;

        offset += pixelSize;
    }

    persistFileH_ = hFile;
    persistMapH_ = hMapping;
    persistData_ = data;
    persistSize_ = size;

    OutputDebugStringA(("Loaded persistent thumb cache: " +
        std::to_string(persistIndex_.size()) + " entries\n").c_str());
}

void ImagePipeline::SavePersistentThumbs(const std::filesystem::path& cachePath)
{
    // Snapshot the save buffer (newly decoded this session)
    std::unordered_map<std::filesystem::path, ThumbSaveEntry> saveBuffer;
    {
        std::lock_guard lock(thumbSaveMutex_);
        saveBuffer = std::move(thumbSaveBuffer_);
        thumbSaveBuffer_.clear();
    }

    // Collect old persistent entries not already in save buffer
    struct OldEntry {
        std::filesystem::path path;
        PersistThumbInfo info;
    };
    std::vector<OldEntry> oldEntries;
    {
        std::shared_lock plock(persistMutex_);
        for (const auto& [path, info] : persistIndex_) {
            if (!saveBuffer.contains(path)) {
                oldEntries.push_back({path, info});
            }
        }
    }

    uint32_t totalEntries = static_cast<uint32_t>(saveBuffer.size() + oldEntries.size());
    if (totalEntries == 0) return;

    // Write to .tmp file
    auto tmpPath = cachePath.wstring() + L".tmp";

    FILE* f = _wfopen(tmpPath.c_str(), L"wb");
    if (!f) return;

    // Header
    uint8_t header[32] = {};
    memcpy(header, "UIVT", 4);
    uint32_t version = 1;
    memcpy(header + 4, &version, 4);
    memcpy(header + 8, &totalEntries, 4);
    fwrite(header, 1, 32, f);

    // Helper: write one entry
    auto writeEntry = [&](const std::filesystem::path& path, uint16_t w, uint16_t h,
                          const uint8_t* pixels) {
        std::wstring pathStr = path.wstring();
        uint16_t pathLen = static_cast<uint16_t>(pathStr.size());
        uint16_t reserved = 0;
        fwrite(&pathLen, 2, 1, f);
        fwrite(&w, 2, 1, f);
        fwrite(&h, 2, 1, f);
        fwrite(&reserved, 2, 1, f);
        fwrite(pathStr.data(), sizeof(wchar_t), pathLen, f);
        fwrite(pixels, 1, static_cast<size_t>(w) * h * 4, f);
    };

    // Write new/updated entries from save buffer
    for (const auto& [path, entry] : saveBuffer) {
        if (entry.pixels) {
            writeEntry(path, entry.width, entry.height, entry.pixels.get());
        }
    }

    // Write old entries (still valid, from previous persistent cache)
    for (const auto& old : oldEntries) {
        writeEntry(old.path, old.info.width, old.info.height, old.info.pixelData);
    }

    fclose(f);

    // Close old memory mapping (releases file handles)
    ClosePersistentMapping();

    // Atomically replace old cache file
    MoveFileExW(tmpPath.c_str(), cachePath.c_str(), MOVEFILE_REPLACE_EXISTING);

    // Reload the new file so persistIndex_ stays populated for the rest of the session.
    // Without this, thumbnails evicted from GPU LRU require full JPEG decode again.
    LoadPersistentThumbs(cachePath);

    OutputDebugStringA(("Saved persistent thumb cache: " +
        std::to_string(totalEntries) + " entries\n").c_str());
}

} // namespace Core
} // namespace UltraImageViewer
