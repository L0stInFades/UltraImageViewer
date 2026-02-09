#include "ui/ImageViewer.hpp"
#include "ui/Theme.hpp"
#include <algorithm>
#include <cmath>

namespace UltraImageViewer {
namespace UI {

using Animation::SpringConfig;

static constexpr SpringConfig kPageSpring{300.0f, 28.0f, 1.0f, 0.5f};
static constexpr SpringConfig kZoomSpring{250.0f, 24.0f, 1.0f, 0.01f};
static constexpr SpringConfig kPanSpring{200.0f, 22.0f, 1.0f, 0.5f};
static constexpr SpringConfig kDismissSpring{300.0f, 25.0f, 1.0f, 0.5f};
static constexpr float kDismissThreshold = 100.0f;
static constexpr float kPageThreshold = 0.25f;  // Fraction of view width to trigger page change
static constexpr float kDragThreshold = 5.0f;
static constexpr ULONGLONG kDoubleTapMs = 300;

ImageViewer::ImageViewer()
    : pageOffsetX_(kPageSpring)
    , zoomSpring_(kZoomSpring)
    , panXSpring_(kPanSpring)
    , panYSpring_(kPanSpring)
    , dismissSpring_(kDismissSpring)
{
    pageOffsetX_.SetValue(0.0f);
    pageOffsetX_.SetTarget(0.0f);
    pageOffsetX_.SnapToTarget();
    zoomSpring_.SetValue(1.0f);
    zoomSpring_.SetTarget(1.0f);
    zoomSpring_.SnapToTarget();
    dismissSpring_.SetValue(0.0f);
    dismissSpring_.SetTarget(0.0f);
    dismissSpring_.SnapToTarget();
}

ImageViewer::~ImageViewer() = default;

void ImageViewer::Initialize(Rendering::Direct2DRenderer* renderer,
                              Core::ImagePipeline* pipeline,
                              Animation::AnimationEngine* engine)
{
    pipeline_ = pipeline;
    engine_ = engine;
    EnsureResources(renderer);
}

void ImageViewer::EnsureResources(Rendering::Direct2DRenderer* renderer)
{
    if (resourcesCreated_ || !renderer) return;
    bgBrush_ = renderer->CreateBrush(Theme::ViewerBg);
    overlayTextBrush_ = renderer->CreateBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.9f));
    overlayBgBrush_ = renderer->CreateBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f));
    counterFormat_ = renderer->CreateTextFormat(L"Segoe UI", 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    filenameFormat_ = renderer->CreateTextFormat(L"Segoe UI", 13.0f);
    if (counterFormat_) {
        counterFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        counterFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (filenameFormat_) {
        filenameFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        filenameFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    resourcesCreated_ = true;
}

void ImageViewer::SetImages(const std::vector<std::filesystem::path>& paths, size_t startIndex)
{
    images_ = paths;
    currentIndex_ = std::min(startIndex, images_.empty() ? 0 : images_.size() - 1);

    zoom_ = 1.0f;
    panX_ = 0.0f;
    panY_ = 0.0f;
    isZoomedIn_ = false;

    zoomSpring_.SetValue(1.0f);
    zoomSpring_.SetTarget(1.0f);
    zoomSpring_.SnapToTarget();
    panXSpring_.SetValue(0.0f);
    panXSpring_.SetTarget(0.0f);
    panXSpring_.SnapToTarget();
    panYSpring_.SetValue(0.0f);
    panYSpring_.SetTarget(0.0f);
    panYSpring_.SnapToTarget();
    pageOffsetX_.SetValue(0.0f);
    pageOffsetX_.SetTarget(0.0f);
    pageOffsetX_.SnapToTarget();
    dismissSpring_.SetValue(0.0f);
    dismissSpring_.SetTarget(0.0f);
    dismissSpring_.SnapToTarget();

    LoadCurrentPage();
}

void ImageViewer::LoadCurrentPage()
{
    if (!pipeline_ || images_.empty()) return;

    currentBitmap_ = pipeline_->GetBitmap(images_[currentIndex_]);
    prevBitmap_ = (currentIndex_ > 0) ? pipeline_->GetThumbnail(images_[currentIndex_ - 1]) : nullptr;
    nextBitmap_ = (currentIndex_ + 1 < images_.size()) ? pipeline_->GetThumbnail(images_[currentIndex_ + 1]) : nullptr;

    // Prefetch full-res neighbors
    if (currentIndex_ > 0) {
        pipeline_->GetBitmapAsync(images_[currentIndex_ - 1], [this](auto bmp) {
            prevBitmap_ = bmp;
        });
    }
    if (currentIndex_ + 1 < images_.size()) {
        pipeline_->GetBitmapAsync(images_[currentIndex_ + 1], [this](auto bmp) {
            nextBitmap_ = bmp;
        });
    }
}

D2D1_RECT_F ImageViewer::CalculateFitRect(float imgW, float imgH) const
{
    if (imgW <= 0 || imgH <= 0) return D2D1::RectF(0, 0, 0, 0);

    float scaleX = viewWidth_ / imgW;
    float scaleY = viewHeight_ / imgH;
    float scale = std::min(scaleX, scaleY);

    float w = imgW * scale;
    float h = imgH * scale;
    float x = (viewWidth_ - w) * 0.5f;
    float y = (viewHeight_ - h) * 0.5f;

    return D2D1::RectF(x, y, x + w, y + h);
}

float ImageViewer::CalculateFitZoom(float imgW, float imgH) const
{
    if (imgW <= 0 || imgH <= 0) return 1.0f;
    float scaleX = viewWidth_ / imgW;
    float scaleY = viewHeight_ / imgH;
    return std::min(scaleX, scaleY);
}

D2D1_RECT_F ImageViewer::CalculatePanBounds() const
{
    if (!currentBitmap_) return D2D1::RectF(0, 0, 0, 0);
    auto size = currentBitmap_->GetSize();
    D2D1_RECT_F fitRect = CalculateFitRect(size.width, size.height);
    float fitW = fitRect.right - fitRect.left;
    float fitH = fitRect.bottom - fitRect.top;
    float zoomedW = fitW * zoom_;
    float zoomedH = fitH * zoom_;
    float excessX = std::max(0.0f, (zoomedW - viewWidth_) * 0.5f);
    float excessY = std::max(0.0f, (zoomedH - viewHeight_) * 0.5f);
    return D2D1::RectF(-excessX, -excessY, excessX, excessY);
}

bool ImageViewer::IsDismissActive() const
{
    return std::abs(dismissSpring_.GetValue()) > 1.0f;
}

void ImageViewer::Render(Rendering::Direct2DRenderer* renderer, bool overlayMode)
{
    if (!renderer) return;
    EnsureResources(renderer);

    auto* ctx = renderer->GetContext();
    if (!ctx) return;

    float dismissY = dismissSpring_.GetValue();
    float dismissScale = 1.0f - std::abs(dismissY) / (viewHeight_ * 2.0f);
    dismissScale = std::max(0.5f, dismissScale);

    // Background with opacity based on dismiss
    float bgAlpha = 1.0f - std::abs(dismissY) / (viewHeight_ * 0.5f);
    bgAlpha = std::max(0.0f, std::min(1.0f, bgAlpha));

    if (overlayMode) {
        // Draw semi-transparent black overlay on top of gallery
        if (bgBrush_) {
            bgBrush_->SetOpacity(bgAlpha);
            ctx->FillRectangle(D2D1::RectF(0, 0, viewWidth_, viewHeight_), bgBrush_.Get());
            bgBrush_->SetOpacity(1.0f);
        }
    } else {
        D2D1_COLOR_F bgColor = {0.0f, 0.0f, 0.0f, bgAlpha};
        ctx->Clear(bgColor);
    }

    float pageOffset = pageOffsetX_.GetValue();
    float currentZoom = zoomSpring_.GetValue();
    float currentPanX = panXSpring_.GetValue();
    float currentPanY = panYSpring_.GetValue();

    // Draw previous page
    if (prevBitmap_) {
        auto size = prevBitmap_->GetSize();
        D2D1_RECT_F rect = CalculateFitRect(size.width, size.height);
        float offsetX = pageOffset - viewWidth_;
        D2D1_RECT_F destRect = D2D1::RectF(
            rect.left + offsetX, rect.top, rect.right + offsetX, rect.bottom);
        renderer->DrawImage(prevBitmap_.Get(), destRect, 1.0f);
    }

    // Draw current page
    if (currentBitmap_) {
        auto size = currentBitmap_->GetSize();
        D2D1_RECT_F fitRect = CalculateFitRect(size.width, size.height);
        fitZoom_ = CalculateFitZoom(size.width, size.height);

        float centerX = viewWidth_ * 0.5f;
        float centerY = viewHeight_ * 0.5f;
        float w = (fitRect.right - fitRect.left) * currentZoom;
        float h = (fitRect.bottom - fitRect.top) * currentZoom;

        D2D1_RECT_F destRect = D2D1::RectF(
            centerX - w * 0.5f + currentPanX + pageOffset,
            centerY - h * 0.5f + currentPanY + dismissY,
            centerX + w * 0.5f + currentPanX + pageOffset,
            centerY + h * 0.5f + currentPanY + dismissY
        );

        // Apply dismiss scale
        if (dismissScale < 1.0f) {
            float dw = (destRect.right - destRect.left) * (1.0f - dismissScale) * 0.5f;
            float dh = (destRect.bottom - destRect.top) * (1.0f - dismissScale) * 0.5f;
            destRect.left += dw;
            destRect.top += dh;
            destRect.right -= dw;
            destRect.bottom -= dh;
        }

        renderer->DrawImage(currentBitmap_.Get(), destRect, 1.0f);
    }

    // Draw next page
    if (nextBitmap_) {
        auto size = nextBitmap_->GetSize();
        D2D1_RECT_F rect = CalculateFitRect(size.width, size.height);
        float offsetX = pageOffset + viewWidth_;
        D2D1_RECT_F destRect = D2D1::RectF(
            rect.left + offsetX, rect.top, rect.right + offsetX, rect.bottom);
        renderer->DrawImage(nextBitmap_.Get(), destRect, 1.0f);
    }

    // Draw overlay (filename + counter)
    float overlayAlpha = 1.0f - std::abs(dismissY) / (viewHeight_ * 0.3f);
    overlayAlpha = std::max(0.0f, std::min(1.0f, overlayAlpha));

    if (overlayAlpha > 0.01f && !images_.empty()) {
        // Top bar gradient background (taller for better readability)
        if (overlayBgBrush_) {
            overlayBgBrush_->SetOpacity(overlayAlpha * 0.5f);
            D2D1_RECT_F topBar = D2D1::RectF(0, 0, viewWidth_, 44.0f);
            ctx->FillRectangle(topBar, overlayBgBrush_.Get());
            overlayBgBrush_->SetOpacity(1.0f);
        }

        // Text shadow brush for readability
        auto shadowBrush = renderer->CreateBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.6f * overlayAlpha));

        // Image counter "3 / 15"
        if (counterFormat_ && overlayTextBrush_) {
            std::wstring counter = std::to_wstring(currentIndex_ + 1) + L" / " + std::to_wstring(images_.size());
            D2D1_RECT_F counterRect = D2D1::RectF(
                viewWidth_ - 140.0f, 6.0f, viewWidth_ - 14.0f, 38.0f);

            // Shadow
            if (shadowBrush) {
                D2D1_RECT_F shadowRect = D2D1::RectF(
                    counterRect.left + 1.0f, counterRect.top + 1.0f,
                    counterRect.right + 1.0f, counterRect.bottom + 1.0f);
                ctx->DrawText(counter.c_str(), static_cast<UINT32>(counter.size()),
                              counterFormat_.Get(), shadowRect, shadowBrush.Get());
            }

            overlayTextBrush_->SetOpacity(overlayAlpha * 0.95f);
            ctx->DrawText(counter.c_str(), static_cast<UINT32>(counter.size()),
                          counterFormat_.Get(), counterRect, overlayTextBrush_.Get());
        }

        // Filename
        if (filenameFormat_ && overlayTextBrush_) {
            auto name = images_[currentIndex_].filename().wstring();
            D2D1_RECT_F nameRect = D2D1::RectF(14.0f, 6.0f, viewWidth_ - 150.0f, 38.0f);

            // Shadow
            if (shadowBrush) {
                D2D1_RECT_F shadowRect = D2D1::RectF(
                    nameRect.left + 1.0f, nameRect.top + 1.0f,
                    nameRect.right + 1.0f, nameRect.bottom + 1.0f);
                ctx->DrawText(name.c_str(), static_cast<UINT32>(name.size()),
                              filenameFormat_.Get(), shadowRect, shadowBrush.Get());
            }

            overlayTextBrush_->SetOpacity(overlayAlpha * 0.8f);
            ctx->DrawText(name.c_str(), static_cast<UINT32>(name.size()),
                          filenameFormat_.Get(), nameRect, overlayTextBrush_.Get());
            overlayTextBrush_->SetOpacity(1.0f);
        }
    }
}

