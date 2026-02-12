#include "core/ImageDecoder.hpp"
#include "core/SimdUtils.hpp"
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <thread>

namespace UltraImageViewer {
namespace Core {

ImageDecoder::ImageDecoder()
{
    // Initialize WIC factory
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory2,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory_)
    );

    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create WIC factory");
    }
}

ImageDecoder::~ImageDecoder() = default;

std::unique_ptr<DecodedImage> ImageDecoder::Decode(
    const std::filesystem::path& filePath,
    DecoderFlags flags)
{
    if (!IsSupportedFormat(filePath)) {
        return nullptr;
    }

    // Check if we should use memory mapping for large files
    if (HasFlag(flags, DecoderFlags::MemoryMapped)) {
        uintmax_t fileSize = std::filesystem::file_size(filePath);
        if (fileSize > 50 * 1024 * 1024) { // 50MB threshold
            return DecodeMemoryMapped(filePath, flags);
        }
    }

    return DecodeWithWIC(filePath, flags);
}

void ImageDecoder::DecodeAsync(
    const std::filesystem::path& filePath,
    std::function<void(std::unique_ptr<DecodedImage>)> callback,
    DecoderFlags flags)
{
    // Launch async decoding in background thread
    std::thread([this, filePath, callback, flags]() {
        auto image = this->Decode(filePath, flags);
        callback(std::move(image));
    }).detach();
}

std::optional<ImageInfo> ImageDecoder::GetImageInfo(const std::filesystem::path& filePath)
{
    if (!std::filesystem::exists(filePath)) {
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wicFactory_->CreateDecoderFromFilename(
        filePath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder
    );

    if (FAILED(hr)) {
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        return std::nullopt;
    }

    ImageInfo info = {};
    frame->GetSize(&info.width, &info.height);
    frame->GetPixelFormat(&info.pixelFormat);

    // Get bits per pixel
    Microsoft::WRL::ComPtr<IWICComponentInfo> componentInfo;
    wicFactory_->CreateComponentInfo(info.pixelFormat, &componentInfo);

    Microsoft::WRL::ComPtr<IWICPixelFormatInfo2> formatInfo;
    componentInfo->QueryInterface(IID_PPV_ARGS(&formatInfo));

    formatInfo->GetBitsPerPixel(&info.bitsPerPixel);

    return info;
}

std::unique_ptr<DecodedImage> ImageDecoder::GenerateThumbnail(
    const std::filesystem::path& filePath,
    uint32_t maxSize)
{
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wicFactory_->CreateDecoderFromFilename(
        filePath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder
    );

    if (FAILED(hr)) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        return nullptr;
    }

    // Get original dimensions
    uint32_t width, height;
    frame->GetSize(&width, &height);

    // Calculate thumbnail size maintaining aspect ratio
    uint32_t thumbWidth, thumbHeight;
    if (width > height) {
        thumbWidth = maxSize;
        thumbHeight = static_cast<uint32_t>((static_cast<float>(height) / width) * maxSize);
    } else {
        thumbHeight = maxSize;
        thumbWidth = static_cast<uint32_t>((static_cast<float>(width) / height) * maxSize);
    }

    // Create thumbnail
    Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
    wicFactory_->CreateBitmapScaler(&scaler);

    scaler->Initialize(
        frame.Get(),
        thumbWidth,
        thumbHeight,
        WICBitmapInterpolationModeFant
    );

    // Convert to 32-bit BGRA
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    wicFactory_->CreateFormatConverter(&converter);

    converter->Initialize(
        scaler.Get(),
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom
    );

    // Allocate buffer
    auto image = std::make_unique<DecodedImage>();
    image->sourcePath = filePath;
    image->info.width = thumbWidth;
    image->info.height = thumbHeight;
    image->info.pixelFormat = GUID_WICPixelFormat32bppPBGRA;
    image->info.bitsPerPixel = 32;
    image->info.dataSize = thumbWidth * thumbHeight * 4;
    image->data = std::make_unique<uint8_t[]>(image->info.dataSize);

    // Copy pixels
    hr = converter->CopyPixels(
        nullptr,
        thumbWidth * 4,
        image->info.dataSize,
        image->data.get()
    );

    if (FAILED(hr)) {
        return nullptr;
    }

    return image;
}

bool ImageDecoder::IsSupportedFormat(const std::filesystem::path& filePath)
{
    static const std::vector<std::wstring> extensions = {
        L".jpg", L".jpeg", L".png", L".bmp", L".gif",
        L".tiff", L".tif", L".webp", L".ico", L".jxr"
    };

    std::wstring ext = filePath.extension().wstring();
    Simd::ToLowerInPlace(ext);

    return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
}

std::vector<std::wstring> ImageDecoder::GetSupportedExtensions()
{
    return {
        L"*.jpg", L"*.jpeg", L"*.png", L"*.bmp", L"*.gif",
        L"*.tiff", L"*.tif", L"*.webp", L"*.ico", L"*.jxr"
    };
}

std::unique_ptr<DecodedImage> ImageDecoder::DecodeWithWIC(
    const std::filesystem::path& filePath,
    DecoderFlags flags)
{
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;

    HRESULT hr = wicFactory_->CreateDecoderFromFilename(
        filePath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder
    );

    if (FAILED(hr)) {
        return nullptr;
    }

    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        return nullptr;
    }

    // Get image info
    auto image = std::make_unique<DecodedImage>();
    image->sourcePath = filePath;

    frame->GetSize(&image->info.width, &image->info.height);
    frame->GetPixelFormat(&image->info.pixelFormat);

    // Check if format conversion is needed
    WICPixelFormatGUID targetFormat = GUID_WICPixelFormat32bppPBGRA;

    // Convert to 32-bit BGRA for GPU compatibility
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    wicFactory_->CreateFormatConverter(&converter);

    hr = converter->Initialize(
        frame.Get(),
        targetFormat,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom
    );

    if (FAILED(hr)) {
        return nullptr;
    }

    // Allocate buffer
    image->info.dataSize = image->info.width * image->info.height * 4;
    image->data = std::make_unique<uint8_t[]>(image->info.dataSize);

    // Copy pixels
    hr = converter->CopyPixels(
        nullptr,
        image->info.width * 4,
        image->info.dataSize,
        image->data.get()
    );

    if (FAILED(hr)) {
        return nullptr;
    }

    image->info.bitsPerPixel = 32;
    image->info.hasAlpha = true;
    image->info.isHDR = false;

    return image;
}

std::unique_ptr<DecodedImage> ImageDecoder::DecodeRAW(
    const std::filesystem::path& filePath,
    DecoderFlags flags)
{
    // TODO: Implement RAW decoding with libraw
    return nullptr;
}

std::unique_ptr<DecodedImage> ImageDecoder::DecodeMemoryMapped(
    const std::filesystem::path& filePath,
    DecoderFlags flags)
{
    // TODO: Implement memory-mapped file decoding
    // This requires:
    // 1. CreateFileMapping with PAGE_READONLY
    // 2. MapViewOfFile with proper alignment
    // 3. Parse image format directly from mapped memory
    // 4. Return DecodedImage with pointer to mapped view

    return nullptr;
}

} // namespace Core
} // namespace UltraImageViewer
