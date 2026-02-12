#include "core/Application.hpp"
#include "core/SimdUtils.hpp"

#include <ShellScalingApi.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <stdexcept>
#include <algorithm>
#include <d2d1helper.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <fstream>
#include <set>
#include <unordered_set>
#include <wrl/client.h>

#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "dwmapi.lib")

// Debug log to file next to exe
namespace {
void DebugLog(const char* msg) {
    static FILE* f = nullptr;
    if (!f) {
        f = fopen("debug_log.txt", "w");
    }
    if (f) {
        fprintf(f, "%s\n", msg);
        fflush(f);
    }
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}
void DebugLogHR(const char* msg, HRESULT hr) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s (HRESULT=0x%08lX)", msg, static_cast<unsigned long>(hr));
    DebugLog(buf);
}
} // namespace

namespace UltraImageViewer {
namespace Core {

Application* Application::s_instance = nullptr;

Application::Application()
{
    if (s_instance != nullptr) {
        throw std::runtime_error("Application already exists");
    }
    s_instance = this;
}

Application::~Application()
{
    Shutdown();
    s_instance = nullptr;
}

bool Application::Initialize(HINSTANCE hInstance)
{
    hInstance_ = hInstance;
    Simd::DetectFeatures();
    DebugLog("=== Shiguang starting ===");

    if (!InitializeWindow()) {
        DebugLog("FAIL: InitializeWindow");
        return false;
    }
    DebugLog("OK: InitializeWindow");

    if (!InitializeComponents()) {
        DebugLog("FAIL: InitializeComponents");
        return false;
    }
    DebugLog("OK: InitializeComponents");

    isInitialized_ = true;
    DebugLog("OK: Initialization complete");
    return true;
}

void Application::Shutdown()
{
    if (!isInitialized_) {
        return;
    }
    SaveRecents();
    SaveAlbumFolders();
    SaveFolderProfiles();

    // Wait for any in-flight persistent thumbnail load
    if (persistLoadThread_.joinable()) {
        persistLoadThread_.join();
    }

    // Wait for any in-flight persistent thumbnail save
    if (thumbSaveThread_.joinable()) {
        thumbSaveThread_.join();
    }

    // Final save of persistent thumbnail cache (captures thumbnails decoded since last scan)
    if (pipeline_) {
        auto thumbPath = GetScanCachePath().parent_path() / L"scan_thumbs.bin";
        pipeline_->SavePersistentThumbs(thumbPath);
    }

    // Cancel any ongoing scan
    scanCancelled_ = true;
    if (scanThread_.joinable()) {
        scanThread_.request_stop();
        scanThread_.join();
    }

    // Shutdown order matters
    if (pipeline_) pipeline_->Shutdown();
    viewManager_.reset();
    animEngine_.reset();
    pipeline_.reset();
    renderer_.reset();
    cache_.reset();
    decoder_.reset();

    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    isInitialized_ = false;
}

int Application::Run(int nCmdShow)
{
    if (!isInitialized_) {
        return 1;
    }
    if (!hwnd_ || !IsWindow(hwnd_)) {
        MessageBoxW(nullptr, L"Main window handle is invalid.", L"UltraImageViewer", MB_ICONERROR | MB_OK);
        return 1;
    }
    if (nCmdShow == SW_HIDE) {
        nCmdShow = SW_SHOWNORMAL;
    }
    DebugLog("Run: ShowWindow");
    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);

    // Auto-scan: merge album folders + system defaults, progressive display
    if (currentImages_.empty()) {
        LoadAlbumFolders();
        LoadHiddenAlbums();
        LoadFolderProfiles();

        // Load persistent thumbnail cache asynchronously (non-blocking first frame)
        if (pipeline_) {
            auto thumbPath = GetScanCachePath().parent_path() / L"scan_thumbs.bin";
            persistLoadThread_ = std::jthread([this, thumbPath](std::stop_token) {
                pipeline_->LoadPersistentThumbs(thumbPath);
            });
        }

        // Load cached scan results for instant display
        auto cached = LoadScanCache();
        if (!cached.empty()) {
            FilterHiddenAlbums(cached);
            DebugLog(("Loaded " + std::to_string(cached.size()) + " cached images (filtered)").c_str());
            if (viewManager_) {
                viewManager_->GetGalleryView()->SetImagesGrouped(cached);
            }
            currentImages_.clear();
            currentImages_.reserve(cached.size());
            for (const auto& img : cached) {
                currentImages_.push_back(img.path);
            }
            std::wstring title = windowTitle_ + L" - " +
                std::to_wstring(cached.size()) + L" photos";
            SetWindowTextW(hwnd_, title.c_str());
        }

        // Start background rescan (will replace cached data when done)
        StartFullScan();
    }

    QueryPerformanceFrequency(&perfFrequency_);
    QueryPerformanceCounter(&lastFrameTime_);

    DebugLog("Run: entering game loop");
    // Game-loop style message loop
    MSG msg = {};
    bool running = true;
    while (running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!running) break;

        // Calculate delta time
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - lastFrameTime_.QuadPart) /
                   static_cast<float>(perfFrequency_.QuadPart);
        lastFrameTime_ = now;
        dt = std::min(dt, 0.05f);  // Cap at 50ms to prevent large jumps

        // Update
        if (animEngine_) animEngine_->Update(dt);
        if (viewManager_) viewManager_->Update(dt);

        // Check scan progress and push results to gallery
        CheckScanProgress();

        // Render only when needed
        bool hasAnimations = animEngine_ && animEngine_->HasActiveAnimations();
        bool viewNeedsRender = viewManager_ && viewManager_->NeedsRender();