void ImageViewer::Update(float deltaTime)
{
    pageOffsetX_.Update(deltaTime);
    zoomSpring_.Update(deltaTime);
    panXSpring_.Update(deltaTime);
    panYSpring_.Update(deltaTime);
    dismissSpring_.Update(deltaTime);

    zoom_ = zoomSpring_.GetValue();
    panX_ = panXSpring_.GetValue();
    panY_ = panYSpring_.GetValue();
    dismissOffsetY_ = dismissSpring_.GetValue();

    // Check if page navigation completed
    if (!isPaging_ && std::abs(pageOffsetX_.GetValue()) < 1.0f && pageOffsetX_.IsFinished()) {
        pageOffsetX_.SetValue(0.0f);
        pageOffsetX_.SetTarget(0.0f);
        pageOffsetX_.SnapToTarget();
    }
}

void ImageViewer::OnMouseDown(float x, float y)
{
    isMouseDown_ = true;
    mouseDownX_ = x;
    mouseDownY_ = y;
    lastMouseX_ = x;
    lastMouseY_ = y;
    mouseVelocityX_ = 0.0f;
    mouseVelocityY_ = 0.0f;
    hasDragged_ = false;
}

void ImageViewer::OnMiddleMouseDown(float x, float y)
{
    isMiddleDragging_ = true;
    lastMouseX_ = x;
    lastMouseY_ = y;
}

