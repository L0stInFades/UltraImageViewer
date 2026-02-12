#include "rendering/Direct2DRenderer.hpp"
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <dcomp.h>
#include <d2d1helper.h>
#include <ShellScalingApi.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <cstdio>

namespace {
void D2DLog(const char* msg) {
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
    static FILE* f = nullptr;
    if (!f) f = fopen("debug_log.txt", "a");
    if (f) { fprintf(f, "  [D2D] %s\n", msg); fflush(f); }
}
void D2DLogHR(const char* msg, HRESULT hr) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s HRESULT=0x%08lX", msg, static_cast<unsigned long>(hr));
    D2DLog(buf);
}
} // namespace

namespace UltraImageViewer {
namespace Rendering {
namespace {
HRESULT CreateDXGIDevice(Microsoft::WRL::ComPtr<IDXGIDevice>* dxgiDevice)
{
    if (!dxgiDevice) {
        return E_INVALIDARG;
    }

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    UINT numFeatureLevels = static_cast<UINT>(sizeof(featureLevels) / sizeof(featureLevels[0]));

    // Try hardware device, optionally with debug layer
    UINT baseFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;
    HRESULT hr = E_FAIL;

#if defined(_DEBUG)
    // Try with debug layer first (requires D3D SDK debug layer installed)
    hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        baseFlags | D3D11_CREATE_DEVICE_DEBUG,
        featureLevels, numFeatureLevels, D3D11_SDK_VERSION,
        &d3dDevice, nullptr, &d3dContext);
#endif

    // Hardware without debug
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            baseFlags,
            featureLevels, numFeatureLevels, D3D11_SDK_VERSION,
            &d3dDevice, nullptr, &d3dContext);
    }

    // WARP fallback
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            baseFlags,
            featureLevels, numFeatureLevels, D3D11_SDK_VERSION,
            &d3dDevice, nullptr, &d3dContext);
    }

    if (FAILED(hr)) {
        return hr;
    }

    return d3dDevice.As(dxgiDevice);
}
} // namespace

Direct2DRenderer::Direct2DRenderer() = default;
Direct2DRenderer::~Direct2DRenderer() = default;

bool Direct2DRenderer::Initialize(HWND hwnd)
{
    hwnd_ = hwnd;
    D2DLog("Initialize start");

    // Get window DPI
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    UINT dpiX, dpiY;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    dpiX_ = static_cast<float>(dpiX);
    dpiY_ = static_cast<float>(dpiY);

    char buf[128];
    snprintf(buf, sizeof(buf), "DPI: %u x %u", dpiX, dpiY);
    D2DLog(buf);

    // Create Direct2D factory
    D2D1_FACTORY_OPTIONS options = {};
#ifdef _DEBUG
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif

    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_MULTI_THREADED,
        __uuidof(ID2D1Factory3),
        &options,
        reinterpret_cast<void**>(factory_.GetAddressOf())
    );

    if (FAILED(hr)) {
        D2DLogHR("FAIL: D2D1CreateFactory", hr);
        return false;
    }
    D2DLog("OK: D2D1CreateFactory");

    // Create DXGI device and Direct2D device
    hr = CreateDXGIDevice(&dxgiDevice_);
    if (FAILED(hr)) {
        D2DLogHR("FAIL: CreateDXGIDevice", hr);
        return false;
    }
    D2DLog("OK: CreateDXGIDevice");

    ComPtr<ID2D1Device> baseDevice;
    hr = factory_->CreateDevice(dxgiDevice_.Get(), &baseDevice);
    if (FAILED(hr)) {
        D2DLogHR("FAIL: CreateDevice", hr);
        return false;
    }

    hr = baseDevice.As(&device_);
    if (FAILED(hr)) {
        D2DLogHR("FAIL: device As ID2D1Device2", hr);
        return false;
    }
    D2DLog("OK: D2D device created");

    // Create device context
    hr = device_->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_ENABLE_MULTITHREADED_OPTIMIZATIONS,
        &context_
    );

    if (FAILED(hr)) {
        D2DLogHR("FAIL: CreateDeviceContext", hr);
        return false;
    }
    D2DLog("OK: DeviceContext created");

    // Set DPI on context
    context_->SetDpi(dpiX_, dpiY_);

    // Get window size
    RECT rc;
    GetClientRect(hwnd, &rc);
    width_ = rc.right - rc.left;
    height_ = rc.bottom - rc.top;

    snprintf(buf, sizeof(buf), "Client size: %u x %u", width_, height_);
    D2DLog(buf);

    // Create swap chain for DirectComposition
    if (!CreateSwapChain()) {
        D2DLog("FAIL: CreateSwapChain");
        return false;
    }
    D2DLog("OK: SwapChain created");

    // Create DirectComposition target
    if (!CreateCompositionTarget(hwnd_)) {
        D2DLog("FAIL: CreateCompositionTarget");
        return false;
    }
    D2DLog("OK: CompositionTarget created");

    D2DLog("Initialize complete - SUCCESS");
    return true;
}

