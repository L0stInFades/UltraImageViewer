#pragma once

#include <d2d1_3.h>
#include <dwrite_3.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <memory>
#include <vector>
#include <filesystem>

namespace UltraImageViewer {
namespace Rendering {

using namespace Microsoft::WRL;

/**
 * Direct2D renderer with DirectComposition integration
 * Features:
 * - Hardware-accelerated 2D rendering
 * - Per-monitor DPI awareness
 * - Smooth zooming and panning
 * - Effect pipeline
 */
class Direct2DRenderer {
public:
    Direct2DRenderer();
    ~Direct2DRenderer();

    // Initialization
    bool Initialize(HWND hwnd);
    void Shutdown();

    // Rendering
    void BeginDraw();
    void EndDraw();
    void Clear(const D2D1_COLOR_F& color);

    // Image rendering
    void DrawImage(
        ID2D1Bitmap* bitmap,
        const D2D1_RECT_F& destRect,
        float opacity = 1.0f,
        D2D1_INTERPOLATION_MODE interpolationMode = D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
    );

    void DrawImageWithTransform(
        ID2D1Bitmap* bitmap,
        const D2D1_MATRIX_3X2_F& transform,
        const D2D1_RECT_F* srcRect = nullptr
    );

    // Bitmap creation
    ComPtr<ID2D1Bitmap> CreateBitmap(
        uint32_t width,
        uint32_t height,
        const void* pixelData,
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM
    );

    ComPtr<ID2D1Bitmap1> CreateBitmapFromWIC(IWICBitmapSource* wicSource);

    // DirectComposition
    bool CreateCompositionTarget(HWND hwnd);
    void UpdateComposition();

    // DPI handling
    float GetDpiX() const { return dpiX_; }
    float GetDpiY() const { return dpiY_; }
    void SetDpi(float dpiX, float dpiY);

    // Device management
    void Resize(uint32_t width, uint32_t height);
    bool IsDeviceLost() const { return deviceLost_; }
    void HandleDeviceLost();
    ID2D1DeviceContext* GetContext() const { return context_.Get(); }
    ID2D1Factory3* GetFactory() const { return factory_.Get(); }

    // Offscreen bitmap for glass effects (render content → read back for blur)
    ComPtr<ID2D1Bitmap1> CreateOffscreenBitmap(uint32_t w, uint32_t h);
    ID2D1Bitmap1* GetRenderTarget() const { return renderTarget_.Get(); }

    // Resources
    ComPtr<ID2D1SolidColorBrush> CreateBrush(const D2D1_COLOR_F& color);
    ComPtr<IDWriteTextFormat> CreateTextFormat(
        const wchar_t* fontFamily,
        float fontSize,
        DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL
    );

private:
    bool CreateSwapChain();
    // Factory and device
    ComPtr<ID2D1Factory3> factory_;
    ComPtr<ID2D1Device2> device_;
    ComPtr<ID2D1DeviceContext2> context_;
    ComPtr<IDXGIDevice> dxgiDevice_;  // Keep reference for swap chain creation

    // DirectComposition
    ComPtr<IDCompositionDevice> dcompDevice_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual> dcompRoot_;

    // Swap chain for DirectComposition
    ComPtr<IDXGISwapChain1> swapChain_;

    // Window
    HWND hwnd_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    // DPI
    float dpiX_ = 96.0f;
    float dpiY_ = 96.0f;

    // Render target
    ComPtr<ID2D1Bitmap1> renderTarget_;

    // Device lost recovery
    bool deviceLost_ = false;
};

/**
 * Mipmap chain generator for smooth zooming
 * Creates pre-filtered mipmaps for each texture level
 */
class MipMapGenerator {
public:
    static std::vector<ComPtr<ID2D1Bitmap>> GenerateMipChain(
        ID2D1DeviceContext* context,
        ID2D1Bitmap* sourceBitmap,
        uint32_t maxLevels = 0 // 0 = auto (based on size)
    );

    static ComPtr<ID2D1Bitmap> GenerateMipLevel(
        ID2D1DeviceContext* context,
        ID2D1Bitmap* source,
        uint32_t level
    );

private:
    static uint32_t CalculateMaxMipLevels(uint32_t width, uint32_t height);
};

/**
 * Smart viewport with culling and LOD selection
 */
class Viewport {
public:
    struct ViewTransform {
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        float zoom = 1.0f;
        float rotation = 0.0f;
    };

    Viewport(uint32_t windowWidth, uint32_t windowHeight);

    // Transform management
    void SetZoom(float zoom, float centerX, float centerY);
    void Pan(float deltaX, float deltaY);
    void Rotate(float angle);

    // Coordinate conversion
    D2D1_POINT_2F ScreenToImage(const D2D1_POINT_2F& screenPoint) const;
    D2D1_POINT_2F ImageToScreen(const D2D1_POINT_2F& imagePoint) const;

    // Visibility testing
    bool IsVisible(const D2D1_RECT_F& imageRect) const;

    // Mipmap level selection
    uint32_t SelectMipLevel(const D2D1_SIZE_F& imageSize) const;

    // Get current transform matrix
    D2D1_MATRIX_3X2_F GetTransform() const;

    // Get transform state
    const ViewTransform& GetTransformState() const { return transform_; }

private:
    void ClampTransform();
    void UpdateMatrix();

    ViewTransform transform_;
    D2D1_MATRIX_3X2_F matrix_;
    uint32_t windowWidth_;
    uint32_t windowHeight_;
};

} // namespace Rendering
} // namespace UltraImageViewer
