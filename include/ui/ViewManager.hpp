#pragma once

#include <d2d1.h>
#include "../animation/AnimationEngine.hpp"
#include "../rendering/Direct2DRenderer.hpp"
#include "../core/ImagePipeline.hpp"
#include "GalleryView.hpp"
#include "ImageViewer.hpp"
#include "TransitionController.hpp"
#include "GestureHandler.hpp"

namespace UltraImageViewer {
namespace UI {

enum class ViewState {
    Gallery,
    Viewer,
    Transition
};

class ViewManager {
public:
    ViewManager();
    ~ViewManager();

    void Initialize(Rendering::Direct2DRenderer* renderer,
                    Animation::AnimationEngine* engine,
                    Core::ImagePipeline* pipeline);

    void SetState(ViewState state);
    ViewState GetState() const { return state_; }

    void Render(Rendering::Direct2DRenderer* renderer);
    void Update(float deltaTime);

    // View transitions
    void TransitionToViewer(size_t imageIndex, D2D1_RECT_F fromRect);
    void TransitionToGallery();

    // Input routing
    void OnMouseWheel(float delta, float x, float y);
    void OnKeyDown(UINT key);
    void OnMouseDown(float x, float y);
    void OnMouseMove(float x, float y);
    void OnMouseUp(float x, float y);
    void OnMiddleMouseDown(float x, float y);
    void OnMiddleMouseUp(float x, float y);
    void OnGesture(const GestureEventArgs& args);

    // View size
    void SetViewSize(float width, float height);

    // Access views
    GalleryView* GetGalleryView() { return &galleryView_; }
    ImageViewer* GetImageViewer() { return &imageViewer_; }

    // Need render?
    bool NeedsRender() const { return needsRender_; }
    void SetNeedsRender() { needsRender_ = true; }

private:
    ViewState state_ = ViewState::Gallery;
    ViewState pendingState_ = ViewState::Gallery;

    GalleryView galleryView_;
    ImageViewer imageViewer_;
    TransitionController transition_;

    Animation::AnimationEngine* animEngine_ = nullptr;
    Core::ImagePipeline* pipeline_ = nullptr;

    float viewWidth_ = 1280.0f;
    float viewHeight_ = 720.0f;
    bool needsRender_ = true;
};

} // namespace UI
} // namespace UltraImageViewer
