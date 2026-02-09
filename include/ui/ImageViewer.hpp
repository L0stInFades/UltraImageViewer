#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <vector>
#include <filesystem>
#include <functional>
#include "../animation/SpringAnimation.hpp"
#include "../animation/AnimationEngine.hpp"
#include "../rendering/Direct2DRenderer.hpp"
#include "../core/ImagePipeline.hpp"

namespace UltraImageViewer {
namespace UI {

class ImageViewer {
public:
    ImageViewer();
    ~ImageViewer();

    void Initialize(Rendering::Direct2DRenderer* renderer,
                    Core::ImagePipeline* pipeline,
                    Animation::AnimationEngine* engine);

    void SetImages(const std::vector<std::filesystem::path>& paths, size_t startIndex);
    size_t GetCurrentIndex() const { return currentIndex_; }
    const std::vector<std::filesystem::path>& GetImages() const { return images_; }

    void Render(Rendering::Direct2DRenderer* renderer, bool overlayMode = false);
    void Update(float deltaTime);

    // Whether dismiss gesture is active (for ViewManager to render gallery behind)
    bool IsDismissActive() const;

    // Mouse/gesture interaction
    void OnMouseDown(float x, float y);
    void OnMouseMove(float x, float y);
    void OnMouseUp(float x, float y);
    void OnMouseWheel(float delta, float x, float y);
    void OnMiddleMouseDown(float x, float y);
    void OnMiddleMouseUp(float x, float y);
    void OnKeyDown(UINT key);

    // Get current image rect (for hero transition return)
    D2D1_RECT_F GetCurrentImageRect() const;

    // Get actual on-screen rect (including dismiss offset, zoom, pan, scale)
    D2D1_RECT_F GetCurrentScreenRect() const;
    float GetCurrentBgAlpha() const;

    // Dismiss callback (back to gallery)
    using DismissCallback = std::function<void(size_t index)>;
    void SetDismissCallback(DismissCallback cb) { dismissCallback_ = std::move(cb); }

    void SetViewSize(float width, float height);

    // Navigate
    void GoToIndex(size_t index);
    void GoNext();
    void GoPrev();

private:
    // Calculate the fit rect for an image in the viewport
    D2D1_RECT_F CalculateFitRect(float imgW, float imgH) const;
    float CalculateFitZoom(float imgW, float imgH) const;
    D2D1_RECT_F CalculatePanBounds() const;

    void LoadCurrentPage();
    void NavigateToPage(int direction);

    // Image data
    std::vector<std::filesystem::path> images_;
    size_t currentIndex_ = 0;

    // Cached bitmaps for current, prev, next
    Microsoft::WRL::ComPtr<ID2D1Bitmap> currentBitmap_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> prevBitmap_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> nextBitmap_;

    // Horizontal paging
    Animation::SpringAnimation pageOffsetX_;
    bool isPaging_ = false;
    float pageStartX_ = 0.0f;
    float pageDragStartX_ = 0.0f;

    // Zoom and pan
    float zoom_ = 1.0f;
    float fitZoom_ = 1.0f;
    float panX_ = 0.0f;
    float panY_ = 0.0f;
    Animation::SpringAnimation zoomSpring_;
    Animation::SpringAnimation panXSpring_;
    Animation::SpringAnimation panYSpring_;

    // Double-tap smart zoom
    bool isZoomedIn_ = false;

    // Dismiss (pull-down to exit)
    float dismissOffsetY_ = 0.0f;
    float dismissStartY_ = 0.0f;
    bool isDismissing_ = false;
    Animation::SpringAnimation dismissSpring_;

    // Interaction state
    bool isMiddleDragging_ = false;
    bool isMouseDown_ = false;
    float mouseDownX_ = 0.0f;
    float mouseDownY_ = 0.0f;
    float lastMouseX_ = 0.0f;
    float lastMouseY_ = 0.0f;
    float mouseVelocityX_ = 0.0f;
    float mouseVelocityY_ = 0.0f;
    bool hasDragged_ = false;
    ULONGLONG lastClickTime_ = 0;
    float lastClickX_ = 0.0f;
    float lastClickY_ = 0.0f;

    // View dimensions
    float viewWidth_ = 1280.0f;
    float viewHeight_ = 720.0f;

    // Callback
    DismissCallback dismissCallback_;

    // Pipeline
    Core::ImagePipeline* pipeline_ = nullptr;
    Animation::AnimationEngine* engine_ = nullptr;

    // Rendering resources
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> overlayTextBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> overlayBgBrush_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> counterFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> filenameFormat_;
    bool resourcesCreated_ = false;
    void EnsureResources(Rendering::Direct2DRenderer* renderer);
};

} // namespace UI
} // namespace UltraImageViewer