        // Scanning: throttle to ~60fps for the blue bar animation instead of 100% CPU spin
        bool scanAnimFrame = false;
        if (isScanning_) {
            LARGE_INTEGER scanNow;
            QueryPerformanceCounter(&scanNow);
            double msSinceLast = static_cast<double>(scanNow.QuadPart - lastScanRender_.QuadPart)
                                 / static_cast<double>(perfFrequency_.QuadPart) * 1000.0;
            if (msSinceLast >= 16.0) {
                scanAnimFrame = true;
                lastScanRender_ = scanNow;
            }
        }

        if (needsRender_ || hasAnimations || viewNeedsRender || scanAnimFrame) {
            try {
                Render();
            } catch (const std::exception& e) {
                DebugLog(("Render exception: " + std::string(e.what())).c_str());
            } catch (...) {
                DebugLog("Render unknown exception");
            }
            needsRender_ = false;
        } else {
            Sleep(1);  // Yield CPU when idle
        }
    }

    return static_cast<int>(msg.wParam);
}

void Application::StartFullScan()
{
    DebugLog("Starting full scan (album + system folders)...");
    isScanning_ = true;
    scanCancelled_ = false;
    scanProgress_ = 0;
    scanDirty_ = false;
    lastGalleryUpdateCount_ = 0;
    lastDisplayedScanCount_ = 0;

    // Update gallery scanning state
    if (viewManager_) {
        viewManager_->GetGalleryView()->SetScanningState(true, 0);
    }

    // Merge folder lists: album folders + system known folders
    auto folders = albumFolders_;  // copy

    // Append system known folders
    for (const auto& id : {FOLDERID_Pictures, FOLDERID_Desktop, FOLDERID_Downloads,
                            FOLDERID_CameraRoll, FOLDERID_SavedPictures}) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) {
            folders.emplace_back(p);
            CoTaskMemFree(p);
        }
    }

    // Deduplicate by canonical path
    {
        std::vector<std::filesystem::path> unique;
        std::set<std::wstring> seen;
        for (auto& f : folders) {
            std::error_code ec;
            auto canonical = std::filesystem::canonical(f, ec);
            if (ec) canonical = f;  // fallback if canonical fails
            std::wstring key = canonical.wstring();
            Simd::ToLowerInPlace(key);
            if (!seen.contains(key)) {
                seen.insert(key);
                unique.push_back(std::move(f));
            }
        }
        folders = std::move(unique);
    }

    // Reorder folders by access profile: frequently visited folders are scanned
    // first so their thumbnails appear sooner (Ledger-inspired prefetch).
    if (!folderProfiles_.empty()) {
        std::unordered_map<std::wstring, uint32_t> visitMap;
        for (const auto& fp : folderProfiles_) {
            std::wstring key = fp.folder.wstring();
            Simd::ToLowerInPlace(key);
            visitMap[key] = fp.visitCount;
        }
        std::stable_sort(folders.begin(), folders.end(),
            [&visitMap](const std::filesystem::path& a, const std::filesystem::path& b) {
                std::wstring ka = a.wstring(), kb = b.wstring();
                Simd::ToLowerInPlace(ka);
                Simd::ToLowerInPlace(kb);
                uint32_t va = 0, vb = 0;
                auto ia = visitMap.find(ka); if (ia != visitMap.end()) va = ia->second;
                auto ib = visitMap.find(kb); if (ib != visitMap.end()) vb = ib->second;
                return va > vb;
            });
    }

    DebugLog(("Full scan: " + std::to_string(folders.size()) + " folders").c_str());

    scanThread_ = std::jthread([this, folders = std::move(folders)](std::stop_token) {
        // COM must be initialized on this thread for SHGetKnownFolderPath
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        // Lower scan thread priority so rendering stays smooth
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        try {
            // No intermediate flush — gallery is frozen during scan, one-shot rebuild at end
            auto results = ImagePipeline::ScanFolders(
                folders, scanCancelled_, scanProgress_, nullptr);

            DebugLog(("Scan found " + std::to_string(results.size()) + " images").c_str());

            // Final results (already sorted by ScanFolders)
            {
                std::lock_guard lock(scanMutex_);
                scannedResults_ = std::move(results);
            }
            // Set isScanning_ BEFORE scanDirty_ to avoid race:
            // CheckScanProgress must see isScanning_==false when it consumes scanDirty_,
            // otherwise the finalization (SaveScanCache, SetScanningState) is permanently skipped.
            isScanning_ = false;
            scanDirty_ = true;
        } catch (const std::exception& e) {
            DebugLog(("Scan exception: " + std::string(e.what())).c_str());
            isScanning_ = false;
        } catch (...) {
            DebugLog("Scan unknown exception");
            isScanning_ = false;
        }
        CoUninitialize();
    });
}

