#include "ui/GalleryView.hpp"
#include "ui/Theme.hpp"
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace UltraImageViewer {
namespace UI {

GalleryView::GalleryView()
    : scrollY_(Animation::SpringConfig{Theme::ScrollStiffness, Theme::ScrollDamping, 1.0f, 0.5f})
{
    scrollY_.SetValue(0.0f);
    scrollY_.SetTarget(0.0f);
    scrollY_.SnapToTarget();
}

GalleryView::~GalleryView() = default;

void GalleryView::Initialize(Rendering::Direct2DRenderer* renderer,
                              Core::ImagePipeline* pipeline,
                              Animation::AnimationEngine* engine)
{
    pipeline_ = pipeline;
    engine_ = engine;
    EnsureResources(renderer);
}

void GalleryView::SetImagesGrouped(const std::vector<Core::ScannedImage>& scannedImages)
{
    images_.clear();
    sections_.clear();

    if (scannedImages.empty()) {
        cachedLayoutWidth_ = 0.0f;
        return;
    }

    // Images should already be sorted by date descending
    int currentYear = -1;
    int currentMonth = -1;

    static const wchar_t* monthNames[] = {
        L"", L"1\u6708", L"2\u6708", L"3\u6708", L"4\u6708", L"5\u6708", L"6\u6708",
        L"7\u6708", L"8\u6708", L"9\u6708", L"10\u6708", L"11\u6708", L"12\u6708"
    };

    for (const auto& img : scannedImages) {
        if (img.year != currentYear || img.month != currentMonth) {
            currentYear = img.year;
            currentMonth = img.month;

            Section section;
            if (currentMonth >= 1 && currentMonth <= 12) {
                section.title = std::to_wstring(currentYear) + L"\u5E74" + monthNames[currentMonth];
            } else {
                section.title = std::to_wstring(currentYear) + L"\u5E74";
            }
            section.startIndex = images_.size();
            section.count = 0;
            sections_.push_back(std::move(section));
        }

        images_.push_back(img.path);
        sections_.back().count++;
    }

    scrollY_.SetValue(0.0f);
    scrollY_.SetTarget(0.0f);
    scrollY_.SnapToTarget();
    cachedLayoutWidth_ = 0.0f;
}

void GalleryView::SetImages(const std::vector<std::filesystem::path>& paths)
{
    images_ = paths;
    sections_.clear();

    if (!paths.empty()) {
        Section section;
        section.title = paths[0].parent_path().filename().wstring();
        section.startIndex = 0;
        section.count = paths.size();
        sections_.push_back(std::move(section));
    }

    scrollY_.SetValue(0.0f);
    scrollY_.SetTarget(0.0f);
    scrollY_.SnapToTarget();
    cachedLayoutWidth_ = 0.0f;
}

void GalleryView::SetScanningState(bool scanning, size_t count)
{
    isScanning_ = scanning;
    scanCount_ = count;
}

void GalleryView::EnsureResources(Rendering::Direct2DRenderer* renderer)
{
    if (resourcesCreated_ || !renderer) return;

    bgBrush_ = renderer->CreateBrush(Theme::Background);
    cellBrush_ = renderer->CreateBrush(Theme::Surface);
    textBrush_ = renderer->CreateBrush(Theme::TextPrimary);
    secondaryBrush_ = renderer->CreateBrush(Theme::TextSecondary);
    accentBrush_ = renderer->CreateBrush(Theme::Accent);

    titleFormat_ = renderer->CreateTextFormat(L"Segoe UI", 28.0f, DWRITE_FONT_WEIGHT_BOLD);
    sectionFormat_ = renderer->CreateTextFormat(L"Segoe UI", 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    countFormat_ = renderer->CreateTextFormat(L"Segoe UI", 14.0f);

    if (titleFormat_) {
        titleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        titleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (sectionFormat_) {
        sectionFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        sectionFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    }
    if (countFormat_) {
        countFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        countFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    countRightFormat_ = renderer->CreateTextFormat(L"Segoe UI", 13.0f);
    if (countRightFormat_) {
        countRightFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        countRightFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    }

    hoverBrush_ = renderer->CreateBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f));
    scrollIndicatorBrush_ = renderer->CreateBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.2f));

    resourcesCreated_ = true;
}

GalleryView::GridLayout GalleryView::CalculateGridLayout(float viewWidth) const
{
    GridLayout grid = {};
    grid.gap = Theme::ThumbnailGap;
    grid.paddingX = Theme::GalleryPadding;

    float availableWidth = viewWidth - grid.paddingX * 2.0f;
    grid.columns = std::max(1, static_cast<int>(availableWidth / (Theme::MinCellSize + grid.gap)));

    grid.cellSize = (availableWidth - grid.gap * (grid.columns - 1)) / grid.columns;
    grid.cellSize = std::min(grid.cellSize, Theme::MaxCellSize);

    // Recalculate columns with clamped cell size
    grid.columns = std::max(1, static_cast<int>((availableWidth + grid.gap) / (grid.cellSize + grid.gap)));
    grid.cellSize = (availableWidth - grid.gap * (grid.columns - 1)) / grid.columns;

    return grid;
}

void GalleryView::ComputeSectionLayouts(const GridLayout& grid) const
{
    sectionLayouts_.clear();
    sectionLayouts_.resize(sections_.size());

    float y = Theme::GalleryHeaderHeight + Theme::GalleryPadding;

    for (size_t i = 0; i < sections_.size(); ++i) {
        sectionLayouts_[i].headerY = y;
        sectionLayouts_[i].contentY = y + Theme::SectionHeaderHeight;
        sectionLayouts_[i].rows = static_cast<int>(
            (sections_[i].count + grid.columns - 1) / grid.columns);

        y = sectionLayouts_[i].contentY +
            sectionLayouts_[i].rows * (grid.cellSize + grid.gap);

        if (i + 1 < sections_.size()) {
            y += Theme::SectionGap;
        }
    }

    cachedTotalHeight_ = y + Theme::GalleryPadding;
}

std::wstring GalleryView::FormatNumber(size_t n)
{
    std::wstring s = std::to_wstring(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
        s.insert(i, L",");
    }
    return s;
}

void GalleryView::Render(Rendering::Direct2DRenderer* renderer)
{
    if (!renderer) return;
    EnsureResources(renderer);

    auto* ctx = renderer->GetContext();
    if (!ctx) return;

    auto grid = CalculateGridLayout(viewWidth_);
    cachedGrid_ = grid;
    cachedLayoutWidth_ = viewWidth_;

    ComputeSectionLayouts(grid);
    maxScroll_ = std::max(0.0f, cachedTotalHeight_ - viewHeight_);

    // Clear background
    ctx->Clear(Theme::Background);

    float scroll = scrollY_.GetValue();

    // === Page header ===
    if (textBrush_ && titleFormat_) {
        D2D1_RECT_F titleRect = D2D1::RectF(
            Theme::GalleryPadding, 8.0f,
            viewWidth_ - Theme::GalleryPadding, 50.0f);
        ctx->DrawText(L"Photos", 6, titleFormat_.Get(), titleRect, textBrush_.Get());
    }

    // Subtitle: scanning state, count, or empty
    if (countFormat_) {
        D2D1_RECT_F subtitleRect = D2D1::RectF(
            Theme::GalleryPadding, 48.0f,
            viewWidth_ - Theme::GalleryPadding, 70.0f);

        if (isScanning_) {
            std::wstring sub = L"Scanning... " + FormatNumber(scanCount_) + L" photos found";
            if (accentBrush_) {
                ctx->DrawText(sub.c_str(), static_cast<UINT32>(sub.size()),
                              countFormat_.Get(), subtitleRect, accentBrush_.Get());
            }
        } else if (images_.empty()) {
            if (secondaryBrush_) {
                std::wstring sub = L"No photos found  \u00B7  Ctrl+O browse files  \u00B7  Ctrl+D add album folder";
                ctx->DrawText(sub.c_str(), static_cast<UINT32>(sub.size()),
                              countFormat_.Get(), subtitleRect, secondaryBrush_.Get());
            }
        } else {
            std::wstring sub = FormatNumber(images_.size()) + L" photos";
            if (secondaryBrush_) {
                ctx->DrawText(sub.c_str(), static_cast<UINT32>(sub.size()),
                              countFormat_.Get(), subtitleRect, secondaryBrush_.Get());
            }
        }
    }

    // === Scanning animation indicator ===
    if (isScanning_ && accentBrush_) {
        // Small progress bar at bottom of header
        float barY = Theme::GalleryHeaderHeight - 2.0f;
        float progress = std::fmod(static_cast<float>(scanCount_) * 0.01f, 1.0f);
        float barWidth = viewWidth_ * 0.3f;
        float barX = progress * (viewWidth_ - barWidth);
        ctx->FillRectangle(
            D2D1::RectF(barX, barY, barX + barWidth, barY + 2.0f),
            accentBrush_.Get());
    }

    // === Section rendering ===
    for (size_t s = 0; s < sections_.size(); ++s) {
        const auto& section = sections_[s];
        const auto& sl = sectionLayouts_[s];

        float sectionEndY = sl.contentY + sl.rows * (grid.cellSize + grid.gap);

        // Skip if entirely above viewport
        if (sectionEndY - scroll < 0.0f) continue;
        // Stop if entirely below viewport
        if (sl.headerY - scroll > viewHeight_) break;

        // Draw section header
        float headerScreenY = sl.headerY - scroll;
        if (headerScreenY + Theme::SectionHeaderHeight > 0 && headerScreenY < viewHeight_) {
            if (sectionFormat_ && textBrush_) {
                D2D1_RECT_F headerRect = D2D1::RectF(
                    grid.paddingX, headerScreenY,
                    viewWidth_ * 0.6f, headerScreenY + Theme::SectionHeaderHeight);
                ctx->DrawText(section.title.c_str(),
                              static_cast<UINT32>(section.title.size()),
                              sectionFormat_.Get(), headerRect, textBrush_.Get());
            }
            // Section photo count (right-aligned)
            if (countRightFormat_ && secondaryBrush_) {
                std::wstring countStr = FormatNumber(section.count) + L" photos";
                D2D1_RECT_F countRect = D2D1::RectF(
                    viewWidth_ * 0.5f, headerScreenY,
                    viewWidth_ - grid.paddingX, headerScreenY + Theme::SectionHeaderHeight);
                ctx->DrawText(countStr.c_str(), static_cast<UINT32>(countStr.size()),
                              countRightFormat_.Get(), countRect, secondaryBrush_.Get());
            }
        }

        // Draw cells for this section
        for (size_t i = 0; i < section.count; ++i) {
            int localRow = static_cast<int>(i) / grid.columns;
            int localCol = static_cast<int>(i) % grid.columns;

            float cellX = grid.paddingX + localCol * (grid.cellSize + grid.gap);
            float cellY = sl.contentY + localRow * (grid.cellSize + grid.gap) - scroll;

            // Skip off-screen cells
            if (cellY + grid.cellSize < 0.0f) continue;
            if (cellY > viewHeight_) break;

            size_t globalIndex = section.startIndex + i;
            D2D1_RECT_F cellRect = D2D1::RectF(
                cellX, cellY, cellX + grid.cellSize, cellY + grid.cellSize);

            // Cell background placeholder
            if (cellBrush_) {
                ctx->FillRectangle(cellRect, cellBrush_.Get());
            }

            // Skip the cell that's currently "lifted" into the viewer
            if (skipIndex_.has_value() && globalIndex == skipIndex_.value()) continue;

            // Thumbnail
            auto thumbnail = pipeline_ ? pipeline_->GetThumbnail(images_[globalIndex]) : nullptr;
            if (thumbnail) {
                // Use axis-aligned clip (fast, no geometry allocation)
                ctx->PushAxisAlignedClip(cellRect, D2D1_ANTIALIAS_MODE_ALIASED);

                // Cover-mode crop
                auto imgSize = thumbnail->GetSize();
                float imgAspect = imgSize.width / imgSize.height;
                float cellW = cellRect.right - cellRect.left;
                float cellH = cellRect.bottom - cellRect.top;
                float cellAspect = cellW / cellH;

                D2D1_RECT_F srcRect;
                if (imgAspect > cellAspect) {
                    float cropWidth = imgSize.height * cellAspect;
                    float offset = (imgSize.width - cropWidth) * 0.5f;
                    srcRect = D2D1::RectF(offset, 0, offset + cropWidth, imgSize.height);
                } else {
                    float cropHeight = imgSize.width / cellAspect;
                    float offset = (imgSize.height - cropHeight) * 0.5f;
                    srcRect = D2D1::RectF(0, offset, imgSize.width, offset + cropHeight);
                }

                ctx->DrawBitmap(thumbnail.Get(), cellRect, 1.0f,
                                D2D1_INTERPOLATION_MODE_LINEAR, &srcRect);
                ctx->PopAxisAlignedClip();
            }

            // Hover highlight
            if (hoverBrush_ &&
                hoverX_ >= cellRect.left && hoverX_ <= cellRect.right &&
                hoverY_ >= cellRect.top && hoverY_ <= cellRect.bottom) {
                ctx->FillRectangle(cellRect, hoverBrush_.Get());
            }
        }
    }

    // Prefetch thumbnails for visible area
    if (pipeline_ && !images_.empty()) {
        size_t centerEstimate = images_.size() / 2;
        // Rough estimate of visible center index
        float centerWorldY = scroll + viewHeight_ * 0.5f;
        for (size_t s = 0; s < sections_.size(); ++s) {
            if (s < sectionLayouts_.size()) {
                float secEnd = sectionLayouts_[s].contentY +
                    sectionLayouts_[s].rows * (grid.cellSize + grid.gap);
                if (centerWorldY < secEnd) {
                    float localY = centerWorldY - sectionLayouts_[s].contentY;
                    int row = std::max(0, static_cast<int>(localY / (grid.cellSize + grid.gap)));
                    centerEstimate = sections_[s].startIndex + row * grid.columns;
                    break;
                }
            }
        }
        centerEstimate = std::min(centerEstimate, images_.size() - 1);
        pipeline_->PrefetchAround(images_, centerEstimate, 8);
    }

    // Scroll indicator
    if (maxScroll_ > 0.0f && !images_.empty()) {
        float scrollRatio = std::max(0.0f, std::min(1.0f, scroll / maxScroll_));
        float indicatorHeight = std::max(30.0f, viewHeight_ * (viewHeight_ / cachedTotalHeight_));
        float indicatorTop = scrollRatio * (viewHeight_ - indicatorHeight);
        D2D1_ROUNDED_RECT indicatorRect = {
            D2D1::RectF(viewWidth_ - 6.0f, indicatorTop + 4.0f,
                         viewWidth_ - 2.0f, indicatorTop + indicatorHeight - 4.0f),
            2.0f, 2.0f
        };
        if (scrollIndicatorBrush_) {
            ctx->FillRoundedRectangle(indicatorRect, scrollIndicatorBrush_.Get());
        }
    }

    // Empty state (only shown when not scanning and no images)
    if (images_.empty() && !isScanning_) {
        float cx = viewWidth_ * 0.5f;
        float cy = viewHeight_ * 0.50f;

        // Icon + hint using existing brushes (subtle appearance)
        if (secondaryBrush_) {
            float sz = 40.0f;
            ctx->DrawRectangle(D2D1::RectF(cx - sz, cy - sz, cx + sz, cy + sz),
                               secondaryBrush_.Get(), 2.0f);
            D2D1_ELLIPSE sun = {D2D1::Point2F(cx + sz * 0.3f, cy - sz * 0.3f), 8.0f, 8.0f};
            ctx->FillEllipse(sun, secondaryBrush_.Get());
        }

        if (countFormat_ && secondaryBrush_) {
            D2D1_RECT_F hintRect = D2D1::RectF(0, cy + 60.0f, viewWidth_, cy + 90.0f);
            auto hintFormat = renderer->CreateTextFormat(L"Segoe UI", 15.0f);
            if (hintFormat) {
                hintFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                hintFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                const wchar_t* hint = L"Drag images here \u00B7 Ctrl+O browse files \u00B7 Ctrl+D add album folder";
                ctx->DrawText(hint, static_cast<UINT32>(wcslen(hint)),
                              hintFormat.Get(), hintRect, secondaryBrush_.Get());
            }
        }
    }
}

void GalleryView::Update(float deltaTime)
{
    scrollY_.Update(deltaTime);

    // Rubber band: only snap back when the spring is settling (velocity is low)
    // This allows the scroll animation to overshoot naturally before bouncing back
    if (!isDragging_) {
        float currentValue = scrollY_.GetValue();
        float velocity = std::abs(scrollY_.GetVelocity());

        // Only rubber-band once the spring is slowing down
        if (velocity < 500.0f) {
            if (currentValue < 0.0f) {
                scrollY_.SetTarget(0.0f);
                if (velocity < 100.0f) {
                    scrollY_.SetConfig({Theme::RubberBandStiffness, Theme::RubberBandDamping, 1.0f, 0.5f});
                }
            } else if (currentValue > maxScroll_ && maxScroll_ > 0.0f) {
                scrollY_.SetTarget(maxScroll_);
                if (velocity < 100.0f) {
                    scrollY_.SetConfig({Theme::RubberBandStiffness, Theme::RubberBandDamping, 1.0f, 0.5f});
                }
            }
        }
    }
}

void GalleryView::OnMouseWheel(float delta)
{
    // delta is typically ±120 per notch; scale to ~300px per notch for fast gallery scrolling
    float scrollAmount = -delta * 2.5f;
    float newTarget = scrollY_.GetTarget() + scrollAmount;
    newTarget = std::max(-80.0f, std::min(newTarget, maxScroll_ + 80.0f));
    scrollY_.SetTarget(newTarget);
    scrollY_.SetConfig({Theme::ScrollStiffness, Theme::ScrollDamping, 1.0f, 0.5f});
}

void GalleryView::OnMouseDown(float x, float y)
{
    isDragging_ = true;
    hasDragged_ = false;
    dragStartX_ = x;
    dragStartY_ = y;
    dragStartScroll_ = scrollY_.GetValue();
    lastDragY_ = y;
    scrollVelocity_ = 0.0f;
}

void GalleryView::OnMouseMove(float x, float y)
{
    hoverX_ = x;
    hoverY_ = y;

    if (isDragging_) {
        if (!hasDragged_) {
            float totalDx = std::abs(x - dragStartX_);
            float totalDy = std::abs(y - dragStartY_);
            if (totalDx > 5.0f || totalDy > 5.0f) {
                hasDragged_ = true;
            }
        }
        float dy = dragStartY_ - y;
        float newScroll = dragStartScroll_ + dy;

        // Rubber band resistance at edges
        if (newScroll < 0.0f) {
            newScroll *= 0.3f;
        } else if (newScroll > maxScroll_) {
            float excess = newScroll - maxScroll_;
            newScroll = maxScroll_ + excess * 0.3f;
        }

        scrollY_.SetValue(newScroll);
        scrollY_.SetTarget(newScroll);

        scrollVelocity_ = (lastDragY_ - y) * 60.0f;
        lastDragY_ = y;
    }
}

void GalleryView::OnMouseUp(float x, float y)
{
    if (isDragging_) {
        isDragging_ = false;

        // Strong inertia: a fast flick can scroll the entire gallery
        float inertiaTarget = scrollY_.GetValue() + scrollVelocity_ * 0.6f;
        inertiaTarget = std::max(-80.0f, std::min(inertiaTarget, maxScroll_ + 80.0f));
        scrollY_.SetTarget(inertiaTarget);
        scrollY_.SetConfig({Theme::ScrollStiffness, Theme::ScrollDamping, 1.0f, 0.5f});
    }
}

std::optional<GalleryView::HitResult> GalleryView::HitTest(float x, float y) const
{
    auto grid = CalculateGridLayout(viewWidth_);
    ComputeSectionLayouts(grid);

    float scroll = scrollY_.GetValue();
    float worldY = y + scroll;

    for (size_t s = 0; s < sections_.size(); ++s) {
        const auto& section = sections_[s];
        if (s >= sectionLayouts_.size()) break;
        const auto& sl = sectionLayouts_[s];

        float contentEnd = sl.contentY + sl.rows * (grid.cellSize + grid.gap);

        // Check if point is in this section's content area
        if (worldY < sl.contentY || worldY >= contentEnd) continue;

        float localY = worldY - sl.contentY;
        int row = static_cast<int>(localY / (grid.cellSize + grid.gap));

        // Check we're in a cell, not in the gap
        float cellYOffset = localY - row * (grid.cellSize + grid.gap);
        if (cellYOffset > grid.cellSize) continue;

        for (int col = 0; col < grid.columns; ++col) {
            float cellX = grid.paddingX + col * (grid.cellSize + grid.gap);

            if (x >= cellX && x <= cellX + grid.cellSize) {
                size_t localIndex = static_cast<size_t>(row) * grid.columns + col;
                if (localIndex >= section.count) continue;

                size_t globalIndex = section.startIndex + localIndex;
                if (globalIndex >= images_.size()) continue;

                float screenCellY = sl.contentY + row * (grid.cellSize + grid.gap) - scroll;
                D2D1_RECT_F rect = D2D1::RectF(
                    cellX, screenCellY,
                    cellX + grid.cellSize, screenCellY + grid.cellSize);
                return HitResult{globalIndex, rect};
            }
        }
    }
    return std::nullopt;
}

void GalleryView::SetViewSize(float width, float height)
{
    viewWidth_ = width;
    viewHeight_ = height;
}

std::optional<D2D1_RECT_F> GalleryView::GetCellScreenRect(size_t index) const
{
    if (index >= images_.size()) return std::nullopt;

    auto grid = CalculateGridLayout(viewWidth_);
    ComputeSectionLayouts(grid);

    for (size_t s = 0; s < sections_.size(); ++s) {
        if (index >= sections_[s].startIndex &&
            index < sections_[s].startIndex + sections_[s].count) {

            size_t localIndex = index - sections_[s].startIndex;
            int row = static_cast<int>(localIndex) / grid.columns;
            int col = static_cast<int>(localIndex) % grid.columns;

            float cellX = grid.paddingX + col * (grid.cellSize + grid.gap);
            float cellY = sectionLayouts_[s].contentY +
                row * (grid.cellSize + grid.gap) - scrollY_.GetValue();

            return D2D1::RectF(cellX, cellY, cellX + grid.cellSize, cellY + grid.cellSize);
        }
    }
    return std::nullopt;
}

} // namespace UI
} // namespace UltraImageViewer
