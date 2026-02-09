#pragma once

#include <windows.h>
#include <shellapi.h>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include <wrl/client.h>

#include "ImageDecoder.hpp"
#include "MemoryManager.hpp"
#include "CacheManager.hpp"
#include "ImagePipeline.hpp"
#include "../rendering/Direct2DRenderer.hpp"
#include "../animation/AnimationEngine.hpp"
#include "../ui/ViewManager.hpp"

namespace UltraImageViewer {
namespace Core {

/**
 * Main application class
 * Direct2D + DirectComposition rendering with iOS Photos-level UX
 */
class Application {
public:
    Application();
    ~Application();

    // Initialization
    bool Initialize(HINSTANCE hInstance);
    void Shutdown();

    // Main loop (game-loop style)
    int Run(int nCmdShow);

    // Accessors
    HINSTANCE GetHInstance() const { return hInstance_; }
    HWND GetWindowHandle() const { return hwnd_; }

    // Core components
    ImageDecoder* GetDecoder() { return decoder_.get(); }
    CacheManager* GetCache() { return cache_.get(); }
    Rendering::Direct2DRenderer* GetRenderer() { return renderer_.get(); }

    // Open images (public for command-line use)
    void OpenImages(const std::vector<std::filesystem::path>& paths);

    // Singleton access
    static Application* GetInstance() { return s_instance; }

private:
    // Window procedure
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    // Component initialization
    bool InitializeComponents();
    bool InitializeWindow();
    bool InitializeRenderer();
    bool InitializeDecoder();
    bool InitializeCache();
    void SetDarkTitleBar();

    // Rendering
    void Render();

    // Message handlers
    void OnPaint();
    void OnSize(UINT width, UINT height);
    void OnKeyDown(UINT key);
    void OnMouseWheel(short delta, int x, int y);
    void OnDropFiles(HDROP hDrop);
    void OnMouseDown(int x, int y);
    void OnMouseMove(int x, int y);
    void OnMouseUp(int x, int y);
    void OnMiddleMouseDown(int x, int y);
    void OnMiddleMouseUp(int x, int y);

    // Image management
    std::vector<std::filesystem::path> ShowOpenDialog();
    std::filesystem::path ShowFolderDialog();

    // Album folders
    void AddAlbumFolder();
    void LoadAlbumFolders();
    void SaveAlbumFolders();
    std::filesystem::path GetAlbumFilePath() const;

    // Recent files
    void LoadRecents();
    void SaveRecents();
    void AddRecent(const std::filesystem::path& path);
    std::filesystem::path GetRecentFilePath() const;

    // DPI
    void UpdateDpi();

    // Instance
    HINSTANCE hInstance_ = nullptr;
    HWND hwnd_ = nullptr;

    // Window state
    std::wstring windowTitle_ = L"\u62FE\u5149 Afterglow";
    UINT windowWidth_ = 1280;
    UINT windowHeight_ = 720;
    float dpiScale_ = 1.0f;

    // Core components
    std::unique_ptr<ImageDecoder> decoder_;
    std::unique_ptr<CacheManager> cache_;
    std::unique_ptr<Rendering::Direct2DRenderer> renderer_;
    std::unique_ptr<ImagePipeline> pipeline_;

    // UI components
    std::unique_ptr<Animation::AnimationEngine> animEngine_;
    std::unique_ptr<UI::ViewManager> viewManager_;

    // Current image list
    std::vector<std::filesystem::path> currentImages_;

    // Album folders (user-specified)
    std::vector<std::filesystem::path> albumFolders_;

    // Recent files
    std::vector<std::filesystem::path> recentItems_;
    std::filesystem::path recentFilePath_;
    static constexpr size_t kMaxRecentItems = 10;

    // Render state
    bool needsRender_ = true;
    LARGE_INTEGER lastFrameTime_ = {};
    LARGE_INTEGER perfFrequency_ = {};

    // Singleton
    static Application* s_instance;

    // State
    bool isInitialized_ = false;

    // System image scanning
    std::jthread scanThread_;
    std::atomic<bool> scanCancelled_{false};
    std::atomic<size_t> scanProgress_{0};
    std::atomic<bool> isScanning_{false};
    std::atomic<bool> scanDirty_{false};
    std::mutex scanMutex_;
    std::vector<ScannedImage> scannedResults_;
    size_t lastGalleryUpdateCount_ = 0;

    void StartFullScan();
    void CheckScanProgress();

    // Scan cache persistence
    std::filesystem::path GetScanCachePath() const;
    void SaveScanCache(const std::vector<ScannedImage>& results);
    std::vector<ScannedImage> LoadScanCache();

    // Persistent thumbnail cache (background save)
    std::jthread thumbSaveThread_;
};

} // namespace Core
} // namespace UltraImageViewer