void Application::CheckScanProgress()
{
    if (!viewManager_) return;

    auto* gallery = viewManager_->GetGalleryView();

    // --- During scan: only update the progress counter for the blue bar animation ---
    if (isScanning_) {
        size_t current = scanProgress_.load();
        if (current != lastDisplayedScanCount_) {
            gallery->SetScanningState(true, current);
            lastDisplayedScanCount_ = current;
            needsRender_ = true;
        }
        return;  // Do NOT touch gallery data until scan completes
    }

    // --- ONE-SHOT rebuild when scan finishes ---
    if (scanDirty_.exchange(false)) {
        std::vector<ScannedImage> results;
        {
            std::lock_guard lock(scanMutex_);
            results = std::move(scannedResults_);
        }

        // Filter out user-hidden albums before display
        FilterHiddenAlbums(results);

        gallery->SetScanningState(false, results.size());
        SaveScanCache(results);  // Persist filtered paths for next launch

        // Auto-clear manual open when scan completes (gallery is fully restored)
        inManualOpen_ = false;
        gallery->SetManualOpenMode(false);

        // Save persistent thumbnail cache in background thread (non-blocking)
        if (pipeline_) {
            auto thumbPath = GetScanCachePath().parent_path() / L"scan_thumbs.bin";
            if (thumbSaveThread_.joinable()) {
                if (thumbSaveDone_.load()) {
                    thumbSaveThread_.join();
                } else {
                    // Previous save still running — skip this time; shutdown will join
                    goto skipThumbSave;
                }
            }
            thumbSaveDone_ = false;
            thumbSaveThread_ = std::jthread([this, thumbPath](std::stop_token) {
                pipeline_->SavePersistentThumbs(thumbPath);
                thumbSaveDone_ = true;
            });
            skipThumbSave:;
        }

        gallery->SetImagesGrouped(results);

        // Update flat image list for viewer compatibility
        currentImages_.clear();
        currentImages_.reserve(results.size());
        for (const auto& img : results) {
            currentImages_.push_back(img.path);
        }

        lastGalleryUpdateCount_ = results.size();

        std::wstring title = windowTitle_ + L" - " +
            std::to_wstring(results.size()) + L" photos";
        SetWindowTextW(hwnd_, title.c_str());

        needsRender_ = true;
    }
}

bool Application::InitializeWindow()
{
    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WindowProc;
    wcex.hInstance = hInstance_;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = nullptr;  // No GDI background - D2D handles everything
    wcex.lpszClassName = L"UltraImageViewerWindowClass";

    if (!RegisterClassExW(&wcex)) {
        return false;
    }

    RECT rc = { 0, 0, static_cast<LONG>(windowWidth_), static_cast<LONG>(windowHeight_) };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    hwnd_ = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP,  // Required for DirectComposition
        L"UltraImageViewerWindowClass",
        windowTitle_.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rc.right - rc.left,
        rc.bottom - rc.top,
        nullptr,
        nullptr,
        hInstance_,
        this
    );

    if (!hwnd_) {
        return false;
    }

    // Enable drag and drop
    DragAcceptFiles(hwnd_, TRUE);

    // Dark title bar
    SetDarkTitleBar();

    return true;
}

void Application::SetDarkTitleBar()
{
    BOOL useDarkMode = TRUE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE = 20
    DwmSetWindowAttribute(hwnd_, 20, &useDarkMode, sizeof(useDarkMode));
}

bool Application::InitializeComponents()
{
    if (!InitializeDecoder()) {
        MessageBoxW(hwnd_, L"Failed to initialize image decoder.", L"UltraImageViewer", MB_ICONERROR);
        return false;
    }

    if (!InitializeCache()) {
        MessageBoxW(hwnd_, L"Failed to initialize cache.", L"UltraImageViewer", MB_ICONERROR);
        return false;
    }

    if (!InitializeRenderer()) {
        MessageBoxW(hwnd_, L"Failed to initialize Direct2D renderer.", L"UltraImageViewer", MB_ICONERROR);
        return false;
    }

    // Sync DPI scale from renderer (physical pixels → DIPs conversion factor)
    dpiScale_ = renderer_->GetDpiX() / 96.0f;

    // Create animation engine
    animEngine_ = std::make_unique<Animation::AnimationEngine>();

    // Create image pipeline
    pipeline_ = std::make_unique<ImagePipeline>();
    pipeline_->Initialize(decoder_.get(), cache_.get(), renderer_.get());

    // Create view manager
    viewManager_ = std::make_unique<UI::ViewManager>();
    viewManager_->Initialize(renderer_.get(), animEngine_.get(), pipeline_.get());
    viewManager_->SetViewSize(static_cast<float>(windowWidth_) / dpiScale_,
                               static_cast<float>(windowHeight_) / dpiScale_);

    // Wire up back-to-library callback for manual open mode
    viewManager_->GetGalleryView()->SetBackToLibraryCallback([this]() {
        RestoreScanGallery();
    });

    // Wire up album edit mode callbacks
    viewManager_->GetGalleryView()->SetDeleteAlbumCallback([this](const auto& path) {
        RemoveAlbumFolder(path);
    });
    viewManager_->GetGalleryView()->SetAddAlbumCallback([this]() {
        AddAlbumFolder();
    });
    viewManager_->GetGalleryView()->SetFolderVisitCallback([this](const auto& folder) {
        RecordFolderVisit(folder);
    });

    LoadRecents();

    return true;
}

bool Application::InitializeDecoder()
{
    decoder_ = std::make_unique<ImageDecoder>();
    return true;
}

bool Application::InitializeCache()
{
    cache_ = std::make_unique<CacheManager>(512 * 1024 * 1024);
    return true;
}

bool Application::InitializeRenderer()
{
    DebugLog("  Creating Direct2DRenderer...");
    renderer_ = std::make_unique<Rendering::Direct2DRenderer>();
    if (!renderer_->Initialize(hwnd_)) {
        DebugLog("  FAIL: renderer->Initialize()");
        return false;
    }
    DebugLog("  OK: renderer->Initialize()");
    return true;
}

void Application::Render()
{
    if (!renderer_ || !viewManager_) return;

    // Sync view size with actual client area every frame to prevent mismatch
    RECT rc;
    if (GetClientRect(hwnd_, &rc)) {
        UINT cw = rc.right - rc.left;
        UINT ch = rc.bottom - rc.top;
        if (cw > 0 && ch > 0) {
            if (cw != windowWidth_ || ch != windowHeight_) {
                windowWidth_ = cw;
                windowHeight_ = ch;
                renderer_->Resize(cw, ch);
                viewManager_->SetViewSize(static_cast<float>(cw) / dpiScale_,
                                           static_cast<float>(ch) / dpiScale_);
            }
        }
    }

    renderer_->BeginDraw();
    viewManager_->Render(renderer_.get());
    renderer_->EndDraw();
}

