#include "ui/GalleryView.hpp"
#include "ui/Theme.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <d2d1_1.h>
#include <dwrite.h>

using Microsoft::WRL::ComPtr;

namespace UltraImageViewer {
namespace UI {

// Helper: draw a bitmap clipped to a rounded rectangle
static void DrawBitmapRounded(ID2D1DeviceContext* ctx, ID2D1Factory* factory,
                               ID2D1Bitmap* bitmap, const D2D1_RECT_F& destRect,
                               float radius, const D2D1_RECT_F* srcRect = nullptr)
{
    if (!bitmap || !ctx || !factory) return;

    D2D1_ROUNDED_RECT rr = {destRect, radius, radius};
    ComPtr<ID2D1RoundedRectangleGeometry> geo;
    factory->CreateRoundedRectangleGeometry(rr, &geo);
    if (!geo) return;

    D2D1_LAYER_PARAMETERS layerParams = D2D1::LayerParameters(destRect, geo.Get());
    ctx->PushLayer(layerParams, nullptr);

    if (srcRect) {
        ctx->DrawBitmap(bitmap, destRect, 1.0f,
                        D2D1_INTERPOLATION_MODE_LINEAR, srcRect);
    } else {
        ctx->DrawBitmap(bitmap, destRect, 1.0f,
                        D2D1_INTERPOLATION_MODE_LINEAR);
    }

    ctx->PopLayer();
}

// Helper: compute center-crop source rect
static D2D1_RECT_F ComputeCropRect(ID2D1Bitmap* bitmap, float destW, float destH)
{
    auto imgSize = bitmap->GetSize();
    float imgAspect = imgSize.width / imgSize.height;
    float destAspect = destW / destH;

    if (imgAspect > destAspect) {
        float cropWidth = imgSize.height * destAspect;
        float offset = (imgSize.width - cropWidth) * 0.5f;
        return D2D1::RectF(offset, 0, offset + cropWidth, imgSize.height);
    } else {
        float cropHeight = imgSize.width / destAspect;
        float offset = (imgSize.height - cropHeight) * 0.5f;
        return D2D1::RectF(0, offset, imgSize.width, offset + cropHeight);
    }
}

GalleryView::GalleryView()
    : scrollY_(Animation::SpringConfig{Theme::ScrollStiffness, Theme::ScrollDamping, 1.0f, 0.5f})
    , albumsScrollY_(Animation::SpringConfig{Theme::ScrollStiffness, Theme::ScrollDamping, 1.0f, 0.5f})
    , folderDetailScrollY_(Animation::SpringConfig{Theme::ScrollStiffness, Theme::ScrollDamping, 1.0f, 0.5f})
    , folderSlide_(Animation::SpringConfig{Theme::NavigationStiffness, Theme::NavigationDamping, 1.0f, 0.005f})
    , tabSlide_(Animation::SpringConfig{Theme::NavigationStiffness, Theme::NavigationDamping, 1.0f, 0.005f})
{
    scrollY_.SetValue(0.0f);
    scrollY_.SetTarget(0.0f);
    scrollY_.SnapToTarget();

    albumsScrollY_.SetValue(0.0f);
    albumsScrollY_.SetTarget(0.0f);
    albumsScrollY_.SnapToTarget();

    folderDetailScrollY_.SetValue(0.0f);
    folderDetailScrollY_.SetTarget(0.0f);
    folderDetailScrollY_.SnapToTarget();

    folderSlide_.SetValue(0.0f);
    folderSlide_.SetTarget(0.0f);
    folderSlide_.SnapToTarget();

    tabSlide_.SetValue(0.0f);
    tabSlide_.SetTarget(0.0f);
    tabSlide_.SnapToTarget();
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
    bool wasEmpty = images_.empty();

    images_.clear();
    sections_.clear();

    if (scannedImages.empty()) {
        cachedLayoutWidth_ = 0.0f;
        allScannedImages_.clear();
        folderAlbums_.clear();
        return;
    }

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

    if (wasEmpty) {
        scrollY_.SetValue(0.0f);
        scrollY_.SetTarget(0.0f);
        scrollY_.SnapToTarget();
    }
    cachedLayoutWidth_ = 0.0f;

    allScannedImages_ = scannedImages;
    BuildFolderAlbums(scannedImages);
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

    allScannedImages_.clear();
    folderAlbums_.clear();
}

const std::vector<std::filesystem::path>& GalleryView::GetActiveImages() const
{
    if (inFolderDetail_) return folderDetailImages_;
    return images_;
}

void GalleryView::SetScanningState(bool scanning, size_t count)
{
    isScanning_ = scanning;
    scanCount_ = count;
}

void GalleryView::BuildFolderAlbums(const std::vector<Core::ScannedImage>& scannedImages)
{
    std::map<std::wstring, FolderAlbum> albumMap;
    for (const auto& img : scannedImages) {
        auto parentDir = img.path.parent_path();
        std::wstring key = parentDir.wstring();
        auto& album = albumMap[key];
        if (album.imageCount == 0) {
            album.folderPath = parentDir;
            album.displayName = parentDir.filename().wstring();
            album.coverImage = img.path;
        }
        album.imageCount++;
    }

    folderAlbums_.clear();
    for (auto& [_, album] : albumMap) {
        folderAlbums_.push_back(std::move(album));
    }

    std::sort(folderAlbums_.begin(), folderAlbums_.end(),
        [](const FolderAlbum& a, const FolderAlbum& b) {
            return a.imageCount > b.imageCount;
        });
}

void GalleryView::EnterFolderDetail(size_t albumIndex)
{
    if (albumIndex >= folderAlbums_.size()) return;

    inFolderDetail_ = true;
    openFolderIndex_ = albumIndex;
    const auto& album = folderAlbums_[albumIndex];

    folderDetailImages_.clear();
    folderDetailSections_.clear();

    int currentYear = -1;
    int currentMonth = -1;

    static const wchar_t* monthNames[] = {
        L"", L"1\u6708", L"2\u6708", L"3\u6708", L"4\u6708", L"5\u6708", L"6\u6708",
        L"7\u6708", L"8\u6708", L"9\u6708", L"10\u6708", L"11\u6708", L"12\u6708"
    };

    for (const auto& img : allScannedImages_) {
        if (img.path.parent_path() != album.folderPath) continue;

        if (img.year != currentYear || img.month != currentMonth) {
            currentYear = img.year;
            currentMonth = img.month;

            Section section;
            if (currentMonth >= 1 && currentMonth <= 12) {
                section.title = std::to_wstring(currentYear) + L"\u5E74" + monthNames[currentMonth];
            } else {
                section.title = std::to_wstring(currentYear) + L"\u5E74";
            }
            section.startIndex = folderDetailImages_.size();
            section.count = 0;
            folderDetailSections_.push_back(std::move(section));
        }

        folderDetailImages_.push_back(img.path);
        folderDetailSections_.back().count++;
    }

    folderDetailScrollY_.SetValue(0.0f);
    folderDetailScrollY_.SetTarget(0.0f);
    folderDetailScrollY_.SnapToTarget();
    folderDetailMaxScroll_ = 0.0f;

    // Start navigation push animation
    folderSlide_.SetValue(0.0f);
    folderSlide_.SetTarget(1.0f);
    folderTransitionActive_ = true;
    folderTransitionForward_ = true;
}

void GalleryView::ExitFolderDetail()
{
    // Start navigation pop animation — data cleared on completion
    folderSlide_.SetTarget(0.0f);
    folderTransitionActive_ = true;
    folderTransitionForward_ = false;
}

void GalleryView::EnsureResources(Rendering::Direct2DRenderer* renderer)
{
    if (resourcesCreated_ || !renderer) return;

    bgBrush_ = renderer->CreateBrush(Theme::Background);
    cellBrush_ = renderer->CreateBrush(Theme::Surface);
    textBrush_ = renderer->CreateBrush(Theme::TextPrimary);
    secondaryBrush_ = renderer->CreateBrush(Theme::TextSecondary);
    accentBrush_ = renderer->CreateBrush(Theme::Accent);

    titleFormat_ = renderer->CreateTextFormat(L"Segoe UI Variable Display", 32.0f, DWRITE_FONT_WEIGHT_BOLD);
    if (!titleFormat_)
        titleFormat_ = renderer->CreateTextFormat(L"Segoe UI", 32.0f, DWRITE_FONT_WEIGHT_BOLD);
    sectionFormat_ = renderer->CreateTextFormat(L"Segoe UI", 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    countFormat_ = renderer->CreateTextFormat(L"Segoe UI", 13.0f);

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

    hoverBrush_ = renderer->CreateBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f));
    scrollIndicatorBrush_ = renderer->CreateBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f));

    // Tab bar
    tabBarBrush_ = renderer->CreateBrush(Theme::TabBarBg);
    tabFormat_ = renderer->CreateTextFormat(L"Segoe UI", 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (tabFormat_) {
        tabFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        tabFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // Album card text
    albumTitleFormat_ = renderer->CreateTextFormat(L"Segoe UI", 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (albumTitleFormat_) {
        albumTitleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        albumTitleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    albumCountFormat_ = renderer->CreateTextFormat(L"Segoe UI", 12.0f);
    if (albumCountFormat_) {
        albumCountFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        albumCountFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    backButtonFormat_ = renderer->CreateTextFormat(L"Segoe UI", 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (backButtonFormat_) {
        backButtonFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        backButtonFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

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

    grid.columns = std::max(1, static_cast<int>((availableWidth + grid.gap) / (grid.cellSize + grid.gap)));
    grid.cellSize = (availableWidth - grid.gap * (grid.columns - 1)) / grid.columns;

    return grid;
}

GalleryView::AlbumGridLayout GalleryView::CalculateAlbumGridLayout(float viewWidth) const
{
    AlbumGridLayout ag = {};
    ag.gap = Theme::AlbumCardGap;
    ag.paddingX = Theme::GalleryPadding;

    float availableWidth = viewWidth - ag.paddingX * 2.0f;
    ag.columns = std::max(1, static_cast<int>(
        (availableWidth + ag.gap) / (Theme::AlbumMinCardWidth + ag.gap)));

    ag.cardWidth = (availableWidth - ag.gap * (ag.columns - 1)) / ag.columns;
    if (ag.cardWidth > Theme::AlbumMaxCardWidth) {
        ag.columns = std::max(1, static_cast<int>(
            (availableWidth + ag.gap) / (Theme::AlbumMaxCardWidth + ag.gap)));
        ag.cardWidth = (availableWidth - ag.gap * (ag.columns - 1)) / ag.columns;
    }

    ag.imageHeight = ag.cardWidth;  // 1:1 cover
    ag.cardTotalHeight = ag.imageHeight + Theme::AlbumTextHeight;
    return ag;
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

void GalleryView::ComputeFolderDetailSectionLayouts(const GridLayout& grid) const
{
    folderDetailSectionLayouts_.clear();
    folderDetailSectionLayouts_.resize(folderDetailSections_.size());

    float y = Theme::GalleryHeaderHeight + Theme::GalleryPadding;

    for (size_t i = 0; i < folderDetailSections_.size(); ++i) {
        folderDetailSectionLayouts_[i].headerY = y;
        folderDetailSectionLayouts_[i].contentY = y + Theme::SectionHeaderHeight;
        folderDetailSectionLayouts_[i].rows = static_cast<int>(
            (folderDetailSections_[i].count + grid.columns - 1) / grid.columns);

        y = folderDetailSectionLayouts_[i].contentY +
            folderDetailSectionLayouts_[i].rows * (grid.cellSize + grid.gap);

        if (i + 1 < folderDetailSections_.size()) {
            y += Theme::SectionGap;
        }
    }

    folderDetailCachedTotalHeight_ = y + Theme::GalleryPadding;
}

std::wstring GalleryView::FormatNumber(size_t n)
{
    std::wstring s = std::to_wstring(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
        s.insert(i, L",");
    }
    return s;
}

// ======================= RENDER =======================

void GalleryView::Render(Rendering::Direct2DRenderer* renderer)
{
    if (!renderer) return;
    EnsureResources(renderer);

    auto* ctx = renderer->GetContext();
    if (!ctx) return;

    ctx->Clear(Theme::Background);

    float contentHeight = viewHeight_ - Theme::TabBarHeight;

    D2D1_RECT_F contentClip = D2D1::RectF(0, 0, viewWidth_, contentHeight);
    ctx->PushAxisAlignedClip(contentClip, D2D1_ANTIALIAS_MODE_ALIASED);

    if (activeTab_ == GalleryTab::Photos) {
        RenderPhotosTab(renderer, ctx, contentHeight);
    } else {
        if (folderTransitionActive_) {
            float t = std::clamp(folderSlide_.GetValue(), 0.0f, 1.0f);

            D2D1_MATRIX_3X2_F savedTransform;
            ctx->GetTransform(&savedTransform);

            // Albums grid slides left (parallax, 30% of screen width)
            ctx->SetTransform(
                D2D1::Matrix3x2F::Translation(-t * viewWidth_ * 0.3f, 0) * savedTransform);
            RenderAlbumsTab(renderer, ctx, contentHeight);

            // Folder detail slides in from right
            float detailOffset = (1.0f - t) * viewWidth_;
            ctx->SetTransform(
                D2D1::Matrix3x2F::Translation(detailOffset, 0) * savedTransform);
            // Opaque background so albums don't show through gaps
            if (bgBrush_) {
                ctx->FillRectangle(
                    D2D1::RectF(0, 0, viewWidth_, contentHeight), bgBrush_.Get());
            }
            RenderFolderDetail(renderer, ctx, contentHeight);

            // Edge shadow on left side of incoming folder detail
            {
                float shadowAlpha = 0.25f * (1.0f - t);
                if (shadowAlpha > 0.001f) {
                    D2D1_COLOR_F shadowColor = {0.0f, 0.0f, 0.0f, shadowAlpha};
                    ComPtr<ID2D1SolidColorBrush> shadowBrush;
                    ctx->CreateSolidColorBrush(shadowColor, &shadowBrush);
                    if (shadowBrush) {
                        ctx->FillRectangle(
                            D2D1::RectF(-12.0f, 0, 0, contentHeight), shadowBrush.Get());
                    }
                }
            }

            ctx->SetTransform(savedTransform);
        } else if (inFolderDetail_) {
            RenderFolderDetail(renderer, ctx, contentHeight);
        } else {
            RenderAlbumsTab(renderer, ctx, contentHeight);
        }
    }

    ctx->PopAxisAlignedClip();

    RenderTabBar(ctx);
}

// Helper: render a section-based image grid (shared by Photos tab & Folder Detail)
// Collects visible paths into outVisiblePaths for pipeline prioritization.
static void RenderImageGrid(
    ID2D1DeviceContext* ctx, ID2D1Factory* factory,
    Core::ImagePipeline* pipeline,
    const GalleryView::GridLayout& grid,
    const std::vector<std::filesystem::path>& images,
    const std::vector<GalleryView::SectionLayoutInfo>& layouts,
    const std::vector<GalleryView::Section>& sections,
    float scroll, float contentHeight, float viewWidth,
    float cornerRadius,
    ID2D1SolidColorBrush* cellBrush,
    ID2D1SolidColorBrush* textBrush,
    ID2D1SolidColorBrush* secondaryBrush,
    ID2D1SolidColorBrush* hoverBrush,
    IDWriteTextFormat* sectionFormat,
    IDWriteTextFormat* countRightFormat,
    float hoverX, float hoverY,
    std::optional<size_t> skipIndex,
    bool isFastScrolling,
    float dpiScale,
    std::vector<std::filesystem::path>* outVisiblePaths)
{
    // Cap thumbnail resolution to keep memory footprint small (160×160×4 = 100KB each)
    // so the cache can hold 10,000+ thumbnails without eviction.
    uint32_t targetPx = std::min(
        static_cast<uint32_t>(grid.cellSize * dpiScale),
        Theme::ThumbnailMaxPx);

    // Prefetch buffer: pre-decode 1.5 screens above and below the viewport
    // so thumbnails are ready before the user scrolls to them.
    float prefetchMargin = contentHeight * 1.5f;

    for (size_t s = 0; s < sections.size(); ++s) {
        const auto& section = sections[s];
        if (s >= layouts.size()) break;
        const auto& sl = layouts[s];

        float sectionEndY = sl.contentY + sl.rows * (grid.cellSize + grid.gap);
        // Skip sections entirely above prefetch zone
        if (sectionEndY - scroll < -prefetchMargin) continue;
        // Stop once past prefetch zone
        if (sl.headerY - scroll > contentHeight + prefetchMargin) break;

        // Section header (only draw if on screen)
        float headerScreenY = sl.headerY - scroll;
        if (headerScreenY + Theme::SectionHeaderHeight > 0 && headerScreenY < contentHeight) {
            if (sectionFormat && textBrush) {
                D2D1_RECT_F headerRect = D2D1::RectF(
                    grid.paddingX, headerScreenY + 8.0f,
                    viewWidth * 0.6f, headerScreenY + Theme::SectionHeaderHeight);
                ctx->DrawText(section.title.c_str(),
                              static_cast<UINT32>(section.title.size()),
                              sectionFormat, headerRect, textBrush);
            }
            if (countRightFormat && secondaryBrush) {
                auto countStr = GalleryView::FormatNumber(section.count) + L" photos";
                D2D1_RECT_F countRect = D2D1::RectF(
                    viewWidth * 0.5f, headerScreenY + 8.0f,
                    viewWidth - grid.paddingX, headerScreenY + Theme::SectionHeaderHeight);
                ctx->DrawText(countStr.c_str(), static_cast<UINT32>(countStr.size()),
                              countRightFormat, countRect, secondaryBrush);
            }
        }

        // Cells
        for (size_t i = 0; i < section.count; ++i) {
            int localRow = static_cast<int>(i) / grid.columns;
            int localCol = static_cast<int>(i) % grid.columns;

            float cellX = grid.paddingX + localCol * (grid.cellSize + grid.gap);
            float cellY = sl.contentY + localRow * (grid.cellSize + grid.gap) - scroll;

            // Skip cells above prefetch zone
            if (cellY + grid.cellSize < -prefetchMargin) continue;
            // Stop past prefetch zone
            if (cellY > contentHeight + prefetchMargin) break;

            size_t globalIndex = section.startIndex + i;
            if (globalIndex >= images.size()) break;

            bool onScreen = (cellY + grid.cellSize >= 0.0f && cellY <= contentHeight);

            D2D1_RECT_F cellRect = D2D1::RectF(
                cellX, cellY, cellX + grid.cellSize, cellY + grid.cellSize);
            D2D1_ROUNDED_RECT roundedCell = {cellRect, cornerRadius, cornerRadius};

            // Placeholder background (only for on-screen cells)
            if (onScreen && cellBrush) {
                ctx->FillRoundedRectangle(roundedCell, cellBrush);
            }

            if (skipIndex.has_value() && globalIndex == skipIndex.value()) continue;

            // Collect visible path (only actually on-screen cells, for eviction protection)
            if (onScreen && outVisiblePaths) {
                outVisiblePaths->push_back(images[globalIndex]);
            }

            // Thumbnail: request decode for visible + prefetch zone
            Microsoft::WRL::ComPtr<ID2D1Bitmap> thumbnail;
            if (pipeline) {
                if (isFastScrolling) {
                    // During fast scroll: show cached thumbnails on-screen, skip prefetch
                    if (onScreen) {
                        thumbnail = pipeline->GetCachedThumbnail(images[globalIndex]);
                    }
                } else {
                    // Normal scroll: request for both visible and prefetch cells
                    thumbnail = pipeline->RequestThumbnail(images[globalIndex], targetPx);
                }
            }

            // Only draw on-screen cells
            if (onScreen) {
                if (thumbnail) {
                    float cellW = cellRect.right - cellRect.left;
                    float cellH = cellRect.bottom - cellRect.top;
                    D2D1_RECT_F srcRect = ComputeCropRect(thumbnail.Get(), cellW, cellH);
                    DrawBitmapRounded(ctx, factory, thumbnail.Get(), cellRect, cornerRadius, &srcRect);
                }

                // Hover
                if (hoverBrush &&
                    hoverX >= cellRect.left && hoverX <= cellRect.right &&
                    hoverY >= cellRect.top && hoverY <= cellRect.bottom) {
                    ctx->FillRoundedRectangle(roundedCell, hoverBrush);
                }
            }
        }
    }
}

void GalleryView::RenderPhotosTab(Rendering::Direct2DRenderer* renderer,
                                   ID2D1DeviceContext* ctx, float contentHeight)
{
    auto grid = CalculateGridLayout(viewWidth_);
    cachedGrid_ = grid;
    cachedLayoutWidth_ = viewWidth_;

    ComputeSectionLayouts(grid);
    maxScroll_ = std::max(0.0f, cachedTotalHeight_ - contentHeight);

    float scroll = scrollY_.GetValue();

    // Flush decoded thumbnails to GPU (up to MaxBitmapsPerFrame per frame)
    if (pipeline_) {
        pipeline_->FlushReadyThumbnails(Theme::MaxBitmapsPerFrame);
    }

    // === Image grid FIRST (rendered behind header) ===
    auto* factory = renderer->GetFactory();
    float dpiScale = renderer->GetDpiX() / 96.0f;

    std::vector<std::filesystem::path> visiblePaths;
    RenderImageGrid(ctx, factory, pipeline_,
        grid, images_, sectionLayouts_, sections_,
        scroll, contentHeight, viewWidth_,
        Theme::ThumbnailCornerRadius,
        cellBrush_.Get(), textBrush_.Get(), secondaryBrush_.Get(), hoverBrush_.Get(),
        sectionFormat_.Get(), countRightFormat_.Get(),
        hoverX_, hoverY_, skipIndex_,
        isFastScrolling_, dpiScale, &visiblePaths);

    // Tell pipeline which paths are visible for prioritization
    if (pipeline_ && !visiblePaths.empty()) {
        pipeline_->SetVisibleRange(visiblePaths);
    }

    // === Header overlay (covers scrolling content) ===
    if (bgBrush_) {
        ctx->FillRectangle(
            D2D1::RectF(0, 0, viewWidth_, Theme::GalleryHeaderHeight),
            bgBrush_.Get());
    }

    if (textBrush_ && titleFormat_) {
        D2D1_RECT_F titleRect = D2D1::RectF(
            Theme::GalleryPadding, 10.0f,
            viewWidth_ - Theme::GalleryPadding, 54.0f);
        ctx->DrawText(L"\u7167\u7247", 2, titleFormat_.Get(), titleRect, textBrush_.Get());
    }

    if (countFormat_) {
        D2D1_RECT_F subtitleRect = D2D1::RectF(
            Theme::GalleryPadding, 54.0f,
            viewWidth_ - Theme::GalleryPadding, 74.0f);

        if (isScanning_) {
            std::wstring sub = L"Scanning... " + FormatNumber(scanCount_) + L" photos found";
            if (accentBrush_) {
                ctx->DrawText(sub.c_str(), static_cast<UINT32>(sub.size()),
                              countFormat_.Get(), subtitleRect, accentBrush_.Get());
            }
        } else if (images_.empty()) {
            if (secondaryBrush_) {
                std::wstring sub = L"No photos found  \u00B7  Ctrl+O browse  \u00B7  Ctrl+D add folder";
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

    // Scanning progress
    if (isScanning_ && accentBrush_) {
        float barY = Theme::GalleryHeaderHeight - 2.0f;
        float progress = std::fmod(static_cast<float>(scanCount_) * 0.01f, 1.0f);
        float barWidth = viewWidth_ * 0.3f;
        float barX = progress * (viewWidth_ - barWidth);
        D2D1_ROUNDED_RECT progressBar = {
            D2D1::RectF(barX, barY, barX + barWidth, barY + 2.0f), 1.0f, 1.0f};
        ctx->FillRoundedRectangle(progressBar, accentBrush_.Get());
    }

    // Scroll indicator
    if (maxScroll_ > 0.0f && !images_.empty()) {
        float scrollRatio = std::max(0.0f, std::min(1.0f, scroll / maxScroll_));
        float indicatorHeight = std::max(40.0f, contentHeight * (contentHeight / cachedTotalHeight_));
        float indicatorTop = scrollRatio * (contentHeight - indicatorHeight);
        D2D1_ROUNDED_RECT indicatorRect = {
            D2D1::RectF(viewWidth_ - 5.0f, indicatorTop + 4.0f,
                         viewWidth_ - 2.0f, indicatorTop + indicatorHeight - 4.0f),
            1.5f, 1.5f
        };
        if (scrollIndicatorBrush_) {
            ctx->FillRoundedRectangle(indicatorRect, scrollIndicatorBrush_.Get());
        }
    }

    // Empty state
    if (images_.empty() && !isScanning_) {
        float cx = viewWidth_ * 0.5f;
        float cy = contentHeight * 0.50f;

        if (secondaryBrush_) {
            // Simple photo icon
            D2D1_ROUNDED_RECT iconRect = {
                D2D1::RectF(cx - 36.0f, cy - 36.0f, cx + 36.0f, cy + 36.0f),
                8.0f, 8.0f
            };
            ctx->DrawRoundedRectangle(iconRect, secondaryBrush_.Get(), 1.5f);

            D2D1_ELLIPSE sun = {D2D1::Point2F(cx + 14.0f, cy - 14.0f), 7.0f, 7.0f};
            ctx->FillEllipse(sun, secondaryBrush_.Get());
        }

        if (countFormat_ && secondaryBrush_) {
            D2D1_RECT_F hintRect = D2D1::RectF(0, cy + 56.0f, viewWidth_, cy + 80.0f);
            const wchar_t* hint = L"Drag images here \u00B7 Ctrl+O browse \u00B7 Ctrl+D add folder";
            ctx->DrawText(hint, static_cast<UINT32>(wcslen(hint)),
                          countFormat_.Get(), hintRect, secondaryBrush_.Get());
        }
    }
}

void GalleryView::RenderAlbumsTab(Rendering::Direct2DRenderer* renderer,
                                   ID2D1DeviceContext* ctx, float contentHeight)
{
    // Flush decoded thumbnails to GPU
    if (pipeline_) {
        pipeline_->FlushReadyThumbnails(Theme::MaxBitmapsPerFrame);
    }

    auto* factory = renderer->GetFactory();
    float scroll = albumsScrollY_.GetValue();

    if (folderAlbums_.empty()) {
        // Header for empty state (no scrolling content to overlap)
        if (textBrush_ && titleFormat_) {
            D2D1_RECT_F titleRect = D2D1::RectF(
                Theme::GalleryPadding, 10.0f,
                viewWidth_ - Theme::GalleryPadding, 54.0f);
            ctx->DrawText(L"\u76F8\u518C", 2, titleFormat_.Get(), titleRect, textBrush_.Get());
        }
        if (countFormat_ && secondaryBrush_) {
            float cy = contentHeight * 0.5f;
            D2D1_RECT_F hintRect = D2D1::RectF(0, cy - 15.0f, viewWidth_, cy + 15.0f);
            const wchar_t* hint = L"No albums yet  \u00B7  Ctrl+D add folder";
            ctx->DrawText(hint, static_cast<UINT32>(wcslen(hint)),
                          countFormat_.Get(), hintRect, secondaryBrush_.Get());
        }
        return;
    }

    // Responsive album card grid
    auto ag = CalculateAlbumGridLayout(viewWidth_);
    float cornerRadius = Theme::AlbumCornerRadius;

    float startY = Theme::GalleryHeaderHeight + Theme::GalleryPadding;

    int numRows = static_cast<int>((folderAlbums_.size() + ag.columns - 1) / ag.columns);
    float totalHeight = startY + numRows * (ag.cardTotalHeight + ag.gap) + ag.paddingX;
    albumsMaxScroll_ = std::max(0.0f, totalHeight - contentHeight);

    for (size_t i = 0; i < folderAlbums_.size(); ++i) {
        int col = static_cast<int>(i) % ag.columns;
        int row = static_cast<int>(i) / ag.columns;

        float cardX = ag.paddingX + col * (ag.cardWidth + ag.gap);
        float cardY = startY + row * (ag.cardTotalHeight + ag.gap) - scroll;

        if (cardY + ag.cardTotalHeight < 0.0f) continue;
        if (cardY > contentHeight) break;

        // Cover image (1:1, rounded)
        D2D1_RECT_F imgRect = D2D1::RectF(cardX, cardY,
                                            cardX + ag.cardWidth, cardY + ag.imageHeight);
        D2D1_ROUNDED_RECT roundedImg = {imgRect, cornerRadius, cornerRadius};

        if (cellBrush_) {
            ctx->FillRoundedRectangle(roundedImg, cellBrush_.Get());
        }

        uint32_t albumTargetPx = std::min(
            static_cast<uint32_t>(ag.cardWidth * (renderer ? renderer->GetDpiX() / 96.0f : 1.0f)),
            Theme::ThumbnailMaxPx);
        auto thumbnail = pipeline_
            ? pipeline_->RequestThumbnail(folderAlbums_[i].coverImage, albumTargetPx)
            : nullptr;
        if (thumbnail) {
            D2D1_RECT_F srcRect = ComputeCropRect(thumbnail.Get(), ag.cardWidth, ag.imageHeight);
            DrawBitmapRounded(ctx, factory, thumbnail.Get(), imgRect, cornerRadius, &srcRect);
        }

        // Hover
        if (hoverBrush_ &&
            hoverX_ >= imgRect.left && hoverX_ <= imgRect.right &&
            hoverY_ >= imgRect.top && hoverY_ <= imgRect.bottom) {
            ctx->FillRoundedRectangle(roundedImg, hoverBrush_.Get());
        }

        // Text below cover
        float textY = cardY + ag.imageHeight + 6.0f;

        if (albumTitleFormat_ && textBrush_) {
            D2D1_RECT_F nameRect = D2D1::RectF(
                cardX + 2.0f, textY,
                cardX + ag.cardWidth - 2.0f, textY + 22.0f);
            const auto& name = folderAlbums_[i].displayName;
            ctx->DrawText(name.c_str(), static_cast<UINT32>(name.size()),
                          albumTitleFormat_.Get(), nameRect, textBrush_.Get());
        }

        if (albumCountFormat_ && secondaryBrush_) {
            D2D1_RECT_F countRect = D2D1::RectF(
                cardX + 2.0f, textY + 22.0f,
                cardX + ag.cardWidth - 2.0f, textY + 38.0f);
            std::wstring countStr = FormatNumber(folderAlbums_[i].imageCount);
            ctx->DrawText(countStr.c_str(), static_cast<UINT32>(countStr.size()),
                          albumCountFormat_.Get(), countRect, secondaryBrush_.Get());
        }
    }

    // === Header overlay (covers scrolling album cards) ===
    if (bgBrush_) {
        ctx->FillRectangle(
            D2D1::RectF(0, 0, viewWidth_, Theme::GalleryHeaderHeight),
            bgBrush_.Get());
    }

    if (textBrush_ && titleFormat_) {
        D2D1_RECT_F titleRect = D2D1::RectF(
            Theme::GalleryPadding, 10.0f,
            viewWidth_ - Theme::GalleryPadding, 54.0f);
        ctx->DrawText(L"\u76F8\u518C", 2, titleFormat_.Get(), titleRect, textBrush_.Get());
    }

    if (countFormat_ && secondaryBrush_) {
        D2D1_RECT_F subtitleRect = D2D1::RectF(
            Theme::GalleryPadding, 54.0f,
            viewWidth_ - Theme::GalleryPadding, 74.0f);
        std::wstring sub = FormatNumber(folderAlbums_.size()) + L" albums";
        ctx->DrawText(sub.c_str(), static_cast<UINT32>(sub.size()),
                      countFormat_.Get(), subtitleRect, secondaryBrush_.Get());
    }

    // Scroll indicator
    if (albumsMaxScroll_ > 0.0f) {
        float scrollRatio = std::max(0.0f, std::min(1.0f, scroll / albumsMaxScroll_));
        float indicatorHeight = std::max(40.0f, contentHeight * (contentHeight / totalHeight));
        float indicatorTop = scrollRatio * (contentHeight - indicatorHeight);
        D2D1_ROUNDED_RECT indicatorRect = {
            D2D1::RectF(viewWidth_ - 5.0f, indicatorTop + 4.0f,
                         viewWidth_ - 2.0f, indicatorTop + indicatorHeight - 4.0f),
            1.5f, 1.5f
        };
        if (scrollIndicatorBrush_) {
            ctx->FillRoundedRectangle(indicatorRect, scrollIndicatorBrush_.Get());
        }
    }
}

void GalleryView::RenderFolderDetail(Rendering::Direct2DRenderer* renderer,
                                      ID2D1DeviceContext* ctx, float contentHeight)
{
    if (openFolderIndex_ >= folderAlbums_.size()) return;
    const auto& album = folderAlbums_[openFolderIndex_];

    auto grid = CalculateGridLayout(viewWidth_);
    ComputeFolderDetailSectionLayouts(grid);
    folderDetailMaxScroll_ = std::max(0.0f, folderDetailCachedTotalHeight_ - contentHeight);

    float scroll = folderDetailScrollY_.GetValue();

    // Flush decoded thumbnails to GPU
    if (pipeline_) {
        pipeline_->FlushReadyThumbnails(Theme::MaxBitmapsPerFrame);
    }

    // === Image grid FIRST (rendered behind header) ===
    auto* factory = renderer->GetFactory();
    float dpiScale = renderer->GetDpiX() / 96.0f;

    std::vector<std::filesystem::path> visiblePaths;
    RenderImageGrid(ctx, factory, pipeline_,
        grid, folderDetailImages_, folderDetailSectionLayouts_, folderDetailSections_,
        scroll, contentHeight, viewWidth_,
        Theme::ThumbnailCornerRadius,
        cellBrush_.Get(), textBrush_.Get(), secondaryBrush_.Get(), hoverBrush_.Get(),
        sectionFormat_.Get(), countRightFormat_.Get(),
        hoverX_, hoverY_, skipIndex_,
        isFastScrolling_, dpiScale, &visiblePaths);

    // Tell pipeline which paths are visible for prioritization
    if (pipeline_ && !visiblePaths.empty()) {
        pipeline_->SetVisibleRange(visiblePaths);
    }

    // === Header overlay (covers scrolling content) ===
    if (bgBrush_) {
        ctx->FillRectangle(
            D2D1::RectF(0, 0, viewWidth_, Theme::GalleryHeaderHeight),
            bgBrush_.Get());
    }

    // Back button (pill shape) + folder title on same row — Y: 10..38
    {
        float btnX = Theme::GalleryPadding;
        float btnY = 10.0f;
        float btnH = 28.0f;
        float btnW = 76.0f;
        D2D1_RECT_F btnRect = D2D1::RectF(btnX, btnY, btnX + btnW, btnY + btnH);
        D2D1_ROUNDED_RECT pill = {btnRect, btnH * 0.5f, btnH * 0.5f};

        bool hovered = (hoverX_ >= btnRect.left && hoverX_ <= btnRect.right &&
                        hoverY_ >= btnRect.top && hoverY_ <= btnRect.bottom);
        if (cellBrush_) ctx->FillRoundedRectangle(pill, cellBrush_.Get());
        if (hovered && hoverBrush_) {
            ctx->FillRoundedRectangle(pill, hoverBrush_.Get());
        }

        if (backButtonFormat_ && accentBrush_) {
            D2D1_RECT_F textRect = D2D1::RectF(btnX, btnY, btnX + btnW, btnY + btnH);
            backButtonFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            ctx->DrawText(L"\u2190 Back", 6, backButtonFormat_.Get(), textRect, accentBrush_.Get());
            backButtonFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }

        // Folder title — to the right of back button, same row, with ellipsis trimming
        if (textBrush_ && backButtonFormat_) {
            float titleX = btnX + btnW + 10.0f;
            float titleMaxW = viewWidth_ - Theme::GalleryPadding - titleX;
            if (titleMaxW > 0) {
                Microsoft::WRL::ComPtr<IDWriteFactory> dwFactory;
                HRESULT hr = DWriteCreateFactory(
                    DWRITE_FACTORY_TYPE_SHARED,
                    __uuidof(IDWriteFactory),
                    reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf()));
                if (SUCCEEDED(hr) && dwFactory) {
                    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
                    hr = dwFactory->CreateTextLayout(
                        album.displayName.c_str(),
                        static_cast<UINT32>(album.displayName.size()),
                        backButtonFormat_.Get(),
                        titleMaxW, btnH, &layout);
                    if (SUCCEEDED(hr) && layout) {
                        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                        Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
                        dwFactory->CreateEllipsisTrimmingSign(backButtonFormat_.Get(), &ellipsis);
                        DWRITE_TRIMMING trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                        layout->SetTrimming(&trimming, ellipsis.Get());
                        ctx->DrawTextLayout(D2D1::Point2F(titleX, btnY), layout.Get(), textBrush_.Get());
                    }
                }
            }
        }
    }

    // Subtitle — Y: 42..58 (shifted up since title moved into back button row)
    if (countFormat_ && secondaryBrush_) {
        D2D1_RECT_F subtitleRect = D2D1::RectF(
            Theme::GalleryPadding, 42.0f,
            viewWidth_ - Theme::GalleryPadding, 58.0f);
        std::wstring sub = FormatNumber(folderDetailImages_.size()) + L" photos";
        ctx->DrawText(sub.c_str(), static_cast<UINT32>(sub.size()),
                      countFormat_.Get(), subtitleRect, secondaryBrush_.Get());
    }

    // Scroll indicator
    if (folderDetailMaxScroll_ > 0.0f) {
        float scrollRatio = std::max(0.0f, std::min(1.0f, scroll / folderDetailMaxScroll_));
        float indicatorHeight = std::max(40.0f, contentHeight * (contentHeight / folderDetailCachedTotalHeight_));
        float indicatorTop = scrollRatio * (contentHeight - indicatorHeight);
        D2D1_ROUNDED_RECT indicatorRect = {
            D2D1::RectF(viewWidth_ - 5.0f, indicatorTop + 4.0f,
                         viewWidth_ - 2.0f, indicatorTop + indicatorHeight - 4.0f),
            1.5f, 1.5f
        };
        if (scrollIndicatorBrush_) {
            ctx->FillRoundedRectangle(indicatorRect, scrollIndicatorBrush_.Get());
        }
    }
}

void GalleryView::RenderTabBar(ID2D1DeviceContext* ctx)
{
    float tabBarTop = viewHeight_ - Theme::TabBarHeight;

    // Background
    D2D1_RECT_F barRect = D2D1::RectF(0, tabBarTop, viewWidth_, viewHeight_);
    if (tabBarBrush_) {
        ctx->FillRectangle(barRect, tabBarBrush_.Get());
    }

    // Subtle top border
    if (scrollIndicatorBrush_) {
        D2D1_RECT_F lineRect = D2D1::RectF(0, tabBarTop, viewWidth_, tabBarTop + 0.5f);
        ctx->FillRectangle(lineRect, scrollIndicatorBrush_.Get());
    }

    float halfWidth = viewWidth_ / 2.0f;

    struct TabInfo {
        const wchar_t* label;
        size_t labelLen;
        GalleryTab tab;
    };
    TabInfo tabs[] = {
        {L"\u7167\u7247", 2, GalleryTab::Photos},
        {L"\u76F8\u518C", 2, GalleryTab::Albums},
    };

    // Animated tab indicator
    if (accentBrush_) {
        float indicatorW = 28.0f;
        float pos0 = halfWidth * 0.5f - indicatorW * 0.5f;
        float pos1 = halfWidth + halfWidth * 0.5f - indicatorW * 0.5f;
        float tabT = std::clamp(tabSlide_.GetValue(), 0.0f, 1.0f);
        float indicatorX = pos0 + tabT * (pos1 - pos0);
        D2D1_ROUNDED_RECT indicator = {
            D2D1::RectF(indicatorX, tabBarTop + 4.0f,
                         indicatorX + indicatorW, tabBarTop + 6.5f),
            1.25f, 1.25f
        };
        ctx->FillRoundedRectangle(indicator, accentBrush_.Get());
    }

    for (int t = 0; t < 2; ++t) {
        float tabX = t * halfWidth;
        bool isActive = (activeTab_ == tabs[t].tab);

        // Label
        D2D1_RECT_F tabRect = D2D1::RectF(tabX, tabBarTop + 8.0f,
                                            tabX + halfWidth, viewHeight_ - 4.0f);

        if (tabFormat_) {
            auto* brush = isActive ? accentBrush_.Get() : secondaryBrush_.Get();
            if (brush) {
                ctx->DrawText(tabs[t].label, static_cast<UINT32>(tabs[t].labelLen),
                              tabFormat_.Get(), tabRect, brush);
            }
        }
    }
}

// ======================= UPDATE =======================

void GalleryView::Update(float deltaTime)
{
    scrollY_.Update(deltaTime);
    albumsScrollY_.Update(deltaTime);
    folderDetailScrollY_.Update(deltaTime);
    folderSlide_.Update(deltaTime);
    tabSlide_.Update(deltaTime);

    // --- Fast-scroll detection ---
    {
        // Pick the active scroll animation's velocity
        float rawVelocity = 0.0f;
        if (activeTab_ == GalleryTab::Photos) {
            rawVelocity = std::abs(scrollY_.GetVelocity());
        } else if (inFolderDetail_) {
            rawVelocity = std::abs(folderDetailScrollY_.GetVelocity());
        } else {
            rawVelocity = std::abs(albumsScrollY_.GetVelocity());
        }

        scrollVelocitySmoothed_ = scrollVelocitySmoothed_ * 0.8f + rawVelocity * 0.2f;
        bool wasFastScrolling = isFastScrolling_;
        isFastScrolling_ = scrollVelocitySmoothed_ > Theme::FastScrollThreshold;

        if (isFastScrolling_ && !wasFastScrolling && pipeline_) {
            pipeline_->InvalidateRequests();
        }
    }

    // Check folder navigation transition completion
    if (folderTransitionActive_ && folderSlide_.IsFinished()) {
        folderTransitionActive_ = false;
        if (!folderTransitionForward_) {
            // Pop animation completed — clean up
            inFolderDetail_ = false;
            folderDetailImages_.clear();
            folderDetailSections_.clear();
        }
    }

    auto rubberBand = [&](Animation::SpringAnimation& spring, float maxScr) {
        if (!isDragging_) {
            float currentValue = spring.GetValue();
            float velocity = std::abs(spring.GetVelocity());
            if (velocity < 500.0f) {
                if (currentValue < 0.0f) {
                    spring.SetTarget(0.0f);
                    if (velocity < 100.0f)
                        spring.SetConfig({Theme::RubberBandStiffness, Theme::RubberBandDamping, 1.0f, 0.5f});
                } else if (currentValue > maxScr && maxScr > 0.0f) {
                    spring.SetTarget(maxScr);
                    if (velocity < 100.0f)
                        spring.SetConfig({Theme::RubberBandStiffness, Theme::RubberBandDamping, 1.0f, 0.5f});
                }
            }
        }
    };

    if (activeTab_ == GalleryTab::Photos) {
        rubberBand(scrollY_, maxScroll_);
    } else if (inFolderDetail_) {
        rubberBand(folderDetailScrollY_, folderDetailMaxScroll_);
    } else {
        rubberBand(albumsScrollY_, albumsMaxScroll_);
    }
}

// ======================= INPUT =======================

void GalleryView::OnMouseWheel(float delta)
{
    float scrollAmount = -delta * 2.5f;

    if (activeTab_ == GalleryTab::Photos) {
        float newTarget = scrollY_.GetTarget() + scrollAmount;
        newTarget = std::max(-80.0f, std::min(newTarget, maxScroll_ + 80.0f));
        scrollY_.SetTarget(newTarget);
        scrollY_.SetConfig({Theme::ScrollStiffness, Theme::ScrollDamping, 1.0f, 0.5f});
    } else if (inFolderDetail_) {
        float newTarget = folderDetailScrollY_.GetTarget() + scrollAmount;
        newTarget = std::max(-80.0f, std::min(newTarget, folderDetailMaxScroll_ + 80.0f));
        folderDetailScrollY_.SetTarget(newTarget);
        folderDetailScrollY_.SetConfig({Theme::ScrollStiffness, Theme::ScrollDamping, 1.0f, 0.5f});
    } else {
        float newTarget = albumsScrollY_.GetTarget() + scrollAmount;
        newTarget = std::max(-80.0f, std::min(newTarget, albumsMaxScroll_ + 80.0f));
        albumsScrollY_.SetTarget(newTarget);
        albumsScrollY_.SetConfig({Theme::ScrollStiffness, Theme::ScrollDamping, 1.0f, 0.5f});
    }
}

void GalleryView::OnMouseDown(float x, float y)
{
    isDragging_ = true;
    hasDragged_ = false;
    dragStartX_ = x;
    dragStartY_ = y;
    lastDragY_ = y;
    scrollVelocity_ = 0.0f;

    if (activeTab_ == GalleryTab::Photos) {
        dragStartScroll_ = scrollY_.GetValue();
    } else if (inFolderDetail_) {
        dragStartScroll_ = folderDetailScrollY_.GetValue();
    } else {
        dragStartScroll_ = albumsScrollY_.GetValue();
    }
}

void GalleryView::OnMouseMove(float x, float y)
{
    hoverX_ = x;
    hoverY_ = y;

    if (isDragging_) {
        if (!hasDragged_) {
            float totalDx = std::abs(x - dragStartX_);
            float totalDy = std::abs(y - dragStartY_);
            if (totalDx > 5.0f || totalDy > 5.0f) hasDragged_ = true;
        }
        float dy = dragStartY_ - y;
        float newScroll = dragStartScroll_ + dy;

        float currentMaxScroll = maxScroll_;
        Animation::SpringAnimation* activeScroll = &scrollY_;

        if (activeTab_ == GalleryTab::Albums && !inFolderDetail_) {
            currentMaxScroll = albumsMaxScroll_;
            activeScroll = &albumsScrollY_;
        } else if (inFolderDetail_) {
            currentMaxScroll = folderDetailMaxScroll_;
            activeScroll = &folderDetailScrollY_;
        }

        if (newScroll < 0.0f) {
            newScroll *= 0.3f;
        } else if (newScroll > currentMaxScroll) {
            float excess = newScroll - currentMaxScroll;
            newScroll = currentMaxScroll + excess * 0.3f;
        }

        activeScroll->SetValue(newScroll);
        activeScroll->SetTarget(newScroll);

        scrollVelocity_ = (lastDragY_ - y) * 60.0f;
        lastDragY_ = y;
    }
}

void GalleryView::OnMouseUp(float x, float y)
{
    consumedClick_ = false;

    if (isDragging_) {
        isDragging_ = false;

        float currentMaxScroll = maxScroll_;
        Animation::SpringAnimation* activeScroll = &scrollY_;

        if (activeTab_ == GalleryTab::Albums && !inFolderDetail_) {
            currentMaxScroll = albumsMaxScroll_;
            activeScroll = &albumsScrollY_;
        } else if (inFolderDetail_) {
            currentMaxScroll = folderDetailMaxScroll_;
            activeScroll = &folderDetailScrollY_;
        }

        float inertiaTarget = activeScroll->GetValue() + scrollVelocity_ * 0.6f;
        inertiaTarget = std::max(-80.0f, std::min(inertiaTarget, currentMaxScroll + 80.0f));
        activeScroll->SetTarget(inertiaTarget);
        activeScroll->SetConfig({Theme::ScrollStiffness, Theme::ScrollDamping, 1.0f, 0.5f});
    }

    if (!hasDragged_) {
        // Tab bar (always clickable, even during transition)
        float tabBarTop = viewHeight_ - Theme::TabBarHeight;
        if (y >= tabBarTop && y <= viewHeight_) {
            float halfWidth = viewWidth_ / 2.0f;
            if (x < halfWidth) {
                activeTab_ = GalleryTab::Photos;
                tabSlide_.SetTarget(0.0f);
                if (inFolderDetail_) {
                    // Instant exit when switching tabs (no animation)
                    inFolderDetail_ = false;
                    folderTransitionActive_ = false;
                    folderDetailImages_.clear();
                    folderDetailSections_.clear();
                }
            } else {
                activeTab_ = GalleryTab::Albums;
                tabSlide_.SetTarget(1.0f);
            }
            consumedClick_ = true;
            return;
        }

        // Block other clicks during folder navigation transition
        if (folderTransitionActive_) {
            consumedClick_ = true;
            return;
        }

        // Back button in folder detail — match actual pill rect
        if (activeTab_ == GalleryTab::Albums && inFolderDetail_) {
            float btnX = Theme::GalleryPadding;
            float btnY = 10.0f;
            float btnW = 76.0f;
            float btnH = 28.0f;
            if (x >= btnX && x <= btnX + btnW && y >= btnY && y <= btnY + btnH) {
                ExitFolderDetail();
                consumedClick_ = true;
                return;
            }
        }

        // Album card click
        if (activeTab_ == GalleryTab::Albums && !inFolderDetail_) {
            auto ag = CalculateAlbumGridLayout(viewWidth_);
            float scroll = albumsScrollY_.GetValue();
            float startY = Theme::GalleryHeaderHeight + Theme::GalleryPadding;
            float worldY = y + scroll;

            for (size_t i = 0; i < folderAlbums_.size(); ++i) {
                int col = static_cast<int>(i) % ag.columns;
                int row = static_cast<int>(i) / ag.columns;

                float cardX = ag.paddingX + col * (ag.cardWidth + ag.gap);
                float cardYPos = startY + row * (ag.cardTotalHeight + ag.gap);

                if (x >= cardX && x <= cardX + ag.cardWidth &&
                    worldY >= cardYPos && worldY <= cardYPos + ag.cardTotalHeight) {
                    EnterFolderDetail(i);
                    consumedClick_ = true;
                    return;
                }
            }
        }
    }
}

std::optional<GalleryView::HitResult> GalleryView::HitTest(float x, float y) const
{
    float tabBarTop = viewHeight_ - Theme::TabBarHeight;
    if (y >= tabBarTop) return std::nullopt;
    if (activeTab_ == GalleryTab::Albums && !inFolderDetail_) return std::nullopt;

    auto grid = CalculateGridLayout(viewWidth_);

    const auto& sections = (inFolderDetail_) ? folderDetailSections_ : sections_;
    const auto& images = (inFolderDetail_) ? folderDetailImages_ : images_;

    if (inFolderDetail_) {
        ComputeFolderDetailSectionLayouts(grid);
    } else {
        ComputeSectionLayouts(grid);
    }

    const auto& layouts = (inFolderDetail_) ? folderDetailSectionLayouts_ : sectionLayouts_;
    float scroll = (inFolderDetail_) ? folderDetailScrollY_.GetValue() : scrollY_.GetValue();

    float worldY = y + scroll;

    for (size_t s = 0; s < sections.size(); ++s) {
        const auto& section = sections[s];
        if (s >= layouts.size()) break;
        const auto& sl = layouts[s];

        float contentEnd = sl.contentY + sl.rows * (grid.cellSize + grid.gap);
        if (worldY < sl.contentY || worldY >= contentEnd) continue;

        float localY = worldY - sl.contentY;
        int row = static_cast<int>(localY / (grid.cellSize + grid.gap));

        float cellYOffset = localY - row * (grid.cellSize + grid.gap);
        if (cellYOffset > grid.cellSize) continue;

        for (int col = 0; col < grid.columns; ++col) {
            float cellX = grid.paddingX + col * (grid.cellSize + grid.gap);

            if (x >= cellX && x <= cellX + grid.cellSize) {
                size_t localIndex = static_cast<size_t>(row) * grid.columns + col;
                if (localIndex >= section.count) continue;

                size_t globalIndex = section.startIndex + localIndex;
                if (globalIndex >= images.size()) continue;

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
    const auto& images = (inFolderDetail_) ? folderDetailImages_ : images_;
    const auto& sections = (inFolderDetail_) ? folderDetailSections_ : sections_;

    if (index >= images.size()) return std::nullopt;

    auto grid = CalculateGridLayout(viewWidth_);

    if (inFolderDetail_) {
        ComputeFolderDetailSectionLayouts(grid);
    } else {
        ComputeSectionLayouts(grid);
    }

    const auto& layouts = (inFolderDetail_) ? folderDetailSectionLayouts_ : sectionLayouts_;
    float scroll = (inFolderDetail_) ? folderDetailScrollY_.GetValue() : scrollY_.GetValue();

    for (size_t s = 0; s < sections.size(); ++s) {
        if (index >= sections[s].startIndex &&
            index < sections[s].startIndex + sections[s].count) {

            size_t localIndex = index - sections[s].startIndex;
            int row = static_cast<int>(localIndex) / grid.columns;
            int col = static_cast<int>(localIndex) % grid.columns;

            float cellX = grid.paddingX + col * (grid.cellSize + grid.gap);
            float cellY = layouts[s].contentY +
                row * (grid.cellSize + grid.gap) - scroll;

            return D2D1::RectF(cellX, cellY, cellX + grid.cellSize, cellY + grid.cellSize);
        }
    }
    return std::nullopt;
}

} // namespace UI
} // namespace UltraImageViewer