void Direct2DRenderer::Shutdown()
{
    // Release resources in reverse order
    dcompRoot_.Reset();
    dcompTarget_.Reset();
    dcompDevice_.Reset();
    swapChain_.Reset();
    renderTarget_.Reset();
    context_.Reset();
    device_.Reset();
    dxgiDevice_.Reset();
    factory_.Reset();
}

void Direct2DRenderer::BeginDraw()
{
    if (!context_ || deviceLost_) {
        return;
    }

    // Begin drawing on render target
    if (renderTarget_) {
        context_->SetTarget(renderTarget_.Get());
    }

    context_->BeginDraw();
}

void Direct2DRenderer::EndDraw()
{
    if (!context_ || deviceLost_) {
        return;
    }

    HRESULT hr = context_->EndDraw();

    if (hr == D2DERR_RECREATE_TARGET || hr == static_cast<HRESULT>(DXGI_ERROR_DEVICE_REMOVED)) {
        D2DLog("Device lost detected in EndDraw — recovering");
        HandleDeviceLost();
        return;
    }

    if (SUCCEEDED(hr) && swapChain_) {
        // Present
        DXGI_PRESENT_PARAMETERS params = {};
        params.DirtyRectsCount = 0;
        params.pDirtyRects = nullptr;
        params.pScrollRect = nullptr;
        params.pScrollOffset = nullptr;

        HRESULT presentHr = swapChain_->Present1(1, 0, &params);
        if (presentHr == DXGI_ERROR_DEVICE_REMOVED || presentHr == DXGI_ERROR_DEVICE_RESET) {
            D2DLog("Device lost detected in Present — recovering");
            HandleDeviceLost();
            return;
        }

        // Commit composition
        if (dcompDevice_) {
            dcompDevice_->Commit();
        }
    }
}

void Direct2DRenderer::HandleDeviceLost()
{
    D2DLog("HandleDeviceLost: releasing all resources");
    deviceLost_ = true;

    // Release everything in reverse order
    Shutdown();

    // Attempt to reinitialize
    if (hwnd_ && Initialize(hwnd_)) {
        D2DLog("HandleDeviceLost: recovery successful");
        deviceLost_ = false;
    } else {
        D2DLog("HandleDeviceLost: recovery FAILED — device remains lost");
    }
}

void Direct2DRenderer::Clear(const D2D1_COLOR_F& color)
{
    if (context_) {
        context_->Clear(color);
    }
}

void Direct2DRenderer::SetDpi(float dpiX, float dpiY)
{
    dpiX_ = dpiX;
    dpiY_ = dpiY;
    if (context_) {
        context_->SetDpi(dpiX_, dpiY_);
    }
}

void Direct2DRenderer::DrawImage(
    ID2D1Bitmap* bitmap,
    const D2D1_RECT_F& destRect,
    float opacity,
    D2D1_INTERPOLATION_MODE interpolationMode)
{
    if (!context_ || !bitmap) {
        return;
    }

    D2D1_RECT_F srcRect = {
        0.0f,
        0.0f,
        static_cast<float>(bitmap->GetSize().width),
        static_cast<float>(bitmap->GetSize().height)
    };

    context_->DrawBitmap(
        bitmap,
        &destRect,
        opacity,
        interpolationMode,
        &srcRect,
        nullptr
    );
}