LRESULT CALLBACK Application::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    Application* app = nullptr;

    if (msg == WM_CREATE) {
        LPCREATESTRUCTW pCreate = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        app = reinterpret_cast<Application*>(pCreate->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        if (app) {
            app->hwnd_ = hwnd;
        }
        return 0;
    } else {
        app = reinterpret_cast<Application*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (app) {
        return app->HandleMessage(msg, wParam, lParam);
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT Application::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_PAINT:
            OnPaint();
            return 0;

        case WM_SIZE:
            OnSize(LOWORD(lParam), HIWORD(lParam));
            return 0;

        case WM_KEYDOWN:
            OnKeyDown(static_cast<UINT>(wParam));
            return 0;

        case WM_MOUSEWHEEL: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &pt);
            OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam), pt.x, pt.y);
            return 0;
        }

        case WM_LBUTTONDOWN:
            OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_MOUSEMOVE:
            OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_LBUTTONUP:
            OnMouseUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_MBUTTONDOWN:
            OnMiddleMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_MBUTTONUP:
            OnMiddleMouseUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_DROPFILES:
            OnDropFiles(reinterpret_cast<HDROP>(wParam));
            return 0;

        case WM_DPICHANGED:
            UpdateDpi();
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        default:
            return DefWindowProc(hwnd_, msg, wParam, lParam);
    }
}

void Application::OnPaint()
{
    // Rendering is driven by the game loop, not WM_PAINT.
    // Just validate the rect to prevent continuous WM_PAINT messages.
    ValidateRect(hwnd_, nullptr);
    needsRender_ = true;
}

void Application::OnSize(UINT width, UINT height)
{
    if (width == 0 || height == 0) return;

    windowWidth_ = width;
    windowHeight_ = height;

    if (renderer_) {
        renderer_->Resize(width, height);
    }
    if (viewManager_) {
        viewManager_->SetViewSize(static_cast<float>(width) / dpiScale_,
                                   static_cast<float>(height) / dpiScale_);
    }
    needsRender_ = true;
}

void Application::OnKeyDown(UINT key)
{
    // Ctrl+D: add album folder
    if ((GetKeyState(VK_CONTROL) & 0x8000) && (key == 'D')) {
        AddAlbumFolder();
        return;
    }

    // Ctrl+O: open file dialog (manual override)
    if ((GetKeyState(VK_CONTROL) & 0x8000) && (key == 'O')) {
        // Cancel any ongoing scan
        if (isScanning_) {
            scanCancelled_ = true;
            if (scanThread_.joinable()) {
                scanThread_.request_stop();
                scanThread_.join();
            }
            isScanning_ = false;
            if (viewManager_) {
                viewManager_->GetGalleryView()->SetScanningState(false, 0);
            }
        }

        auto paths = ShowOpenDialog();
        if (!paths.empty()) {
            OpenImages(paths);
        }
        return;
    }

    // Escape in gallery during manual open: restore full scan library
    // (but not when edit mode is active — let edit mode exit first)
    if (key == VK_ESCAPE && inManualOpen_) {
        if (viewManager_ && viewManager_->GetState() == UI::ViewState::Gallery) {
            if (!viewManager_->GetGalleryView()->IsInEditMode()) {
                RestoreScanGallery();
                return;
            }
        }
    }

    if (viewManager_) {
        viewManager_->OnKeyDown(key);
    }
    needsRender_ = true;
}

void Application::OnMouseWheel(short delta, int x, int y)
{
    if (viewManager_) {
        viewManager_->OnMouseWheel(static_cast<float>(delta),
                                    static_cast<float>(x) / dpiScale_,
                                    static_cast<float>(y) / dpiScale_);
    }
    needsRender_ = true;
}

void Application::OnDropFiles(HDROP hDrop)
{
    UINT fileCount = DragQueryFile(hDrop, 0xFFFFFFFF, nullptr, 0);

    std::vector<std::filesystem::path> droppedFiles;
    for (UINT i = 0; i < fileCount; ++i) {
        WCHAR fileName[MAX_PATH];
        if (DragQueryFile(hDrop, i, fileName, MAX_PATH) > 0) {
            std::filesystem::path path(fileName);
            if (ImageDecoder::IsSupportedFormat(path)) {
                droppedFiles.push_back(path);
            }
        }
    }
    DragFinish(hDrop);

    if (!droppedFiles.empty()) {
        // Cancel any ongoing scan
        if (isScanning_) {
            scanCancelled_ = true;
            if (scanThread_.joinable()) {
                scanThread_.request_stop();
                scanThread_.join();
            }
            isScanning_ = false;
        }
        OpenImages(droppedFiles);
    }
}

void Application::OnMouseDown(int x, int y)
{
    SetCapture(hwnd_);
    if (viewManager_) {
        viewManager_->OnMouseDown(static_cast<float>(x) / dpiScale_,
                                   static_cast<float>(y) / dpiScale_);
    }
    needsRender_ = true;
}

void Application::OnMouseMove(int x, int y)
{
    if (viewManager_) {
        viewManager_->OnMouseMove(static_cast<float>(x) / dpiScale_,
                                   static_cast<float>(y) / dpiScale_);
    }
}

void Application::OnMouseUp(int x, int y)
{
    ReleaseCapture();
    if (viewManager_) {
        viewManager_->OnMouseUp(static_cast<float>(x) / dpiScale_,
                                 static_cast<float>(y) / dpiScale_);
    }
    needsRender_ = true;
}

void Application::OnMiddleMouseDown(int x, int y)
{
    SetCapture(hwnd_);
    if (viewManager_) {
        viewManager_->OnMiddleMouseDown(static_cast<float>(x) / dpiScale_,
                                         static_cast<float>(y) / dpiScale_);
    }
    needsRender_ = true;
}

