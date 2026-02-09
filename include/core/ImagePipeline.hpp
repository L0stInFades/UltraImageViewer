#pragma once

#include <filesystem>
#include <vector>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <thread>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <wrl/client.h>
#include <d2d1.h>

#include "ImageDecoder.hpp"
#include "CacheManager.hpp"
#include "../rendering/Direct2DRenderer.hpp"

namespace UltraImageViewer {
namespace Core {

struct ScannedImage {
    std::filesystem::path path;
    std::filesystem::path sourceFolder;  // Top-level scan folder this image came from
    int year = 0;
    int month = 0;
};

class ImagePipeline {
public:
    ImagePipeline();
    ~ImagePipeline();

    void Initialize(ImageDecoder* decoder, CacheManager* cache, Rendering::Direct2DRenderer* renderer);
    void Shutdown();

    // Synchronous bitmap retrieval (from cache first)
    Microsoft::WRL::ComPtr<ID2D1Bitmap> GetBitmap(const std::filesystem::path& path);

    // Asynchronous bitmap retrieval
    using BitmapCallback = std::function<void(Microsoft::WRL::ComPtr<ID2D1Bitmap>)>;
    void GetBitmapAsync(const std::filesystem::path& path, BitmapCallback callback);

    // Thumbnail (fast, low-resolution)
    Microsoft::WRL::ComPtr<ID2D1Bitmap> GetThumbnail(const std::filesystem::path& path, uint32_t maxSize = 256);

    // Prefetch images around current index
    void PrefetchAround(const std::vector<std::filesystem::path>& allPaths, size_t currentIndex, size_t radius = 3);

    // Scan a directory for supported image files
    static std::vector<std::filesystem::path> ScanDirectory(const std::filesystem::path& dir);

    // Scan arbitrary folders recursively for images (with date grouping)
    // Optional flushCallback is invoked periodically with sorted intermediate results
    // (every 200 images or after each top-level folder).
    using ScanFlushCallback = std::function<void(const std::vector<ScannedImage>&)>;

    static std::vector<ScannedImage> ScanFolders(
        const std::vector<std::filesystem::path>& folders,
        std::atomic<bool>& cancelFlag,
        std::atomic<size_t>& outCount,
        ScanFlushCallback flushCallback = nullptr);

    // Scan system image folders (Pictures, Desktop, Downloads) recursively
    static std::vector<ScannedImage> ScanSystemImages(
        std::atomic<bool>& cancelFlag,
        std::atomic<size_t>& outCount);

    // Check if a thumbnail is already cached
    bool HasThumbnail(const std::filesystem::path& path) const;
    bool HasFullImage(const std::filesystem::path& path) const;

private:
    // Decode and create D2D bitmap from a path
    Microsoft::WRL::ComPtr<ID2D1Bitmap> DecodeAndCreateBitmap(const std::filesystem::path& path);
    Microsoft::WRL::ComPtr<ID2D1Bitmap> DecodeAndCreateThumbnail(const std::filesystem::path& path, uint32_t maxSize);

    // Background loading thread
    void LoadThreadFunc();

    ImageDecoder* decoder_ = nullptr;
    CacheManager* cache_ = nullptr;
    Rendering::Direct2DRenderer* renderer_ = nullptr;

    // Bitmap caches
    std::unordered_map<std::filesystem::path, Microsoft::WRL::ComPtr<ID2D1Bitmap>> thumbnailCache_;
    std::unordered_map<std::filesystem::path, Microsoft::WRL::ComPtr<ID2D1Bitmap>> fullImageCache_;
    mutable std::mutex cacheMutex_;

    // Async loading
    struct LoadRequest {
        std::filesystem::path path;
        BitmapCallback callback;
        bool isThumbnail = false;
        uint32_t maxSize = 256;
    };

    std::jthread loadThread_;
    std::mutex queueMutex_;
    std::condition_variable queueCV_;
    std::queue<LoadRequest> loadQueue_;
    std::atomic<bool> shutdownRequested_ = false;
};

} // namespace Core
} // namespace UltraImageViewer