void Direct2DRenderer::DrawImageWithTransform(
    ID2D1Bitmap* bitmap,
    const D2D1_MATRIX_3X2_F& transform,
    const D2D1_RECT_F* srcRect)
{
    if (!context_ || !bitmap) {
        return;
    }

    context_->SetTransform(transform);

    D2D1_RECT_F defaultSrcRect = {
        0.0f,
        0.0f,
        static_cast<float>(bitmap->GetSize().width),
        static_cast<float>(bitmap->GetSize().height)
    };

    context_->DrawBitmap(
        bitmap,
        nullptr,
        1.0f,
        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
        srcRect ? srcRect : &defaultSrcRect,
        nullptr
    );

    // Reset transform
    D2D1_MATRIX_3X2_F identity = D2D1::Matrix3x2F::Identity();
    context_->SetTransform(identity);
}

ComPtr<ID2D1Bitmap> Direct2DRenderer::CreateBitmap(
    uint32_t width,
    uint32_t height,
    const void* pixelData,
    DXGI_FORMAT format)
{
    if (!context_) {
        return nullptr;
    }

    D2D1_BITMAP_PROPERTIES1 props = {};
    props.pixelFormat.format = format;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.dpiX = dpiX_;
    props.dpiY = dpiY_;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
    props.colorContext = nullptr;

    ComPtr<ID2D1Bitmap1> bitmap;

    D2D1_SIZE_U size = { width, height };

    HRESULT hr = context_->CreateBitmap(
        size,
        pixelData,
        width * 4, // Pitch (assuming 32-bit BGRA)
        &props,
        &bitmap
    );

    if (FAILED(hr)) {
        return nullptr;
    }

    // Convert to ID2D1Bitmap
    ComPtr<ID2D1Bitmap> result;
    bitmap->QueryInterface(__uuidof(ID2D1Bitmap), &result);

    return result;
}

ComPtr<ID2D1Bitmap1> Direct2DRenderer::CreateBitmapFromWIC(IWICBitmapSource* wicSource)
{
    if (!context_ || !wicSource) {
        return nullptr;
    }

    ComPtr<ID2D1Bitmap1> bitmap;

    HRESULT hr = context_->CreateBitmapFromWicBitmap(
        wicSource,
        nullptr,
        &bitmap
    );

    if (FAILED(hr)) {
        return nullptr;
    }

    return bitmap;
}

bool Direct2DRenderer::CreateSwapChain()
{
    D2DLog("CreateSwapChain start");
    if (!dxgiDevice_ || !hwnd_) {
        D2DLog("CreateSwapChain: no dxgiDevice or hwnd");
        return false;
    }

    // Get DXGI adapter
    ComPtr<IDXGIAdapter> dxgiAdapter;
    HRESULT qhr = dxgiDevice_->GetAdapter(&dxgiAdapter);
    if (FAILED(qhr)) {
        D2DLogHR("FAIL: GetAdapter", qhr);
        return false;
    }
    D2DLog("OK: got adapter");

    // Get DXGI factory
    ComPtr<IDXGIFactory2> dxgiFactory;
    qhr = dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
    if (FAILED(qhr)) {
        D2DLogHR("FAIL: GetParent(IDXGIFactory2)", qhr);
        return false;
    }
    D2DLog("OK: got factory");

    // Create swap chain
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = width_;
    swapChainDesc.Height = height_;
    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = 2;
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    swapChainDesc.Flags = 0;

    D2DLog("Calling CreateSwapChainForComposition...");
    HRESULT hr = dxgiFactory->CreateSwapChainForComposition(
        dxgiDevice_.Get(),
        &swapChainDesc,
        nullptr,
        &swapChain_
    );
    D2DLogHR("CreateSwapChainForComposition result", hr);

    if (FAILED(hr)) {
        return false;
    }

    // Create render target from swap chain
    ComPtr<IDXGISurface> surface;
    swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface));

    D2D1_BITMAP_PROPERTIES1 props = {};
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.dpiX = dpiX_;
    props.dpiY = dpiY_;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

    hr = context_->CreateBitmapFromDxgiSurface(
        surface.Get(),
        &props,
        &renderTarget_
    );

    return SUCCEEDED(hr);
}