void Application::OnMiddleMouseUp(int x, int y)
{
    ReleaseCapture();
    if (viewManager_) {
        viewManager_->OnMiddleMouseUp(static_cast<float>(x) / dpiScale_,
                                       static_cast<float>(y) / dpiScale_);
    }
    needsRender_ = true;
}

void Application::OpenImages(const std::vector<std::filesystem::path>& paths)
{
    if (paths.empty()) return;

    // Cancel any ongoing scan
    if (isScanning_) {
        scanCancelled_ = true;
        if (scanThread_.joinable()) {
            scanThread_.request_stop();
            scanThread_.join();
        }
        isScanning_ = false;
        if (viewManager_) {
            viewManager_->GetGalleryView()->SetScanningState(false, 0);
        }
    }

    // Take the first file, scan its directory for all images
    const auto& firstPath = paths[0];
    auto dir = firstPath.parent_path();

    currentImages_ = ImagePipeline::ScanDirectory(dir);

    if (currentImages_.empty()) {
        // If directory scan failed, just use the provided paths
        currentImages_ = paths;
    }

    // Add to recent
    for (const auto& p : paths) {
        AddRecent(p);
    }

    // Set gallery view images
    if (viewManager_) {
        viewManager_->GetGalleryView()->SetImages(currentImages_);

        // Find the index of the first opened file
        size_t startIndex = 0;
        for (size_t i = 0; i < currentImages_.size(); ++i) {
            if (currentImages_[i] == firstPath) {
                startIndex = i;
                break;
            }
        }

        // If only opening a single file, go directly to viewer
        if (paths.size() == 1) {
            auto cellRect = viewManager_->GetGalleryView()->GetCellScreenRect(startIndex);
            D2D1_RECT_F fromRect = cellRect.value_or(D2D1::RectF(
                static_cast<float>(windowWidth_) * 0.5f - 50.0f,
                static_cast<float>(windowHeight_) * 0.5f - 50.0f,
                static_cast<float>(windowWidth_) * 0.5f + 50.0f,
                static_cast<float>(windowHeight_) * 0.5f + 50.0f));
            viewManager_->TransitionToViewer(startIndex, fromRect);
        }
    }

    // Track manual open so user can return to scan gallery
    inManualOpen_ = true;
    if (viewManager_) {
        viewManager_->GetGalleryView()->SetManualOpenMode(true);
    }

    std::wstring title = windowTitle_ + L" - " + dir.filename().wstring();
    SetWindowTextW(hwnd_, title.c_str());

    needsRender_ = true;
}

void Application::RestoreScanGallery()
{
    inManualOpen_ = false;
    if (viewManager_) {
        viewManager_->GetGalleryView()->SetManualOpenMode(false);
    }

    // Try to restore from cached scan results
    std::vector<ScannedImage> results;
    {
        std::lock_guard lock(scanMutex_);
        results = scannedResults_;
    }

    if (!results.empty() && viewManager_) {
        viewManager_->GetGalleryView()->SetImagesGrouped(results);
        currentImages_.clear();
        for (const auto& img : results) {
            currentImages_.push_back(img.path);
        }
        std::wstring title = windowTitle_ + L" - " +
            std::to_wstring(results.size()) + L" photos";
        SetWindowTextW(hwnd_, title.c_str());
    } else {
        // No cached results — start a fresh scan
        StartFullScan();
    }

    needsRender_ = true;
}

std::vector<std::filesystem::path> Application::ShowOpenDialog()
{
    std::vector<std::filesystem::path> result;
    std::vector<wchar_t> buffer(64 * 1024);
    buffer[0] = L'\0';

    const wchar_t filter[] =
        L"Image Files (*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff;*.webp;*.ico;*.jxr)\0"
        L"*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff;*.webp;*.ico;*.jxr\0"
        L"All Files (*.*)\0*.*\0";

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT;

    if (!GetOpenFileNameW(&ofn)) {
        return result;
    }

    std::wstring first = buffer.data();
    size_t offset = first.size() + 1;
    if (buffer[offset] == L'\0') {
        result.emplace_back(first);
        return result;
    }

    std::filesystem::path dir(first);
    while (buffer[offset] != L'\0') {
        std::wstring file = &buffer[offset];
        result.emplace_back(dir / file);
        offset += file.size() + 1;
    }

    return result;
}

void Application::UpdateDpi()
{
    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    UINT dpiX, dpiY;
    if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        dpiScale_ = static_cast<float>(dpiX) / 96.0f;
        if (renderer_) {
            renderer_->SetDpi(static_cast<float>(dpiX), static_cast<float>(dpiY));
        }
    }
    needsRender_ = true;
}

void Application::LoadRecents()
{
    recentItems_.clear();
    recentFilePath_ = GetRecentFilePath();
    if (recentFilePath_.empty()) return;

    std::wifstream in(recentFilePath_);
    if (!in.is_open()) return;

    std::wstring line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            recentItems_.push_back(line);
        }
        if (recentItems_.size() >= kMaxRecentItems) break;
    }
}

void Application::SaveRecents()
{
    if (recentFilePath_.empty()) return;
    std::filesystem::create_directories(recentFilePath_.parent_path());
    std::wofstream out(recentFilePath_, std::ios::trunc);
    for (size_t i = 0; i < recentItems_.size() && i < kMaxRecentItems; ++i) {
        out << recentItems_[i].wstring() << L"\n";
    }
}