void ImageViewer::OnMiddleMouseUp(float x, float y)
{
    if (!isMiddleDragging_) return;
    isMiddleDragging_ = false;

    // Snap pan back to bounds
    if (zoom_ <= 1.01f) {
        panXSpring_.SetTarget(0.0f);
        panYSpring_.SetTarget(0.0f);
    } else {
        auto bounds = CalculatePanBounds();
        panXSpring_.SetTarget(std::max(bounds.left, std::min(panX_, bounds.right)));
        panYSpring_.SetTarget(std::max(bounds.top, std::min(panY_, bounds.bottom)));
    }
}

void ImageViewer::OnMouseMove(float x, float y)
{
    // Middle-button: pure pan, no dismiss/page logic
    if (isMiddleDragging_) {
        float dx = x - lastMouseX_;
        float dy = y - lastMouseY_;
        panX_ += dx;
        panY_ += dy;
        panXSpring_.SetValue(panX_);
        panXSpring_.SetTarget(panX_);
        panYSpring_.SetValue(panY_);
        panYSpring_.SetTarget(panY_);
        lastMouseX_ = x;
        lastMouseY_ = y;
        return;
    }

    if (!isMouseDown_) return;

    float dx = x - lastMouseX_;
    float dy = y - lastMouseY_;
    float totalDx = x - mouseDownX_;
    float totalDy = y - mouseDownY_;

    if (!hasDragged_ && (std::abs(totalDx) > kDragThreshold || std::abs(totalDy) > kDragThreshold)) {
        hasDragged_ = true;
        bool isVertical = std::abs(totalDy) > std::abs(totalDx);

        if (zoom_ <= 1.01f) {
            // Not zoomed: vertical = dismiss, horizontal = page
            if (isVertical) {
                isDismissing_ = true;
                dismissStartY_ = mouseDownY_;
            } else {
                isPaging_ = true;
            }
        } else {
            // Zoomed in: always start as pan, dismiss only triggers at pan boundary
        }
    }

    if (hasDragged_) {
        if (isDismissing_) {
            float newDismiss = y - dismissStartY_;
            dismissSpring_.SetValue(newDismiss);
            dismissSpring_.SetTarget(newDismiss);
        } else if (isPaging_) {
            float newOffset = x - mouseDownX_;
            pageOffsetX_.SetValue(newOffset);
            pageOffsetX_.SetTarget(newOffset);
        } else if (zoom_ > 1.01f) {
            // Zoomed-in panning with boundary-triggered dismiss
            float newPanX = panX_ + dx;
            float newPanY = panY_ + dy;

            auto bounds = CalculatePanBounds();

            // Clamp X to bounds
            newPanX = std::max(bounds.left, std::min(newPanX, bounds.right));

            // Check if pulling past vertical boundary → transition to dismiss
            if (newPanY > bounds.bottom + kDragThreshold) {
                isDismissing_ = true;
                dismissStartY_ = y;
                dismissSpring_.SetValue(0.0f);
                dismissSpring_.SetTarget(0.0f);
                panY_ = bounds.bottom;
                panYSpring_.SetValue(panY_);
                panYSpring_.SetTarget(panY_);
                panX_ = newPanX;
                panXSpring_.SetValue(panX_);
                panXSpring_.SetTarget(panX_);
            } else if (newPanY < bounds.top - kDragThreshold) {
                isDismissing_ = true;
                dismissStartY_ = y;
                dismissSpring_.SetValue(0.0f);
                dismissSpring_.SetTarget(0.0f);
                panY_ = bounds.top;
                panYSpring_.SetValue(panY_);
                panYSpring_.SetTarget(panY_);
                panX_ = newPanX;
                panXSpring_.SetValue(panX_);
                panXSpring_.SetTarget(panX_);
            } else {
                // Normal pan within bounds
                panX_ = newPanX;
                panY_ = newPanY;
                panXSpring_.SetValue(panX_);
                panXSpring_.SetTarget(panX_);
                panYSpring_.SetValue(panY_);
                panYSpring_.SetTarget(panY_);
            }
        }
    }

    mouseVelocityX_ = dx * 60.0f;
    mouseVelocityY_ = dy * 60.0f;
    lastMouseX_ = x;
    lastMouseY_ = y;
}