bool Direct2DRenderer::CreateCompositionTarget(HWND hwnd)
{
    if (!hwnd) {
        return false;
    }

    // Create DirectComposition device
    HRESULT hr = DCompositionCreateDevice(
        nullptr,
        IID_PPV_ARGS(&dcompDevice_)
    );

    if (FAILED(hr)) {
        return false;
    }

    // Create composition target for window
    hr = dcompDevice_->CreateTargetForHwnd(hwnd, TRUE, &dcompTarget_);

    if (FAILED(hr)) {
        return false;
    }

    // Create root visual
    hr = dcompDevice_->CreateVisual(&dcompRoot_);

    if (FAILED(hr)) {
        return false;
    }

    // Set swap chain as content of root visual
    ComPtr<IDXGISwapChain> swapChainAsIDXGI;
    swapChain_->QueryInterface(IID_PPV_ARGS(&swapChainAsIDXGI));

    hr = dcompRoot_->SetContent(swapChainAsIDXGI.Get());

    if (FAILED(hr)) {
        return false;
    }

    // Set root visual on target
    hr = dcompTarget_->SetRoot(dcompRoot_.Get());

    if (FAILED(hr)) {
        return false;
    }

    return true;
}

void Direct2DRenderer::UpdateComposition()
{
    if (dcompDevice_) {
        dcompDevice_->Commit();
    }
}

void Direct2DRenderer::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0 || deviceLost_) {
        return;
    }

    if (width == width_ && height == height_) {
        return;
    }

    width_ = width;
    height_ = height;

    if (swapChain_ && context_) {
        // Must release all references to back buffer before ResizeBuffers
        context_->SetTarget(nullptr);
        renderTarget_.Reset();

        HRESULT hr = swapChain_->ResizeBuffers(
            2,
            width,
            height,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            0
        );

        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            HandleDeviceLost();
            return;
        }

        if (SUCCEEDED(hr)) {
            // Recreate render target from new back buffer
            ComPtr<IDXGISurface> surface;
            swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface));

            D2D1_BITMAP_PROPERTIES1 props = {};
            props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
            props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
            props.dpiX = dpiX_;
            props.dpiY = dpiY_;
            props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

            HRESULT bmpHr = context_->CreateBitmapFromDxgiSurface(
                surface.Get(),
                &props,
                &renderTarget_
            );
            if (FAILED(bmpHr)) {
                D2DLogHR("FAIL: CreateBitmapFromDxgiSurface in Resize", bmpHr);
            }
        }
    }

    // Update composition visual size
    if (dcompRoot_) {
        dcompRoot_->SetOffsetX(0.0f);
        dcompRoot_->SetOffsetY(0.0f);
    }
}

ComPtr<ID2D1Bitmap1> Direct2DRenderer::CreateOffscreenBitmap(uint32_t w, uint32_t h)
{
    if (!context_ || w == 0 || h == 0) return nullptr;

    D2D1_BITMAP_PROPERTIES1 props = {};
    props.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;  // TARGET only, no CANNOT_DRAW
    props.dpiX = dpiX_;
    props.dpiY = dpiY_;

    ComPtr<ID2D1Bitmap1> bitmap;
    HRESULT hr = context_->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &props, &bitmap);
    if (FAILED(hr)) return nullptr;
    return bitmap;
}

ComPtr<ID2D1SolidColorBrush> Direct2DRenderer::CreateBrush(const D2D1_COLOR_F& color)
{
    if (!context_) {
        return nullptr;
    }

    ComPtr<ID2D1SolidColorBrush> brush;
    context_->CreateSolidColorBrush(color, &brush);
    return brush;
}

