#pragma once

#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include <optional>
#include <functional>
#include <wrl/client.h>
#include <wincodec.h>

namespace UltraImageViewer {
namespace Core {

struct ImageInfo {
    uint32_t width;
    uint32_t height;
    uint32_t bitsPerPixel;
    WICPixelFormatGUID pixelFormat;
    size_t dataSize;
    bool hasAlpha;
    bool isHDR;
};

struct DecodedImage {
    std::unique_ptr<uint8_t[]> data;
    ImageInfo info;
    std::filesystem::path sourcePath;

    // Zero-copy metadata
    void* userData = nullptr;
    size_t referenceCount = 0;
};

enum class DecoderFlags {
    None = 0,
    ZeroCopy = 1 << 0,
    MemoryMapped = 1 << 1,
    Cacheable = 1 << 3,
    BackgroundLoad = 1 << 4
};

inline DecoderFlags operator|(DecoderFlags a, DecoderFlags b) {
    return static_cast<DecoderFlags>(static_cast<int>(a) | static_cast<int>(b));
}

inline bool HasFlag(DecoderFlags flags, DecoderFlags flag) {
    return (static_cast<int>(flags) & static_cast<int>(flag)) != 0;
}

/**
 * Zero-copy image decoder
 * Supports JPEG, PNG, TIFF, BMP, GIF, WebP, and RAW formats
 */
class ImageDecoder {
public:
    ImageDecoder();
    ~ImageDecoder();

    // Main decoding interface
    std::unique_ptr<DecodedImage> Decode(
        const std::filesystem::path& filePath,
        DecoderFlags flags = DecoderFlags::ZeroCopy
    );

    // Async decoding
    void DecodeAsync(
        const std::filesystem::path& filePath,
        std::function<void(std::unique_ptr<DecodedImage>)> callback,
        DecoderFlags flags = DecoderFlags::ZeroCopy | DecoderFlags::BackgroundLoad
    );

    // Image info without full decoding
    std::optional<ImageInfo> GetImageInfo(const std::filesystem::path& filePath);

    // Thumbnail generation (fast, low-resolution)
    std::unique_ptr<DecodedImage> GenerateThumbnail(
        const std::filesystem::path& filePath,
        uint32_t maxSize = 256
    );

    // Supported formats
    static bool IsSupportedFormat(const std::filesystem::path& filePath);
    static std::vector<std::wstring> GetSupportedExtensions();

private:
    // WIC decoder implementation
    std::unique_ptr<DecodedImage> DecodeWithWIC(
        const std::filesystem::path& filePath,
        DecoderFlags flags
    );

    // RAW decoder implementation
    std::unique_ptr<DecodedImage> DecodeRAW(
        const std::filesystem::path& filePath,
        DecoderFlags flags
    );

    // Memory-mapped file support
    std::unique_ptr<DecodedImage> DecodeMemoryMapped(
        const std::filesystem::path& filePath,
        DecoderFlags flags
    );

    Microsoft::WRL::ComPtr<IWICImagingFactory2> wicFactory_;
};

} // namespace Core
} // namespace UltraImageViewer