void ImageViewer::OnMouseUp(float x, float y)
{
    if (!isMouseDown_) return;
    isMouseDown_ = false;

    // Check for double-tap
    ULONGLONG now = GetTickCount64();
    if (!hasDragged_) {
        if (now - lastClickTime_ < kDoubleTapMs &&
            std::abs(x - lastClickX_) < 20.0f &&
            std::abs(y - lastClickY_) < 20.0f) {
            // Double-tap: toggle zoom
            if (isZoomedIn_) {
                zoomSpring_.SetTarget(1.0f);
                panXSpring_.SetTarget(0.0f);
                panYSpring_.SetTarget(0.0f);
                isZoomedIn_ = false;
            } else {
                zoomSpring_.SetTarget(2.5f);
                // Pan towards tap point
                float tapOffsetX = (x - viewWidth_ * 0.5f) * -1.5f;
                float tapOffsetY = (y - viewHeight_ * 0.5f) * -1.5f;
                panXSpring_.SetTarget(tapOffsetX);
                panYSpring_.SetTarget(tapOffsetY);
                isZoomedIn_ = true;
            }
            lastClickTime_ = 0;
            return;
        }
        lastClickTime_ = now;
        lastClickX_ = x;
        lastClickY_ = y;
    }

    if (isDismissing_) {
        isDismissing_ = false;
        if (std::abs(dismissSpring_.GetValue()) > kDismissThreshold) {
            // Reset zoom/pan but keep dismiss offset — ViewManager reads it for transition
            zoom_ = 1.0f;
            panX_ = 0.0f;
            panY_ = 0.0f;
            isZoomedIn_ = false;
            zoomSpring_.SetValue(1.0f);
            zoomSpring_.SetTarget(1.0f);
            zoomSpring_.SnapToTarget();
            panXSpring_.SetValue(0.0f);
            panXSpring_.SetTarget(0.0f);
            panXSpring_.SnapToTarget();
            panYSpring_.SetValue(0.0f);
            panYSpring_.SetTarget(0.0f);
            panYSpring_.SnapToTarget();
            // Call callback BEFORE resetting dismiss (ViewManager reads current screen rect)
            if (dismissCallback_) {
                dismissCallback_(currentIndex_);
            }
            // Now reset dismiss
            dismissSpring_.SetValue(0.0f);
            dismissSpring_.SetTarget(0.0f);
            dismissSpring_.SnapToTarget();
        } else {
            dismissSpring_.SetTarget(0.0f);
        }
    } else if (isPaging_) {
        isPaging_ = false;
        float offset = pageOffsetX_.GetValue();
        float velocity = mouseVelocityX_;

        if (offset > viewWidth_ * kPageThreshold || velocity > 500.0f) {
            // Go to previous
            if (currentIndex_ > 0) {
                pageOffsetX_.SetTarget(viewWidth_);
                pageOffsetX_.SetVelocity(velocity);
                // After animation finishes, change page
                NavigateToPage(-1);
            } else {
                pageOffsetX_.SetTarget(0.0f);
            }
        } else if (offset < -viewWidth_ * kPageThreshold || velocity < -500.0f) {
            // Go to next
            if (currentIndex_ + 1 < images_.size()) {
                pageOffsetX_.SetTarget(-viewWidth_);
                pageOffsetX_.SetVelocity(velocity);
                NavigateToPage(1);
            } else {
                pageOffsetX_.SetTarget(0.0f);
            }
        } else {
            // Snap back
            pageOffsetX_.SetTarget(0.0f);
        }
    } else if (hasDragged_ && zoom_ > 1.01f) {
        // Bounce pan back if out of bounds
        auto bounds = CalculatePanBounds();
        float targetPanX = std::max(bounds.left, std::min(panX_, bounds.right));
        float targetPanY = std::max(bounds.top, std::min(panY_, bounds.bottom));
        panXSpring_.SetTarget(targetPanX);
        panYSpring_.SetTarget(targetPanY);
    }
}