void Application::AddRecent(const std::filesystem::path& path)
{
    auto it = std::find(recentItems_.begin(), recentItems_.end(), path);
    if (it != recentItems_.end()) {
        recentItems_.erase(it);
    }
    recentItems_.insert(recentItems_.begin(), path);
    if (recentItems_.size() > kMaxRecentItems) {
        recentItems_.resize(kMaxRecentItems);
    }
    SaveRecents();
}

std::filesystem::path Application::GetRecentFilePath() const
{
    PWSTR outPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &outPath))) {
        return {};
    }
    std::filesystem::path base(outPath);
    CoTaskMemFree(outPath);
    return base / L"UltraImageViewer" / L"recent.txt";
}

// --- Album folder management ---

std::filesystem::path Application::GetAlbumFilePath() const
{
    PWSTR outPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &outPath))) {
        return {};
    }
    std::filesystem::path base(outPath);
    CoTaskMemFree(outPath);
    return base / L"UltraImageViewer" / L"albums.txt";
}

void Application::LoadAlbumFolders()
{
    albumFolders_.clear();
    auto filePath = GetAlbumFilePath();
    if (filePath.empty()) return;

    std::wifstream in(filePath);
    if (!in.is_open()) return;

    std::wstring line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::filesystem::path dir(line);
        if (std::filesystem::is_directory(dir)) {
            albumFolders_.push_back(dir);
        }
    }

    DebugLog(("Loaded " + std::to_string(albumFolders_.size()) + " album folders").c_str());
}

void Application::SaveAlbumFolders()
{
    auto filePath = GetAlbumFilePath();
    if (filePath.empty()) return;

    std::filesystem::create_directories(filePath.parent_path());
    std::wofstream out(filePath, std::ios::trunc);
    for (const auto& dir : albumFolders_) {
        out << dir.wstring() << L"\n";
    }
}

std::filesystem::path Application::ShowFolderDialog()
{
    Microsoft::WRL::ComPtr<IFileDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return {};

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Select Album Folder");

    if (dialog->Show(hwnd_) != S_OK)
        return {};

    Microsoft::WRL::ComPtr<IShellItem> item;
    dialog->GetResult(&item);
    if (!item) return {};

    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return {};

    std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}

void Application::AddAlbumFolder()
{
    auto folder = ShowFolderDialog();
    if (folder.empty()) return;

    // Check for duplicate
    for (const auto& existing : albumFolders_) {
        if (std::filesystem::equivalent(existing, folder)) {
            DebugLog("Album folder already exists, skipping");
            return;
        }
    }

    albumFolders_.push_back(folder);
    SaveAlbumFolders();

    DebugLog(("Added album folder: " + folder.string()).c_str());

    // Cancel current scan and rescan all sources
    if (isScanning_) {
        scanCancelled_ = true;
        if (scanThread_.joinable()) {
            scanThread_.request_stop();
            scanThread_.join();
        }
        isScanning_ = false;
    }

    StartFullScan();
}

void Application::RemoveAlbumFolder(const std::filesystem::path& albumPath)
{
    DebugLog(("RemoveAlbumFolder: " + albumPath.string()).c_str());

    // --- 1. Add to persistent hidden list (survives rescans) ---
    hiddenAlbumPaths_.push_back(albumPath);
    SaveHiddenAlbums();

    // --- 2. In-memory surgery: erase images whose parent == albumPath ---
    std::wstring albumLower = albumPath.wstring();
    Simd::ToLowerInPlace(albumLower);

    std::vector<ScannedImage> results;
    {
        std::lock_guard lock(scanMutex_);
        scannedResults_.erase(
            std::remove_if(scannedResults_.begin(), scannedResults_.end(),
                [&albumLower](const ScannedImage& img) {
                    std::wstring p = img.path.parent_path().wstring();
                    Simd::ToLowerInPlace(p);
                    return p == albumLower;
                }),
            scannedResults_.end());
        results = scannedResults_;
    }

    // --- 3. Push pruned data to gallery (rebuilds Photos + Albums) ---
    if (viewManager_)
        viewManager_->GetGalleryView()->SetImagesGrouped(results);

    // --- 4. Update flat image list ---
    currentImages_.clear();
    currentImages_.reserve(results.size());
    for (const auto& img : results)
        currentImages_.push_back(img.path);

    SetWindowTextW(hwnd_, (windowTitle_ + L" - " +
        std::to_wstring(results.size()) + L" photos").c_str());

    // --- 5. Persist pruned scan cache ---
    SaveScanCache(results);

    needsRender_ = true;
}


// --- Hidden albums persistence (user-deleted albums survive rescans) ---

std::filesystem::path Application::GetHiddenAlbumsPath() const
{
    PWSTR outPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &outPath))) {
        return {};
    }
    std::filesystem::path base(outPath);
    CoTaskMemFree(outPath);
    return base / L"UltraImageViewer" / L"hidden_albums.txt";
}

void Application::LoadHiddenAlbums()
{
    hiddenAlbumPaths_.clear();
    auto filePath = GetHiddenAlbumsPath();
    if (filePath.empty()) return;

    std::wifstream in(filePath);
    if (!in.is_open()) return;

    std::wstring line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            hiddenAlbumPaths_.emplace_back(line);
        }
    }

    DebugLog(("Loaded " + std::to_string(hiddenAlbumPaths_.size()) + " hidden albums").c_str());
}

void Application::SaveHiddenAlbums()
{
    auto filePath = GetHiddenAlbumsPath();
    if (filePath.empty()) return;

    std::filesystem::create_directories(filePath.parent_path());
    std::wofstream out(filePath, std::ios::trunc);
    for (const auto& p : hiddenAlbumPaths_) {
        out << p.wstring() << L"\n";
    }
}