ComPtr<IDWriteTextFormat> Direct2DRenderer::CreateTextFormat(
    const wchar_t* fontFamily,
    float fontSize,
    DWRITE_FONT_WEIGHT weight)
{
    if (!context_) {
        return nullptr;
    }

    ComPtr<IDWriteFactory> writeFactory;
    HRESULT hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(writeFactory.GetAddressOf())
    );

    if (FAILED(hr)) {
        return nullptr;
    }

    ComPtr<IDWriteTextFormat> textFormat;
    hr = writeFactory->CreateTextFormat(
        fontFamily,
        nullptr,
        weight,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        fontSize,
        L"en-us",
        &textFormat
    );

    if (FAILED(hr)) {
        return nullptr;
    }

    return textFormat;
}

// MipMapGenerator implementation
std::vector<ComPtr<ID2D1Bitmap>> MipMapGenerator::GenerateMipChain(
    ID2D1DeviceContext* context,
    ID2D1Bitmap* sourceBitmap,
    uint32_t maxLevels)
{
    std::vector<ComPtr<ID2D1Bitmap>> mipChain;

    if (!context || !sourceBitmap) {
        return mipChain;
    }

    D2D1_SIZE_U size = sourceBitmap->GetPixelSize();
    uint32_t levels = maxLevels > 0 ? maxLevels : CalculateMaxMipLevels(size.width, size.height);

    mipChain.reserve(levels);
    mipChain.push_back(sourceBitmap);

    for (uint32_t level = 1; level < levels; ++level) {
        ComPtr<ID2D1Bitmap> mipLevel = GenerateMipLevel(context, sourceBitmap, level);
        if (mipLevel) {
            mipChain.push_back(mipLevel);
        } else {
            break;
        }
    }

    return mipChain;
}

ComPtr<ID2D1Bitmap> MipMapGenerator::GenerateMipLevel(
    ID2D1DeviceContext* context,
    ID2D1Bitmap* source,
    uint32_t level)
{
    if (!context || !source || level == 0) {
        return nullptr;
    }

    D2D1_SIZE_U sourceSize = source->GetPixelSize();
    uint32_t scale = 1 << level;
    uint32_t mipWidth = std::max(1u, sourceSize.width / scale);
    uint32_t mipHeight = std::max(1u, sourceSize.height / scale);

    FLOAT dpiX = 96.0f;
    FLOAT dpiY = 96.0f;
    context->GetDpi(&dpiX, &dpiY);
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(source->GetPixelFormat(), dpiX, dpiY);

    ComPtr<ID2D1Bitmap> mipBitmap;
    HRESULT hr = context->CreateBitmap(
        D2D1::SizeU(mipWidth, mipHeight),
        nullptr,
        0,
        &props,
        &mipBitmap
    );

    if (FAILED(hr)) {
        return nullptr;
    }

    // Draw scaled down
    Microsoft::WRL::ComPtr<ID2D1Image> oldTarget;
    context->GetTarget(&oldTarget);
    context->SetTarget(mipBitmap.Get());
    context->BeginDraw();
    context->Clear(D2D1::ColorF(0, 0, 0, 0));
    context->DrawBitmap(
        source,
        D2D1::RectF(0, 0, static_cast<float>(mipWidth), static_cast<float>(mipHeight)),
        1.0f,
        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
    );
    context->EndDraw();
    context->SetTarget(oldTarget.Get());

    return mipBitmap;
}

uint32_t MipMapGenerator::CalculateMaxMipLevels(uint32_t width, uint32_t height)
{
    uint32_t maxDimension = std::max(width, height);
    uint32_t levels = 1;

    while (maxDimension > 1) {
        maxDimension >>= 1;
        levels++;
    }

    return levels;
}

// Viewport implementation
Viewport::Viewport(uint32_t windowWidth, uint32_t windowHeight)
    : windowWidth_(windowWidth)
    , windowHeight_(windowHeight)
{
    UpdateMatrix();
}