void ImageViewer::OnMouseWheel(float delta, float x, float y)
{
    // Zoom with mouse wheel
    float zoomDelta = delta > 0 ? 1.15f : 0.87f;
    float newZoom = zoom_ * zoomDelta;
    newZoom = std::max(0.5f, std::min(newZoom, 10.0f));

    if (newZoom < 1.0f) {
        newZoom = 1.0f;
        panXSpring_.SetTarget(0.0f);
        panYSpring_.SetTarget(0.0f);
        isZoomedIn_ = false;
    } else {
        isZoomedIn_ = (newZoom > 1.1f);
    }

    zoomSpring_.SetTarget(newZoom);
}

void ImageViewer::OnKeyDown(UINT key)
{
    switch (key) {
        case VK_LEFT:
            GoPrev();
            break;
        case VK_RIGHT:
            GoNext();
            break;
        case VK_ESCAPE:
            // Reset zoom/pan for clean hero transition from center
            zoom_ = 1.0f;
            panX_ = 0.0f;
            panY_ = 0.0f;
            isZoomedIn_ = false;
            zoomSpring_.SetValue(1.0f);
            zoomSpring_.SetTarget(1.0f);
            zoomSpring_.SnapToTarget();
            panXSpring_.SetValue(0.0f);
            panXSpring_.SetTarget(0.0f);
            panXSpring_.SnapToTarget();
            panYSpring_.SetValue(0.0f);
            panYSpring_.SetTarget(0.0f);
            panYSpring_.SnapToTarget();
            // dismissSpring_ is at 0 → GetCurrentScreenRect returns centered fit rect
            if (dismissCallback_) {
                dismissCallback_(currentIndex_);
            }
            break;
    }
}