void Application::FilterHiddenAlbums(std::vector<ScannedImage>& images) const
{
    if (hiddenAlbumPaths_.empty()) return;

    // Build O(1) lookup set of lowered hidden folder paths
    std::unordered_set<std::wstring> hidden;
    hidden.reserve(hiddenAlbumPaths_.size());
    for (const auto& p : hiddenAlbumPaths_) {
        std::wstring low = p.wstring();
        Simd::ToLowerInPlace(low);
        hidden.insert(std::move(low));
    }

    // Erase images whose parent folder is in the hidden set
    images.erase(
        std::remove_if(images.begin(), images.end(),
            [&hidden](const ScannedImage& img) {
                std::wstring parent = img.path.parent_path().wstring();
                Simd::ToLowerInPlace(parent);
                return hidden.contains(parent);
            }),
        images.end());
}

// --- Scan cache persistence (binary format) ---

std::filesystem::path Application::GetScanCachePath() const
{
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return {};
    return std::filesystem::path(exePath).parent_path() / L"scan_cache.bin";
}

void Application::SaveScanCache(const std::vector<ScannedImage>& results)
{
    auto filePath = GetScanCachePath();
    if (filePath.empty()) return;

    try {
        // Binary layout:
        //   Header (32 bytes): magic(4) + version(4) + entry_count(4) + string_blob_size(4) + timestamp(8) + reserved(8)
        //   Entry table (entry_count * 12 bytes): path_offset(4) + path_len(2) + year(2) + month(2) + reserved(2)
        //   String blob (string_blob_size bytes): packed wchar_t path strings

        const uint32_t entryCount = static_cast<uint32_t>(results.size());
        constexpr uint32_t kHeaderSize = 32;
        constexpr uint32_t kEntrySize = 12;

        // Build entry table and string blob
        std::vector<uint8_t> entryTable(entryCount * kEntrySize);
        std::vector<uint8_t> stringBlob;
        stringBlob.reserve(entryCount * 160);  // ~80 wchar avg path

        for (uint32_t i = 0; i < entryCount; ++i) {
            const auto& img = results[i];
            std::wstring pathStr = img.path.wstring();
            uint32_t pathOffset = static_cast<uint32_t>(stringBlob.size());
            uint16_t pathLen = static_cast<uint16_t>(pathStr.size());

            // Append path wchars to string blob
            const uint8_t* pathBytes = reinterpret_cast<const uint8_t*>(pathStr.data());
            stringBlob.insert(stringBlob.end(), pathBytes, pathBytes + pathLen * sizeof(wchar_t));

            // Fill entry
            uint8_t* entry = entryTable.data() + i * kEntrySize;
            memcpy(entry + 0, &pathOffset, 4);
            memcpy(entry + 4, &pathLen, 2);
            int16_t year = static_cast<int16_t>(img.year);
            int16_t month = static_cast<int16_t>(img.month);
            memcpy(entry + 6, &year, 2);
            memcpy(entry + 8, &month, 2);
            uint16_t reserved = 0;
            memcpy(entry + 10, &reserved, 2);
        }

        uint32_t stringBlobSize = static_cast<uint32_t>(stringBlob.size());

        // Build header
        uint8_t header[kHeaderSize] = {};
        memcpy(header + 0, "UIVC", 4);                         // magic
        uint32_t version = 1;
        memcpy(header + 4, &version, 4);                        // version
        memcpy(header + 8, &entryCount, 4);                     // entry_count
        memcpy(header + 12, &stringBlobSize, 4);                // string_blob_size
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        uint64_t timestamp = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        memcpy(header + 16, &timestamp, 8);                     // timestamp
        // [24..31] reserved, already zero

        // Write entire buffer in one call
        FILE* f = _wfopen(filePath.c_str(), L"wb");
        if (!f) return;

        fwrite(header, 1, kHeaderSize, f);
        if (!entryTable.empty()) fwrite(entryTable.data(), 1, entryTable.size(), f);
        if (!stringBlob.empty()) fwrite(stringBlob.data(), 1, stringBlob.size(), f);
        fclose(f);

        DebugLog(("Saved scan cache (binary): " + std::to_string(results.size()) + " entries, "
                  + std::to_string(kHeaderSize + entryTable.size() + stringBlob.size()) + " bytes").c_str());
    } catch (...) {
        DebugLog("Failed to save scan cache");
    }
}

std::vector<ScannedImage> Application::LoadScanCache()
{
    std::vector<ScannedImage> results;
    auto filePath = GetScanCachePath();
    if (filePath.empty()) return results;

    try {
        FILE* f = _wfopen(filePath.c_str(), L"rb");
        if (!f) return results;

        // Get file size
        _fseeki64(f, 0, SEEK_END);
        long long fileSize = _ftelli64(f);
        _fseeki64(f, 0, SEEK_SET);

        constexpr uint32_t kHeaderSize = 32;
        constexpr uint32_t kEntrySize = 12;

        if (fileSize < kHeaderSize) { fclose(f); return results; }

        // Read entire file in one call
        std::vector<uint8_t> buf(static_cast<size_t>(fileSize));
        size_t bytesRead = fread(buf.data(), 1, buf.size(), f);
        fclose(f);

        if (bytesRead != static_cast<size_t>(fileSize)) return results;

        // Validate header
        if (memcmp(buf.data(), "UIVC", 4) != 0) return results;  // bad magic

        uint32_t version, entryCount, stringBlobSize;
        memcpy(&version, buf.data() + 4, 4);
        if (version != 1) return results;  // unsupported version

        memcpy(&entryCount, buf.data() + 8, 4);
        memcpy(&stringBlobSize, buf.data() + 12, 4);

        // Validate total size
        uint64_t expectedSize = static_cast<uint64_t>(kHeaderSize)
                              + static_cast<uint64_t>(entryCount) * kEntrySize
                              + stringBlobSize;
        if (expectedSize != static_cast<uint64_t>(fileSize)) return results;

        // Parse entries
        const uint8_t* entryBase = buf.data() + kHeaderSize;
        const uint8_t* blobBase = entryBase + static_cast<size_t>(entryCount) * kEntrySize;

        results.reserve(entryCount);
        for (uint32_t i = 0; i < entryCount; ++i) {
            const uint8_t* entry = entryBase + i * kEntrySize;

            uint32_t pathOffset;
            uint16_t pathLen;
            int16_t year, month;
            memcpy(&pathOffset, entry + 0, 4);
            memcpy(&pathLen, entry + 4, 2);
            memcpy(&year, entry + 6, 2);
            memcpy(&month, entry + 8, 2);

            // Bounds check: path must fit within string blob
            if (pathOffset + pathLen * sizeof(wchar_t) > stringBlobSize) return {};

            const wchar_t* pathChars = reinterpret_cast<const wchar_t*>(blobBase + pathOffset);

            ScannedImage img;
            img.path = std::wstring(pathChars, pathLen);
            img.year = year;
            img.month = month;
            results.push_back(std::move(img));
        }

        DebugLog(("Loaded scan cache (binary): " + std::to_string(results.size()) + " entries").c_str());
    } catch (...) {
        DebugLog("Failed to load scan cache");
    }

    return results;
}

