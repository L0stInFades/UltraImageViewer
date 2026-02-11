#pragma once

#include <filesystem>
#include <vector>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <deque>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <wrl/client.h>
#include <d2d1.h>

#include "ImageDecoder.hpp"
#include "CacheManager.hpp"
#include "ThreadPool.hpp"
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

    // Thumbnail (fast, low-resolution) — synchronous, kept for compatibility
    Microsoft::WRL::ComPtr<ID2D1Bitmap> GetThumbnail(const std::filesystem::path& path, uint32_t maxSize = 256);

    // --- Async thumbnail API (non-blocking) ---

    // Returns cached bitmap immediately, or nullptr if not yet decoded.
    // Queues a background decode request on cache miss.
    Microsoft::WRL::ComPtr<ID2D1Bitmap> RequestThumbnail(const std::filesystem::path& path, uint32_t targetSize);

    // Called by render thread each frame. Creates D2D bitmaps from decoded pixel
    // buffers (up to maxCount per frame to stay within frame budget).
    // Returns the number of bitmaps created this frame.
    int FlushReadyThumbnails(int maxCount);

    // Cancel pending non-visible requests and increment generation counter.
    // Call on fast scroll to avoid wasting decode work on off-screen images.
    void InvalidateRequests();

    // Tell pipeline which paths are currently visible for prioritization.
    void SetVisibleRange(const std::vector<std::filesystem::path>& paths);

    // True if any decoded thumbnails are waiting for GPU upload
    bool HasPendingThumbnails() const;

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

    // Cache-only thumbnail lookup (no decode queuing). Used during fast scroll
    // to display already-loaded thumbnails without starting new work.
    Microsoft::WRL::ComPtr<ID2D1Bitmap> GetCachedThumbnail(const std::filesystem::path& path);

    // Check if a thumbnail is already cached
    bool HasThumbnail(const std::filesystem::path& path) const;
    bool HasFullImage(const std::filesystem::path& path) const;

    // Persistent thumbnail cache (disk-backed, memory-mapped)
    void LoadPersistentThumbs(const std::filesystem::path& cachePath);
    void SavePersistentThumbs(const std::filesystem::path& cachePath);

private:
    // Decode and create D2D bitmap from a path
    Microsoft::WRL::ComPtr<ID2D1Bitmap> DecodeAndCreateBitmap(const std::filesystem::path& path);
    Microsoft::WRL::ComPtr<ID2D1Bitmap> DecodeAndCreateThumbnail(const std::filesystem::path& path, uint32_t maxSize);

    // Single-task thumbnail decode (submitted to ThreadPool)
    void ThumbnailDecodeTask(const std::filesystem::path& path,
                             uint32_t targetSize, uint64_t generation);

    // LRU eviction for thumbnail cache (demotes to Tier 2 compressed cache)
    void EvictThumbnailsIfNeeded();

    // --- Tier 2: CPU-RAM compressed pixel cache ---
    // Evicted GPU bitmaps are compressed and kept in RAM. On re-request,
    // decompressing from RAM (~0.3ms) is much faster than re-reading from
    // disk and decoding JPEG (~5ms).
    struct CompressedThumbnail {
        std::unique_ptr<uint8_t[]> data;
        size_t compressedSize = 0;
        uint32_t rawSize = 0;  // uncompressed BGRA size
        uint16_t width = 0;
        uint16_t height = 0;
    };
    std::unordered_map<std::filesystem::path, CompressedThumbnail> tier2Cache_;
    size_t tier2Bytes_ = 0;  // total compressed bytes
    static constexpr size_t kTier2MaxBytes = 256ULL * 1024 * 1024;  // 256MB compressed

    // Compress/decompress helpers (Windows Compression API: XPRESS + Huffman)
    static bool CompressPixels(const uint8_t* src, uint32_t srcSize,
                               std::unique_ptr<uint8_t[]>& outBuf, size_t& outSize);
    static bool DecompressPixels(const uint8_t* src, size_t srcSize,
                                 uint8_t* dst, uint32_t dstSize);

    ImageDecoder* decoder_ = nullptr;
    CacheManager* cache_ = nullptr;
    Rendering::Direct2DRenderer* renderer_ = nullptr;

    // Unified thread pool (replaces all ad-hoc threads)
    std::unique_ptr<ThreadPool> threadPool_;
    std::atomic<bool> shutdownRequested_ = false;

    // Bitmap caches
    struct ThumbnailCacheEntry {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        uint32_t width = 0;
        uint32_t height = 0;
        std::chrono::steady_clock::time_point lastAccess;
    };
    std::unordered_map<std::filesystem::path, ThumbnailCacheEntry> thumbnailCache_;
    size_t thumbnailCacheBytes_ = 0;

    std::unordered_map<std::filesystem::path, Microsoft::WRL::ComPtr<ID2D1Bitmap>> fullImageCache_;
    mutable std::mutex cacheMutex_;

    // --- Async thumbnail pipeline ---

    // Decoded pixel buffer produced by worker threads (CPU-only, no D2D)
    struct ReadyThumbnail {
        std::filesystem::path path;
        std::unique_ptr<uint8_t[]> pixels;
        uint32_t width;
        uint32_t height;
    };

    // Ready queue: decoded pixel buffers waiting for GPU upload (deque for O(1) pop_front)
    std::deque<ReadyThumbnail> readyQueue_;
    mutable std::mutex readyMutex_;

    // Generation counter: incremented on InvalidateRequests()
    std::atomic<uint64_t> generation_{0};

    // Track which paths have pending requests to avoid duplicate queuing
    // Protected by cacheMutex_
    std::unordered_map<std::filesystem::path, uint64_t> pendingRequests_;

    // Currently visible paths (for prioritization). Protected by cacheMutex_
    std::unordered_map<std::filesystem::path, bool> visiblePaths_;

    // --- Persistent thumbnail cache (memory-mapped file) ---
    void ClosePersistentMapping();

    struct PersistThumbInfo {
        const uint8_t* pixelData;  // pointer into memory-mapped region
        uint16_t width;
        uint16_t height;
    };
    std::unordered_map<std::filesystem::path, PersistThumbInfo> persistIndex_;
    void* persistFileH_ = nullptr;      // HANDLE, nullptr = not open
    void* persistMapH_ = nullptr;       // HANDLE
    const uint8_t* persistData_ = nullptr;
    size_t persistSize_ = 0;
    std::shared_mutex persistMutex_;    // readers: worker threads, writer: save

    // Save buffer: raw pixels collected during FlushReadyThumbnails
    struct ThumbSaveEntry {
        uint16_t width;
        uint16_t height;
        uint32_t pixelSize;
        std::unique_ptr<uint8_t[]> pixels;
    };
    std::unordered_map<std::filesystem::path, ThumbSaveEntry> thumbSaveBuffer_;
    std::mutex thumbSaveMutex_;

    // Per-frame budget for synchronous D2D bitmap creation from persistent cache
    // (render thread only — no synchronization needed)
    int persistSyncBudget_ = 0;
};

} // namespace Core
} // namespace UltraImageViewer