D2D1_RECT_F ImageViewer::GetCurrentImageRect() const
{
    if (!currentBitmap_) {
        return D2D1::RectF(0, 0, viewWidth_, viewHeight_);
    }
    auto size = currentBitmap_->GetSize();
    return CalculateFitRect(size.width, size.height);
}

D2D1_RECT_F ImageViewer::GetCurrentScreenRect() const
{
    if (!currentBitmap_) {
        return D2D1::RectF(0, 0, viewWidth_, viewHeight_);
    }
    auto size = currentBitmap_->GetSize();
    D2D1_RECT_F fitRect = CalculateFitRect(size.width, size.height);

    float currentZoom = zoomSpring_.GetValue();
    float currentPanX = panXSpring_.GetValue();
    float currentPanY = panYSpring_.GetValue();
    float dismissY = dismissSpring_.GetValue();

    float centerX = viewWidth_ * 0.5f;
    float centerY = viewHeight_ * 0.5f;
    float w = (fitRect.right - fitRect.left) * currentZoom;
    float h = (fitRect.bottom - fitRect.top) * currentZoom;

    D2D1_RECT_F rect = D2D1::RectF(
        centerX - w * 0.5f + currentPanX,
        centerY - h * 0.5f + currentPanY + dismissY,
        centerX + w * 0.5f + currentPanX,
        centerY + h * 0.5f + currentPanY + dismissY
    );

    // Apply dismiss scale (same formula as Render)
    float dismissScale = 1.0f - std::abs(dismissY) / (viewHeight_ * 2.0f);
    dismissScale = std::max(0.5f, dismissScale);
    if (dismissScale < 1.0f) {
        float dw = (rect.right - rect.left) * (1.0f - dismissScale) * 0.5f;
        float dh = (rect.bottom - rect.top) * (1.0f - dismissScale) * 0.5f;
        rect.left += dw;
        rect.top += dh;
        rect.right -= dw;
        rect.bottom -= dh;
    }

    return rect;
}