// --- Folder access profiles (Ledger-inspired persistence) ---

std::filesystem::path Application::GetFolderProfilePath() const
{
    PWSTR outPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &outPath))) {
        return {};
    }
    std::filesystem::path base(outPath);
    CoTaskMemFree(outPath);
    return base / L"UltraImageViewer" / L"folder_profiles.bin";
}

void Application::LoadFolderProfiles()
{
    folderProfiles_.clear();
    auto path = GetFolderProfilePath();
    if (path.empty()) return;

    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return;

    // Header: "FPROF" + version(4) + count(4)
    char magic[6] = {};
    if (fread(magic, 1, 5, f) != 5) { fclose(f); return; }
    if (memcmp(magic, "FPROF", 5) != 0) { fclose(f); return; }

    uint32_t version = 0, count = 0;
    if (fread(&version, 4, 1, f) != 1 || fread(&count, 4, 1, f) != 1) { fclose(f); return; }
    if (version != 1) { fclose(f); return; }
    if (count > 100000) { fclose(f); return; }  // sanity: no more than 100k folders

    for (uint32_t i = 0; i < count; ++i) {
        uint16_t pathLen = 0;
        if (fread(&pathLen, 2, 1, f) != 1) break;

        std::wstring wpath(pathLen, L'\0');
        if (fread(wpath.data(), sizeof(wchar_t), pathLen, f) != pathLen) break;

        FolderProfile fp;
        fp.folder = std::filesystem::path(std::move(wpath));
        if (fread(&fp.visitCount, 4, 1, f) != 1 ||
            fread(&fp.thumbnailCount, 4, 1, f) != 1 ||
            fread(&fp.totalDecodeTimeMs, 8, 1, f) != 1 ||
            fread(&fp.lastVisitEpoch, 8, 1, f) != 1) break;
        folderProfiles_.push_back(std::move(fp));
    }

    fclose(f);
    DebugLog(("Loaded " + std::to_string(folderProfiles_.size()) + " folder profiles").c_str());
}

void Application::SaveFolderProfiles()
{
    auto path = GetFolderProfilePath();
    if (path.empty()) return;

    std::lock_guard lock(scanMutex_);

    std::filesystem::create_directories(path.parent_path());
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;

    fwrite("FPROF", 1, 5, f);
    uint32_t version = 1;
    uint32_t count = static_cast<uint32_t>(folderProfiles_.size());
    fwrite(&version, 4, 1, f);
    fwrite(&count, 4, 1, f);

    for (const auto& fp : folderProfiles_) {
        std::wstring wpath = fp.folder.wstring();
        uint16_t pathLen = static_cast<uint16_t>(wpath.size());
        fwrite(&pathLen, 2, 1, f);
        fwrite(wpath.data(), sizeof(wchar_t), pathLen, f);
        fwrite(&fp.visitCount, 4, 1, f);
        fwrite(&fp.thumbnailCount, 4, 1, f);
        fwrite(&fp.totalDecodeTimeMs, 8, 1, f);
        fwrite(&fp.lastVisitEpoch, 8, 1, f);
    }

    fclose(f);
}

void Application::RecordFolderVisit(const std::filesystem::path& folder)
{
    std::lock_guard lock(scanMutex_);

    auto now = std::chrono::system_clock::now();
    int64_t epoch = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    for (auto& fp : folderProfiles_) {
        if (fp.folder == folder) {
            fp.visitCount++;
            fp.lastVisitEpoch = epoch;
            return;
        }
    }

    // New folder
    FolderProfile fp;
    fp.folder = folder;
    fp.visitCount = 1;
    fp.lastVisitEpoch = epoch;
    folderProfiles_.push_back(std::move(fp));
}

std::vector<std::filesystem::path> Application::GetPrioritizedFolders() const
{
    // Sort by visit count descending, then by recency
    std::lock_guard lock(scanMutex_);
    auto sorted = folderProfiles_;
    std::sort(sorted.begin(), sorted.end(),
        [](const FolderProfile& a, const FolderProfile& b) {
            if (a.visitCount != b.visitCount) return a.visitCount > b.visitCount;
            return a.lastVisitEpoch > b.lastVisitEpoch;
        });

    std::vector<std::filesystem::path> result;
    result.reserve(sorted.size());
    for (const auto& fp : sorted) {
        result.push_back(fp.folder);
    }
    return result;
}

} // namespace Core
} // namespace UltraImageViewer