void Viewport::SetZoom(float zoom, float centerX, float centerY)
{
    // Calculate zoom around center point
    float oldZoom = transform_.zoom;
    transform_.zoom = std::max(0.01f, std::min(zoom, 100.0f));

    // Adjust offset to zoom around point
    transform_.offsetX = centerX - (centerX - transform_.offsetX) * (transform_.zoom / oldZoom);
    transform_.offsetY = centerY - (centerY - transform_.offsetY) * (transform_.zoom / oldZoom);

    UpdateMatrix();
}

void Viewport::Pan(float deltaX, float deltaY)
{
    transform_.offsetX += deltaX;
    transform_.offsetY += deltaY;

    ClampTransform();
    UpdateMatrix();
}

void Viewport::Rotate(float angle)
{
    transform_.rotation += angle;

    // Normalize to 0-360
    while (transform_.rotation >= 360.0f) transform_.rotation -= 360.0f;
    while (transform_.rotation < 0.0f) transform_.rotation += 360.0f;

    UpdateMatrix();
}

D2D1_POINT_2F Viewport::ScreenToImage(const D2D1_POINT_2F& screenPoint) const
{
    D2D1_MATRIX_3X2_F inverse = matrix_;
    if (D2D1InvertMatrix(&inverse)) {
        D2D1::Matrix3x2F invMatrix(
            inverse.m11, inverse.m12, inverse.m21, inverse.m22, inverse.dx, inverse.dy);
        return invMatrix.TransformPoint(screenPoint);
    }

    return screenPoint;
}

D2D1_POINT_2F Viewport::ImageToScreen(const D2D1_POINT_2F& imagePoint) const
{
    D2D1::Matrix3x2F mat(
        matrix_.m11, matrix_.m12, matrix_.m21, matrix_.m22, matrix_.dx, matrix_.dy);
    return mat.TransformPoint(imagePoint);
}

bool Viewport::IsVisible(const D2D1_RECT_F& imageRect) const
{
    // Transform rect corners to screen space
    D2D1_POINT_2F topLeft = { imageRect.left, imageRect.top };
    D2D1_POINT_2F bottomRight = { imageRect.right, imageRect.bottom };

    D2D1_POINT_2F screenTopLeft = ImageToScreen(topLeft);
    D2D1_POINT_2F screenBottomRight = ImageToScreen(bottomRight);

    // Check if intersects with screen bounds
    return !(screenBottomRight.x < 0 || screenTopLeft.x > windowWidth_ ||
             screenBottomRight.y < 0 || screenTopLeft.y > windowHeight_);
}

uint32_t Viewport::SelectMipLevel(const D2D1_SIZE_F& imageSize) const
{
    // Calculate image size in screen space
    float screenScale = transform_.zoom;

    // Base level for 1:1 pixel mapping
    if (screenScale >= 1.0f) {
        return 0;
    }

    // Select mip level based on scale
    float level = std::floor(-std::log2(screenScale));
    return static_cast<uint32_t>(std::max(0.0f, level));
}

D2D1_MATRIX_3X2_F Viewport::GetTransform() const
{
    return matrix_;
}

void Viewport::ClampTransform()
{
    // Optional: Clamp offset/zoom to reasonable values
    transform_.offsetX = std::max(-10000.0f, std::min(10000.0f, transform_.offsetX));
    transform_.offsetY = std::max(-10000.0f, std::min(10000.0f, transform_.offsetY));
}

void Viewport::UpdateMatrix()
{
    // Build transform matrix: translate * rotate * scale
    D2D1_MATRIX_3X2_F translation = D2D1::Matrix3x2F::Translation(
        transform_.offsetX,
        transform_.offsetY
    );

    D2D1_MATRIX_3X2_F rotation = D2D1::Matrix3x2F::Rotation(
        transform_.rotation,
        D2D1::Point2F(windowWidth_ / 2.0f, windowHeight_ / 2.0f)
    );

    D2D1_MATRIX_3X2_F scale = D2D1::Matrix3x2F::Scale(
        transform_.zoom,
        transform_.zoom,
        D2D1::Point2F(windowWidth_ / 2.0f, windowHeight_ / 2.0f)
    );

    matrix_ = translation * rotation * scale;
}

} // namespace Rendering
} // namespace UltraImageViewer