float ImageViewer::GetCurrentBgAlpha() const
{
    float dismissY = dismissSpring_.GetValue();
    float bgAlpha = 1.0f - std::abs(dismissY) / (viewHeight_ * 0.5f);
    return std::max(0.0f, std::min(1.0f, bgAlpha));
}

void ImageViewer::SetViewSize(float width, float height)
{
    viewWidth_ = width;
    viewHeight_ = height;
}

void ImageViewer::GoToIndex(size_t index)
{
    if (index >= images_.size()) return;
    currentIndex_ = index;
    zoom_ = 1.0f;
    panX_ = 0.0f;
    panY_ = 0.0f;
    isZoomedIn_ = false;
    zoomSpring_.SetValue(1.0f);
    zoomSpring_.SetTarget(1.0f);
    zoomSpring_.SnapToTarget();
    panXSpring_.SetValue(0.0f);
    panXSpring_.SetTarget(0.0f);
    panXSpring_.SnapToTarget();
    panYSpring_.SetValue(0.0f);
    panYSpring_.SetTarget(0.0f);
    panYSpring_.SnapToTarget();
    pageOffsetX_.SetValue(0.0f);
    pageOffsetX_.SetTarget(0.0f);
    pageOffsetX_.SnapToTarget();
    LoadCurrentPage();
}

void ImageViewer::GoNext()
{
    if (currentIndex_ + 1 < images_.size()) {
        NavigateToPage(1);
    }
}

void ImageViewer::GoPrev()
{
    if (currentIndex_ > 0) {
        NavigateToPage(-1);
    }
}

void ImageViewer::NavigateToPage(int direction)
{
    size_t newIndex = currentIndex_;
    if (direction > 0 && currentIndex_ + 1 < images_.size()) {
        newIndex = currentIndex_ + 1;
    } else if (direction < 0 && currentIndex_ > 0) {
        newIndex = currentIndex_ - 1;
    } else {
        return;
    }

    currentIndex_ = newIndex;
    zoom_ = 1.0f;
    panX_ = 0.0f;
    panY_ = 0.0f;
    isZoomedIn_ = false;

    zoomSpring_.SetValue(1.0f);
    zoomSpring_.SetTarget(1.0f);
    zoomSpring_.SnapToTarget();
    panXSpring_.SetValue(0.0f);
    panXSpring_.SetTarget(0.0f);
    panXSpring_.SnapToTarget();
    panYSpring_.SetValue(0.0f);
    panYSpring_.SetTarget(0.0f);
    panYSpring_.SnapToTarget();
    pageOffsetX_.SetValue(0.0f);
    pageOffsetX_.SetTarget(0.0f);
    pageOffsetX_.SnapToTarget();

    LoadCurrentPage();
}

} // namespace UI
} // namespace UltraImageViewer
