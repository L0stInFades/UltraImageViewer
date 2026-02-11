#include "ui/GalleryView.hpp"
#include "ui/Theme.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>
#include <d2d1_1.h>
#include <d2d1effects.h>
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
    , editBadgeScale_(Animation::SpringConfig{Theme::EditBadgeStiffness, Theme::EditBadgeDamping, 1.0f, 0.01f})
    , deleteCardScale_(Animation::SpringConfig{Theme::DeleteShrinkStiffness, Theme::DeleteShrinkDamping, 1.0f, 0.01f})
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

    editBadgeScale_.SetValue(0.0f);
    editBadgeScale_.SetTarget(0.0f);
    editBadgeScale_.SnapToTarget();

    deleteCardScale_.SetValue(1.0f);
    deleteCardScale_.SetTarget(1.0f);
    deleteCardScale_.SnapToTarget();
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

    // Edit mode resilience: re-init jiggle phases for new album count
    if (editMode_) {
        jigglePhases_.resize(folderAlbums_.size() + 1);
        for (auto& phase : jigglePhases_) {
            phase = static_cast<float>(rand()) / static_cast<float>(RAND_MAX) * 6.2831853f;
        }
        deletingCardIndex_ = -1;
        deleteCardScale_.SetValue(1.0f);
        deleteCardScale_.SetTarget(1.0f);
        deleteCardScale_.SnapToTarget();
    }
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

void GalleryView::SetManualOpenMode(bool enabled)
{
    manualOpenMode_ = enabled;
}

void GalleryView::SetBackToLibraryCallback(std::function<void()> cb)
{
    backToLibraryCallback_ = std::move(cb);
}

void GalleryView::SetDeleteAlbumCallback(std::function<void(const std::filesystem::path&)> cb)
{
    deleteAlbumCallback_ = std::move(cb);
}

void GalleryView::SetAddAlbumCallback(std::function<void()> cb)
{
    addAlbumCallback_ = std::move(cb);
}

void GalleryView::SetFolderVisitCallback(std::function<void(const std::filesystem::path&)> cb)
{
    folderVisitCallback_ = std::move(cb);
}

void GalleryView::SetEditMode(bool enabled)
{
    if (editMode_ == enabled) return;
    editMode_ = enabled;

    if (enabled) {
        // Generate random jiggle phases (one per album + 1 for add card)
        jigglePhases_.resize(folderAlbums_.size() + 1);
        for (auto& phase : jigglePhases_) {
            phase = static_cast<float>(rand()) / static_cast<float>(RAND_MAX) * 6.2831853f;
        }
        editModeTime_ = 0.0f;
        editBadgeScale_.SetValue(0.0f);
        editBadgeScale_.SetTarget(1.0f);
        deleteCardScale_.SetValue(1.0f);
        deleteCardScale_.SetTarget(1.0f);
        deleteCardScale_.SnapToTarget();
        deletingCardIndex_ = -1;
    } else {
        editBadgeScale_.SetTarget(0.0f);
        deletingCardIndex_ = -1;
    }
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

    // Record folder visit for access profile (Ledger-inspired prefetch prioritization)
    if (folderVisitCallback_) {
        folderVisitCallback_(album.folderPath);
    }

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

    // Pre-warm decode pipeline: request first batch of thumbnails so they're
    // decoding during the ~300ms slide animation and ready when it ends
    if (pipeline_) {
        size_t preload = std::min(folderDetailImages_.size(), size_t(40));
        for (size_t i = 0; i < preload; ++i) {
            pipeline_->RequestThumbnail(folderDetailImages_[i], Theme::ThumbnailMaxPx);
        }
    }

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

    // Glass brushes
    glassTintBrush_ = renderer->CreateBrush(Theme::GlassTintColor);
    glassBorderBrush_ = renderer->CreateBrush(Theme::GlassBorderColor);
    glassHighlightBrush_ = renderer->CreateBrush(Theme::GlassHighlightColor);
    glassActivePillBrush_ = renderer->CreateBrush(Theme::GlassActivePillColor);
    glassActivePillBorderBrush_ = renderer->CreateBrush(Theme::GlassActivePillBorder);
    glassTabTextBrush_ = renderer->CreateBrush(Theme::GlassTabTextActive);
    glassTabTextInactiveBrush_ = renderer->CreateBrush(Theme::GlassTabTextInactive);

    // Tab bar text (glass style)
    tabFormat_ = renderer->CreateTextFormat(L"Segoe UI", 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (tabFormat_) {
        tabFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        tabFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // Album card text
    albumTitleFormat_ = renderer->CreateTextFormat(L"Segoe UI", 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (albumTitleFormat_) {
        albumTitleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        albumTitleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        albumTitleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        albumTitleFormat_->SetTrimming(&trimming, nullptr);
    }

    albumCountFormat_ = renderer->CreateTextFormat(L"Segoe UI", 12.0f);
    if (albumCountFormat_) {
        albumCountFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        albumCountFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    backButtonFormat_ = renderer->CreateTextFormat(L"Segoe UI", 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (backButtonFormat_) {
        backButtonFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        backButtonFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // Edit mode brushes
    editBadgeBrush_ = renderer->CreateBrush(Theme::EditBadgeColor);
    editBadgeIconBrush_ = renderer->CreateBrush(Theme::EditBadgeIconColor);
    addCardBorderBrush_ = renderer->CreateBrush(Theme::AddCardBorderColor);
    addCardIconBrush_ = renderer->CreateBrush(Theme::AddCardIconColor);

    editButtonFormat_ = renderer->CreateTextFormat(L"Segoe UI", 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (editButtonFormat_) {
        editButtonFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        editButtonFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // Cache DWrite factory (avoid per-frame COM allocation)
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwFactory_.GetAddressOf()));

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
    EnsureOffscreenBitmap(renderer);

    auto* ctx = renderer->GetContext();
    if (!ctx) return;

    EnsureGlassEffects(ctx);

    // Compute frame budget deadline for content rendering.
    // Content gets ContentBudgetMs; remaining time is reserved for glass overlays.
    QueryPerformanceFrequency(&framePerfFreq_);
    QueryPerformanceCounter(&frameStart_);
    frameBudgetDeadline_.QuadPart = frameStart_.QuadPart +
        static_cast<LONGLONG>(Theme::ContentBudgetMs * 0.001 * framePerfFreq_.QuadPart);

    // Flush decoded thumbnails once per frame
    if (pipeline_) {
        pipeline_->FlushReadyThumbnails(Theme::MaxBitmapsPerFrame);
    }

    // --- Pass 1: Render content to offscreen bitmap (full viewport, no tab bar clip) ---
    if (offscreenBitmap_) {
        ctx->SetTarget(offscreenBitmap_.Get());
    }
    ctx->Clear(Theme::Background);

    // No clip at tab bar — content renders to full viewport (behind glass)
    if (activeTab_ == GalleryTab::Photos) {
        RenderPhotosTab(renderer, ctx, viewHeight_);
    } else {
        if (folderTransitionActive_) {
            float t = std::clamp(folderSlide_.GetValue(), 0.0f, 1.0f);

            // Suppress synchronous GPU uploads + decode requests during transition:
            // only show already-in-memory thumbnails for both views (same as fast scroll)
            bool savedFastScroll = isFastScrolling_;
            isFastScrolling_ = true;

            D2D1_MATRIX_3X2_F savedTransform;
            ctx->GetTransform(&savedTransform);

            ctx->SetTransform(
                D2D1::Matrix3x2F::Translation(-t * viewWidth_ * 0.3f, 0) * savedTransform);
            RenderAlbumsTab(renderer, ctx, viewHeight_);

            float detailOffset = (1.0f - t) * viewWidth_;
            ctx->SetTransform(
                D2D1::Matrix3x2F::Translation(detailOffset, 0) * savedTransform);
            if (bgBrush_) {
                ctx->FillRectangle(
                    D2D1::RectF(0, 0, viewWidth_, viewHeight_), bgBrush_.Get());
            }
            RenderFolderDetail(renderer, ctx, viewHeight_);

            if (scrollIndicatorBrush_) {
                ctx->FillRectangle(
                    D2D1::RectF(-8.0f, 0, 0, viewHeight_), scrollIndicatorBrush_.Get());
            }

            ctx->SetTransform(savedTransform);
            isFastScrolling_ = savedFastScroll;
        } else if (inFolderDetail_) {
            RenderFolderDetail(renderer, ctx, viewHeight_);
        } else {
            RenderAlbumsTab(renderer, ctx, viewHeight_);
        }
    }

    // --- Pass 2: Compose to swap chain ---
    auto* swapTarget = renderer->GetRenderTarget();
    if (swapTarget) {
        ctx->SetTarget(swapTarget);
    }

    // Blit full content from offscreen bitmap
    if (offscreenBitmap_) {
        ctx->DrawBitmap(offscreenBitmap_.Get());
    }

    // Generate displacement map for compact tab bar pill size
    {
        float margin = Theme::GlassTabBarMargin;
        float maxBarW = 200.0f;
        float barW = std::min(maxBarW, viewWidth_ - margin * 4.0f);
        float barH = Theme::GlassTabBarHeight;
        GenerateDisplacementMap(ctx, barW, barH, Theme::GlassTabBarCornerRadius);
    }

    // Glass tab bar overlay
    if (offscreenBitmap_) {
        RenderGlassTabBar(ctx, offscreenBitmap_.Get());

        // Glass folder header + back button in folder detail
        if (activeTab_ == GalleryTab::Albums && inFolderDetail_ && !folderTransitionActive_) {
            RenderGlassFolderHeader(ctx, offscreenBitmap_.Get());
            RenderGlassBackButton(ctx, offscreenBitmap_.Get());
        }
        // Glass back button for manual open mode (any tab, not in folder detail)
        if (manualOpenMode_ && !inFolderDetail_) {
            RenderGlassBackButton(ctx, offscreenBitmap_.Get());
        }

        // Glass edit button on Albums tab (not in folder detail)
        if (activeTab_ == GalleryTab::Albums && !inFolderDetail_ && !folderTransitionActive_) {
            RenderGlassEditButton(ctx, offscreenBitmap_.Get());
        }
    }
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
    std::vector<std::filesystem::path>* outVisiblePaths,
    LARGE_INTEGER budgetDeadline = {},
    LARGE_INTEGER perfFreq = {})
{
    // Cap thumbnail resolution to keep memory footprint small (160×160×4 = 100KB each)
    // so the cache can hold 10,000+ thumbnails without eviction.
    uint32_t targetPx = std::min(
        static_cast<uint32_t>(grid.cellSize * dpiScale),
        Theme::ThumbnailMaxPx);

    // Prefetch buffer: pre-decode 1.5 screens above and below the viewport
    // so thumbnails are ready before the user scrolls to them.
    float prefetchMargin = contentHeight * Theme::PrefetchScreens;

    // Frame budget: stop rendering content if we've exceeded our time budget,
    // ensuring glass overlays always get rendered (dual-rendering-inspired).
    bool hasBudget = (budgetDeadline.QuadPart > 0 && perfFreq.QuadPart > 0);
    int cellsSinceBudgetCheck = 0;
    bool budgetExhausted = false;

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

            // Frame budget check: periodically test if we've exceeded our
            // content rendering budget. If so, stop drawing more cells so
            // glass overlays (tab bar, back button) always render on time.
            if (hasBudget && onScreen) {
                if (++cellsSinceBudgetCheck >= Theme::BudgetCheckInterval) {
                    cellsSinceBudgetCheck = 0;
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    if (now.QuadPart >= budgetDeadline.QuadPart) {
                        budgetExhausted = true;
                        break;
                    }
                }
            }
        }
        if (budgetExhausted) break;
    }
}

void GalleryView::RenderPhotosTab(Rendering::Direct2DRenderer* renderer,
                                   ID2D1DeviceContext* ctx, float contentHeight)
{
    auto grid = CalculateGridLayout(viewWidth_);
    cachedGrid_ = grid;
    cachedLayoutWidth_ = viewWidth_;

    ComputeSectionLayouts(grid);
    // Add bottom padding so content can scroll fully above the floating glass tab bar
    float glassOverlap = Theme::GlassTabBarHeight + Theme::GlassTabBarMargin * 2;
    maxScroll_ = std::max(0.0f, cachedTotalHeight_ - contentHeight + glassOverlap);

    float scroll = scrollY_.GetValue();

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
        isFastScrolling_, dpiScale, &visiblePaths,
        frameBudgetDeadline_, framePerfFreq_);

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
    float glassOverlap = Theme::GlassTabBarHeight + Theme::GlassTabBarMargin * 2;
    albumsMaxScroll_ = std::max(0.0f, totalHeight - contentHeight + glassOverlap);

    // Check if we have active jiggle (edit mode or animating out)
    bool hasJiggle = !jigglePhases_.empty();
    float badgeScale = editBadgeScale_.GetValue();
    constexpr float PI2 = 6.2831853f;

    // Account for add card in total height calculation
    if (editMode_) {
        size_t totalCards = folderAlbums_.size() + 1;  // +1 for add card
        int numRowsWithAdd = static_cast<int>((totalCards + ag.columns - 1) / ag.columns);
        float totalHeightWithAdd = startY + numRowsWithAdd * (ag.cardTotalHeight + ag.gap) + ag.paddingX;
        albumsMaxScroll_ = std::max(0.0f, totalHeightWithAdd - contentHeight + glassOverlap);
    }

    for (size_t i = 0; i < folderAlbums_.size(); ++i) {
        int col = static_cast<int>(i) % ag.columns;
        int row = static_cast<int>(i) / ag.columns;

        float cardX = ag.paddingX + col * (ag.cardWidth + ag.gap);
        float cardY = startY + row * (ag.cardTotalHeight + ag.gap) - scroll;

        if (cardY + ag.cardTotalHeight < 0.0f) continue;
        if (cardY > contentHeight) break;

        // Card center for transform
        float centerX = cardX + ag.cardWidth * 0.5f;
        float centerY = cardY + ag.cardTotalHeight * 0.5f;

        // Compute scale for deleting card
        float cardScale = (static_cast<int>(i) == deletingCardIndex_) ? deleteCardScale_.GetValue() : 1.0f;
        if (cardScale < 0.01f) continue;  // Skip fully shrunk cards

        // Save transform, apply jiggle + scale
        D2D1_MATRIX_3X2_F savedTransform;
        ctx->GetTransform(&savedTransform);

        if (hasJiggle && i < jigglePhases_.size()) {
            float angle = Theme::JiggleAmplitudeDeg *
                std::sin(PI2 * Theme::JiggleFrequencyHz * editModeTime_ + jigglePhases_[i]);
            auto transform = D2D1::Matrix3x2F::Translation(-centerX, -centerY) *
                D2D1::Matrix3x2F::Scale(cardScale, cardScale) *
                D2D1::Matrix3x2F::Rotation(angle) *
                D2D1::Matrix3x2F::Translation(centerX, centerY) *
                savedTransform;
            ctx->SetTransform(transform);
        } else if (cardScale < 1.0f) {
            auto transform = D2D1::Matrix3x2F::Translation(-centerX, -centerY) *
                D2D1::Matrix3x2F::Scale(cardScale, cardScale) *
                D2D1::Matrix3x2F::Translation(centerX, centerY) *
                savedTransform;
            ctx->SetTransform(transform);
        }

        // Cover image (1:1, rounded)
        D2D1_RECT_F imgRect = D2D1::RectF(cardX, cardY,
                                            cardX + ag.cardWidth, cardY + ag.imageHeight);
        D2D1_ROUNDED_RECT roundedImg = {imgRect, cornerRadius, cornerRadius};

        if (cellBrush_) {
            ctx->FillRoundedRectangle(roundedImg, cellBrush_.Get());
        }

        Microsoft::WRL::ComPtr<ID2D1Bitmap> thumbnail;
        if (pipeline_) {
            if (isFastScrolling_) {
                thumbnail = pipeline_->GetCachedThumbnail(folderAlbums_[i].coverImage);
            } else {
                uint32_t albumTargetPx = std::min(
                    static_cast<uint32_t>(ag.cardWidth * (renderer ? renderer->GetDpiX() / 96.0f : 1.0f)),
                    Theme::ThumbnailMaxPx);
                thumbnail = pipeline_->RequestThumbnail(folderAlbums_[i].coverImage, albumTargetPx);
            }
        }
        if (thumbnail) {
            D2D1_RECT_F srcRect = ComputeCropRect(thumbnail.Get(), ag.cardWidth, ag.imageHeight);
            DrawBitmapRounded(ctx, factory, thumbnail.Get(), imgRect, cornerRadius, &srcRect);
        }

        // Hover (only when not in edit mode)
        if (!editMode_ && hoverBrush_ &&
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

        // Delete badge (all albums in edit mode)
        if (badgeScale > 0.01f) {
            float badgeCx = cardX + Theme::EditBadgeOffset;
            float badgeCy = cardY + Theme::EditBadgeOffset;
            RenderDeleteBadge(ctx, badgeCx, badgeCy, badgeScale);
        }

        // Restore transform
        ctx->SetTransform(savedTransform);
    }

    // Add card at end of grid (edit mode only)
    if (editMode_) {
        size_t addIdx = folderAlbums_.size();
        int addCol = static_cast<int>(addIdx) % ag.columns;
        int addRow = static_cast<int>(addIdx) / ag.columns;
        float addX = ag.paddingX + addCol * (ag.cardWidth + ag.gap);
        float addY = startY + addRow * (ag.cardTotalHeight + ag.gap) - scroll;

        if (addY + ag.cardTotalHeight >= 0.0f && addY <= contentHeight) {
            D2D1_MATRIX_3X2_F savedTransform;
            ctx->GetTransform(&savedTransform);

            if (hasJiggle && addIdx < jigglePhases_.size()) {
                float angle = Theme::JiggleAmplitudeDeg *
                    std::sin(PI2 * Theme::JiggleFrequencyHz * editModeTime_ + jigglePhases_[addIdx]);
                float acx = addX + ag.cardWidth * 0.5f;
                float acy = addY + ag.cardTotalHeight * 0.5f;
                auto transform = D2D1::Matrix3x2F::Translation(-acx, -acy) *
                    D2D1::Matrix3x2F::Rotation(angle) *
                    D2D1::Matrix3x2F::Translation(acx, acy) *
                    savedTransform;
                ctx->SetTransform(transform);
            }

            RenderAddCard(ctx, addX, addY, ag.cardWidth, ag.imageHeight, cornerRadius);
            ctx->SetTransform(savedTransform);
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

    auto grid = CalculateGridLayout(viewWidth_);
    ComputeFolderDetailSectionLayouts(grid);
    float glassOverlap = Theme::GlassTabBarHeight + Theme::GlassTabBarMargin * 2;
    folderDetailMaxScroll_ = std::max(0.0f, folderDetailCachedTotalHeight_ - contentHeight + glassOverlap);

    float scroll = folderDetailScrollY_.GetValue();

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
        isFastScrolling_, dpiScale, &visiblePaths,
        frameBudgetDeadline_, framePerfFreq_);

    // Tell pipeline which paths are visible for prioritization
    if (pipeline_ && !visiblePaths.empty()) {
        pipeline_->SetVisibleRange(visiblePaths);
    }

    // Header text moved to RenderGlassFolderHeader (Pass 2) for glass backing

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

// ======================= GLASS EFFECTS =======================

void GalleryView::EnsureOffscreenBitmap(Rendering::Direct2DRenderer* renderer)
{
    // viewWidth_/viewHeight_ are in DIPs — convert to physical pixels to match swap chain
    float dpiX = renderer->GetDpiX();
    float dpiY = renderer->GetDpiY();
    uint32_t w = static_cast<uint32_t>(viewWidth_ * dpiX / 96.0f);
    uint32_t h = static_cast<uint32_t>(viewHeight_ * dpiY / 96.0f);
    if (w == 0 || h == 0) return;

    if (offscreenBitmap_ && offscreenW_ == w && offscreenH_ == h) return;

    offscreenBitmap_ = renderer->CreateOffscreenBitmap(w, h);
    offscreenW_ = w;
    offscreenH_ = h;
}

void GalleryView::EnsureGlassEffects(ID2D1DeviceContext* ctx)
{
    if (glassBlurEffect_) return;  // Already created

    // Create Gaussian blur effect
    ctx->CreateEffect(CLSID_D2D1GaussianBlur, &glassBlurEffect_);
    if (glassBlurEffect_) {
        glassBlurEffect_->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, Theme::GlassBlurSigma);
        glassBlurEffect_->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
    }

    // Create displacement map effect
    ctx->CreateEffect(CLSID_D2D1DisplacementMap, &glassDisplaceEffect_);
    if (glassDisplaceEffect_) {
        glassDisplaceEffect_->SetValue(D2D1_DISPLACEMENTMAP_PROP_SCALE, Theme::GlassDisplacementScale);
        glassDisplaceEffect_->SetValue(D2D1_DISPLACEMENTMAP_PROP_X_CHANNEL_SELECT, D2D1_CHANNEL_SELECTOR_R);
        glassDisplaceEffect_->SetValue(D2D1_DISPLACEMENTMAP_PROP_Y_CHANNEL_SELECT, D2D1_CHANNEL_SELECTOR_G);
    }

    // Chain: content → displace → blur
    if (glassBlurEffect_ && glassDisplaceEffect_) {
        glassBlurEffect_->SetInputEffect(0, glassDisplaceEffect_.Get());
    }
}

void GalleryView::GenerateDisplacementMap(ID2D1DeviceContext* ctx,
                                           float width, float height, float cornerRadius)
{
    if (width == displacementMapW_ && height == displacementMapH_ && displacementMap_) return;

    uint32_t w = static_cast<uint32_t>(std::ceil(width));
    uint32_t h = static_cast<uint32_t>(std::ceil(height));
    if (w == 0 || h == 0) return;

    // CPU-side pixel buffer (BGRA)
    std::vector<uint8_t> pixels(w * h * 4, 0);

    float bezelWidth = 6.0f;  // Edge zone with refraction
    float n_glass = 1.5f;     // Refractive index for Snell's law

    for (uint32_t py = 0; py < h; ++py) {
        for (uint32_t px = 0; px < w; ++px) {
            uint8_t dx = 128;  // Neutral = no displacement
            uint8_t dy = 128;

            // Distance from nearest edge of the pill
            float fx = static_cast<float>(px);
            float fy = static_cast<float>(py);

            // Pill shape: rounded rectangle distance
            float cx = width * 0.5f;
            float cy = height * 0.5f;
            float hw = width * 0.5f - cornerRadius;
            float hh = height * 0.5f - cornerRadius;

            // Distance from rounded rect edge (approximate)
            float edgeDistX = std::max(0.0f, std::abs(fx - cx) - hw);
            float edgeDistY = std::max(0.0f, std::abs(fy - cy) - hh);
            float cornerDist = std::sqrt(edgeDistX * edgeDistX + edgeDistY * edgeDistY);
            float distFromEdge = cornerRadius - cornerDist;

            // Only apply displacement in the bezel zone
            if (distFromEdge >= 0.0f && distFromEdge < bezelWidth) {
                float t = 1.0f - (distFromEdge / bezelWidth);  // 0=inside, 1=edge
                // Surface normal angle based on position
                float theta = t * 1.2f;  // Max incidence angle ~70°
                float sinRefracted = std::sin(theta) / n_glass;
                sinRefracted = std::clamp(sinRefracted, -1.0f, 1.0f);
                float displacement = (std::sin(theta) - sinRefracted) * 127.0f * t;

                // Direction: displace toward center
                float dirX = 0.0f, dirY = 0.0f;
                if (edgeDistX > 0.01f || edgeDistY > 0.01f) {
                    float len = std::max(0.001f, std::sqrt(edgeDistX * edgeDistX + edgeDistY * edgeDistY));
                    dirX = (fx > cx ? -1.0f : 1.0f) * edgeDistX / len;
                    dirY = (fy > cy ? -1.0f : 1.0f) * edgeDistY / len;
                } else {
                    // Straight edges
                    if (fx < bezelWidth) dirX = 1.0f;
                    else if (fx > width - bezelWidth) dirX = -1.0f;
                    if (fy < bezelWidth) dirY = 1.0f;
                    else if (fy > height - bezelWidth) dirY = -1.0f;
                }

                dx = static_cast<uint8_t>(std::clamp(128.0f + displacement * dirX, 0.0f, 255.0f));
                dy = static_cast<uint8_t>(std::clamp(128.0f + displacement * dirY, 0.0f, 255.0f));
            }

            // BGRA layout: B=dx(unused), G=dy, R=dx, A=255
            size_t offset = (py * w + px) * 4;
            pixels[offset + 0] = 128;  // B (unused)
            pixels[offset + 1] = dy;   // G = Y displacement
            pixels[offset + 2] = dx;   // R = X displacement
            pixels[offset + 3] = 255;  // A
        }
    }

    // Create D2D bitmap from pixel data
    D2D1_BITMAP_PROPERTIES bitmapProps = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    float dpiX, dpiY;
    ctx->GetDpi(&dpiX, &dpiY);
    bitmapProps.dpiX = dpiX;
    bitmapProps.dpiY = dpiY;

    displacementMap_.Reset();
    ctx->CreateBitmap(D2D1::SizeU(w, h), pixels.data(), w * 4, bitmapProps, &displacementMap_);

    displacementMapW_ = width;
    displacementMapH_ = height;
}

void GalleryView::RenderGlassElement(ID2D1DeviceContext* ctx, ID2D1Bitmap* contentBitmap,
                                      const D2D1_ROUNDED_RECT& pill,
                                      ID2D1SolidColorBrush* tintBrush,
                                      ID2D1SolidColorBrush* borderBrush)
{
    if (!ctx || !contentBitmap || !glassBlurEffect_) return;

    auto* factory = static_cast<ID2D1Factory*>(nullptr);
    {
        ComPtr<ID2D1Factory> f;
        ctx->GetFactory(&f);
        factory = f.Get();
        if (!factory) return;
    }

    // Create pill geometry for clipping
    ComPtr<ID2D1RoundedRectangleGeometry> pillGeo;
    {
        ComPtr<ID2D1Factory> f;
        ctx->GetFactory(&f);
        if (!f) return;
        f->CreateRoundedRectangleGeometry(pill, &pillGeo);
    }
    if (!pillGeo) return;

    // Push geometry layer to clip all drawing to the pill
    D2D1_LAYER_PARAMETERS layerParams = D2D1::LayerParameters(
        D2D1::InfiniteRect(), pillGeo.Get());
    ctx->PushLayer(layerParams, nullptr);

    // Set up effect chain input
    if (glassDisplaceEffect_) {
        glassDisplaceEffect_->SetInput(0, contentBitmap);
        if (displacementMap_) {
            glassDisplaceEffect_->SetInput(1, displacementMap_.Get());
        } else {
            // Fallback: no displacement, just pass through
            glassDisplaceEffect_->SetInput(1, contentBitmap);
        }
    } else if (glassBlurEffect_) {
        glassBlurEffect_->SetInput(0, contentBitmap);
    }

    // Draw blurred content (D2D lazy-evaluates within clip region)
    ComPtr<ID2D1Image> output;
    glassBlurEffect_->GetOutput(&output);
    if (output) {
        ctx->DrawImage(output.Get());
    }

    // Dark tint overlay
    if (tintBrush) {
        ctx->FillRoundedRectangle(pill, tintBrush);
    }

    // Specular border
    if (borderBrush) {
        ctx->DrawRoundedRectangle(pill, borderBrush, 1.0f);
    }

    // Top-edge highlight (specular glint)
    if (glassHighlightBrush_) {
        float left = pill.rect.left + pill.radiusX;
        float right = pill.rect.right - pill.radiusX;
        float top = pill.rect.top + 0.5f;
        ctx->DrawLine(D2D1::Point2F(left, top), D2D1::Point2F(right, top),
                      glassHighlightBrush_.Get(), 0.5f);
    }

    ctx->PopLayer();
}

void GalleryView::RenderGlassTabBar(ID2D1DeviceContext* ctx, ID2D1Bitmap* contentBitmap)
{
    float margin = Theme::GlassTabBarMargin;
    float barH = Theme::GlassTabBarHeight;
    float barR = Theme::GlassTabBarCornerRadius;

    // Compact centered pill (iOS 26 style) — not edge-to-edge
    float maxBarW = 200.0f;
    float barW = std::min(maxBarW, viewWidth_ - margin * 4.0f);
    float barLeft = (viewWidth_ - barW) / 2.0f;
    float barRight = barLeft + barW;
    float barTop = viewHeight_ - barH - margin;
    float barBottom = viewHeight_ - margin;

    D2D1_ROUNDED_RECT barPill = {
        D2D1::RectF(barLeft, barTop, barRight, barBottom),
        barR, barR
    };

    // Render the main glass bar
    RenderGlassElement(ctx, contentBitmap, barPill, glassTintBrush_.Get(), glassBorderBrush_.Get());

    // Active tab indicator pill (glass-within-glass)
    float halfWidth = barW / 2.0f;
    float tabT = std::clamp(tabSlide_.GetValue(), 0.0f, 1.0f);
    float pillPadding = 4.0f;
    float pillH = barH - pillPadding * 2.0f;
    float pillR = pillH / 2.0f;

    float activePillLeft0 = barLeft + pillPadding;
    float activePillLeft1 = barLeft + halfWidth + pillPadding;
    float activePillW = halfWidth - pillPadding * 2.0f;

    float activePillX = activePillLeft0 + tabT * (activePillLeft1 - activePillLeft0);

    D2D1_ROUNDED_RECT activePill = {
        D2D1::RectF(activePillX, barTop + pillPadding,
                     activePillX + activePillW, barTop + pillPadding + pillH),
        pillR, pillR
    };

    // Active indicator: lighter glass fill
    {
        ComPtr<ID2D1RoundedRectangleGeometry> activePillGeo;
        ComPtr<ID2D1Factory> f;
        ctx->GetFactory(&f);
        if (f) f->CreateRoundedRectangleGeometry(activePill, &activePillGeo);
        if (activePillGeo) {
            D2D1_LAYER_PARAMETERS layerParams = D2D1::LayerParameters(
                D2D1::InfiniteRect(), activePillGeo.Get());
            ctx->PushLayer(layerParams, nullptr);

            // Draw blurred content through active pill
            if (glassBlurEffect_) {
                ComPtr<ID2D1Image> output;
                glassBlurEffect_->GetOutput(&output);
                if (output) ctx->DrawImage(output.Get());
            }

            if (glassActivePillBrush_)
                ctx->FillRoundedRectangle(activePill, glassActivePillBrush_.Get());
            if (glassActivePillBorderBrush_)
                ctx->DrawRoundedRectangle(activePill, glassActivePillBorderBrush_.Get(), 1.0f);

            ctx->PopLayer();
        }
    }

    // Draw tab labels
    struct TabInfo { const wchar_t* label; size_t labelLen; GalleryTab tab; };
    TabInfo tabs[] = {
        {L"\u7167\u7247", 2, GalleryTab::Photos},
        {L"\u76F8\u518C", 2, GalleryTab::Albums},
    };

    for (int t = 0; t < 2; ++t) {
        float tabLeft = barLeft + t * halfWidth;
        float tabRight = tabLeft + halfWidth;
        bool isActive = (activeTab_ == tabs[t].tab);

        D2D1_RECT_F tabRect = D2D1::RectF(tabLeft, barTop, tabRight, barBottom);

        if (tabFormat_) {
            auto* brush = isActive ? glassTabTextBrush_.Get() : glassTabTextInactiveBrush_.Get();
            if (brush) {
                ctx->DrawText(tabs[t].label, static_cast<UINT32>(tabs[t].labelLen),
                              tabFormat_.Get(), tabRect, brush);
            }
        }
    }
}

void GalleryView::RenderGlassBackButton(ID2D1DeviceContext* ctx, ID2D1Bitmap* contentBitmap)
{
    if (!backButtonFormat_ || !dwFactory_) return;

    // Dynamic text: folder detail → "‹ 相册", manual open → "‹ 照片"
    const wchar_t* text = (manualOpenMode_ && !inFolderDetail_) ? L"\u2039 \u7167\u7247" : L"\u2039 \u76F8\u518C";
    uint32_t textLen = 4;
    float maxW = 200.0f;
    float btnH = Theme::GlassBackBtnHeight;

    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = dwFactory_->CreateTextLayout(text, textLen, backButtonFormat_.Get(),
                                               maxW, btnH, &layout);
    if (FAILED(hr) || !layout) return;

    DWRITE_TEXT_METRICS metrics;
    layout->GetMetrics(&metrics);

    float btnW = metrics.width + Theme::GlassBackBtnPadding * 2.0f;
    float btnR = btnH / 2.0f;
    float btnX = Theme::GlassTabBarMargin;
    float btnY = Theme::GlassTabBarMargin;

    D2D1_ROUNDED_RECT btnPill = {
        D2D1::RectF(btnX, btnY, btnX + btnW, btnY + btnH),
        btnR, btnR
    };

    RenderGlassElement(ctx, contentBitmap, btnPill, glassTintBrush_.Get(), glassBorderBrush_.Get());

    // Draw text centered in pill
    if (glassTabTextBrush_) {
        D2D1_RECT_F textRect = D2D1::RectF(
            btnX + Theme::GlassBackBtnPadding, btnY,
            btnX + btnW - Theme::GlassBackBtnPadding, btnY + btnH);
        ctx->DrawText(text, textLen, backButtonFormat_.Get(), textRect, glassTabTextBrush_.Get());
    }
}

void GalleryView::RenderGlassFolderHeader(ID2D1DeviceContext* ctx, ID2D1Bitmap* contentBitmap)
{
    if (!dwFactory_ || openFolderIndex_ >= folderAlbums_.size()) return;
    const auto& album = folderAlbums_[openFolderIndex_];

    float titleY = Theme::GlassTabBarMargin + Theme::GlassBackBtnHeight + 8.0f;
    float headerBottom = titleY + 58.0f;

    // Full-width glass header bar (covers title + subtitle zone)
    D2D1_ROUNDED_RECT headerBar = {
        D2D1::RectF(0, 0, viewWidth_, headerBottom),
        0.0f, 0.0f
    };
    RenderGlassElement(ctx, contentBitmap, headerBar, glassTintBrush_.Get(), nullptr);

    // Subtle bottom separator
    if (glassBorderBrush_) {
        ctx->DrawLine(
            D2D1::Point2F(0, headerBottom),
            D2D1::Point2F(viewWidth_, headerBottom),
            glassBorderBrush_.Get(), 0.5f);
    }

    // Folder title — large bold
    if (textBrush_ && titleFormat_) {
        float titleH = 36.0f;
        float titleMaxW = viewWidth_ - Theme::GalleryPadding * 2;
        if (titleMaxW > 0) {
            ComPtr<IDWriteTextLayout> layout;
            HRESULT hr = dwFactory_->CreateTextLayout(
                album.displayName.c_str(),
                static_cast<UINT32>(album.displayName.size()),
                titleFormat_.Get(),
                titleMaxW, titleH, &layout);
            if (SUCCEEDED(hr) && layout) {
                layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                ComPtr<IDWriteInlineObject> ellipsis;
                dwFactory_->CreateEllipsisTrimmingSign(titleFormat_.Get(), &ellipsis);
                DWRITE_TRIMMING trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                layout->SetTrimming(&trimming, ellipsis.Get());
                ctx->DrawTextLayout(
                    D2D1::Point2F(Theme::GalleryPadding, titleY),
                    layout.Get(), textBrush_.Get());
            }
        }
    }

    // Subtitle: photo count
    if (countFormat_ && secondaryBrush_) {
        D2D1_RECT_F subtitleRect = D2D1::RectF(
            Theme::GalleryPadding, titleY + 38.0f,
            viewWidth_ - Theme::GalleryPadding, titleY + 54.0f);
        std::wstring sub = FormatNumber(folderDetailImages_.size()) + L" photos";
        ctx->DrawText(sub.c_str(), static_cast<UINT32>(sub.size()),
                      countFormat_.Get(), subtitleRect, secondaryBrush_.Get());
    }
}

void GalleryView::RenderGlassEditButton(ID2D1DeviceContext* ctx, ID2D1Bitmap* contentBitmap)
{
    if (!editButtonFormat_ || !dwFactory_) return;

    const wchar_t* text = editMode_ ? L"\u5B8C\u6210" : L"\u7F16\u8F91";
    uint32_t textLen = 2;
    float maxW = 200.0f;
    float btnH = Theme::GlassBackBtnHeight;

    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = dwFactory_->CreateTextLayout(text, textLen, editButtonFormat_.Get(),
                                               maxW, btnH, &layout);
    if (FAILED(hr) || !layout) return;

    DWRITE_TEXT_METRICS metrics;
    layout->GetMetrics(&metrics);

    float btnW = metrics.width + Theme::GlassBackBtnPadding * 2.0f;
    float btnR = btnH / 2.0f;
    float btnX = viewWidth_ - Theme::GlassTabBarMargin - btnW;
    float btnY = Theme::GlassTabBarMargin;

    D2D1_ROUNDED_RECT btnPill = {
        D2D1::RectF(btnX, btnY, btnX + btnW, btnY + btnH),
        btnR, btnR
    };

    RenderGlassElement(ctx, contentBitmap, btnPill, glassTintBrush_.Get(), glassBorderBrush_.Get());

    if (glassTabTextBrush_) {
        D2D1_RECT_F textRect = D2D1::RectF(btnX, btnY, btnX + btnW, btnY + btnH);
        ctx->DrawText(text, textLen, editButtonFormat_.Get(), textRect, glassTabTextBrush_.Get());
    }
}

void GalleryView::RenderDeleteBadge(ID2D1DeviceContext* ctx, float cx, float cy, float scale)
{
    if (!editBadgeBrush_ || !editBadgeIconBrush_ || scale < 0.01f) return;

    float r = Theme::EditBadgeRadius * scale;

    // Red circle
    D2D1_ELLIPSE badge = {D2D1::Point2F(cx, cy), r, r};
    ctx->FillEllipse(badge, editBadgeBrush_.Get());

    // White minus sign
    float lineHalf = r * 0.5f;
    float lineThickness = 2.0f * scale;
    ctx->DrawLine(
        D2D1::Point2F(cx - lineHalf, cy),
        D2D1::Point2F(cx + lineHalf, cy),
        editBadgeIconBrush_.Get(), lineThickness);
}

void GalleryView::RenderAddCard(ID2D1DeviceContext* ctx, float x, float y,
                                 float w, float h, float cornerRadius)
{
    // Dashed border rounded rect
    D2D1_RECT_F rect = D2D1::RectF(x, y, x + w, y + h);
    D2D1_ROUNDED_RECT rr = {rect, cornerRadius, cornerRadius};

    if (addCardBorderBrush_) {
        ctx->DrawRoundedRectangle(rr, addCardBorderBrush_.Get(), 2.0f);
    }

    // Plus sign in center
    if (addCardIconBrush_) {
        float cx = x + w * 0.5f;
        float cy = y + h * 0.5f;
        float arm = 20.0f;
        float thickness = 3.0f;

        ctx->DrawLine(D2D1::Point2F(cx - arm, cy), D2D1::Point2F(cx + arm, cy),
                      addCardIconBrush_.Get(), thickness);
        ctx->DrawLine(D2D1::Point2F(cx, cy - arm), D2D1::Point2F(cx, cy + arm),
                      addCardIconBrush_.Get(), thickness);
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
    editBadgeScale_.Update(deltaTime);
    deleteCardScale_.Update(deltaTime);

    // Edit mode time accumulator
    if (editMode_ || !editBadgeScale_.IsFinished()) {
        editModeTime_ += deltaTime;
    }

    // Delete card shrink completion
    if (deletingCardIndex_ >= 0 && deleteCardScale_.IsFinished() &&
        deleteCardScale_.GetValue() < 0.05f) {
        size_t idx = static_cast<size_t>(deletingCardIndex_);
        deletingCardIndex_ = -1;
        if (idx < folderAlbums_.size() && deleteAlbumCallback_) {
            deleteAlbumCallback_(folderAlbums_[idx].folderPath);
        }
    }

    // Exiting edit mode: when badge scale finishes at ~0, clean up
    if (!editMode_ && editBadgeScale_.IsFinished() &&
        editBadgeScale_.GetValue() < 0.05f && !jigglePhases_.empty()) {
        jigglePhases_.clear();
    }

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
        // Glass tab bar hit test (compact centered pill at bottom)
        float margin = Theme::GlassTabBarMargin;
        float barH = Theme::GlassTabBarHeight;
        float maxBarW = 200.0f;
        float barW = std::min(maxBarW, viewWidth_ - margin * 4.0f);
        float barLeft = (viewWidth_ - barW) / 2.0f;
        float barRight = barLeft + barW;
        float barTop = viewHeight_ - barH - margin;
        float barBottom = viewHeight_ - margin;

        if (y >= barTop && y <= barBottom && x >= barLeft && x <= barRight) {
            float halfWidth = barW / 2.0f;
            float relX = x - barLeft;
            if (relX < halfWidth) {
                if (editMode_) SetEditMode(false);
                activeTab_ = GalleryTab::Photos;
                tabSlide_.SetTarget(0.0f);
                if (inFolderDetail_) {
                    inFolderDetail_ = false;
                    folderTransitionActive_ = false;
                    folderDetailImages_.clear();
                    folderDetailSections_.clear();
                }
            } else {
                if (editMode_ && activeTab_ != GalleryTab::Albums) SetEditMode(false);
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

        // Glass back button in folder detail
        if (activeTab_ == GalleryTab::Albums && inFolderDetail_) {
            float btnX = Theme::GlassTabBarMargin;
            float btnY = Theme::GlassTabBarMargin;
            float btnW = 100.0f;  // Generous hit area
            float btnH = Theme::GlassBackBtnHeight;
            if (x >= btnX && x <= btnX + btnW && y >= btnY && y <= btnY + btnH) {
                ExitFolderDetail();
                consumedClick_ = true;
                return;
            }
        }

        // Glass back button for manual open mode (return to library)
        if (manualOpenMode_ && !inFolderDetail_) {
            float btnX = Theme::GlassTabBarMargin;
            float btnY = Theme::GlassTabBarMargin;
            float btnW = 100.0f;  // Generous hit area
            float btnH = Theme::GlassBackBtnHeight;
            if (x >= btnX && x <= btnX + btnW && y >= btnY && y <= btnY + btnH) {
                if (backToLibraryCallback_) backToLibraryCallback_();
                consumedClick_ = true;
                return;
            }
        }

        // Glass edit button hit test (top-right pill, Albums tab only)
        if (activeTab_ == GalleryTab::Albums && !inFolderDetail_) {
            float ebtnH = Theme::GlassBackBtnHeight;
            float ebtnW = 80.0f;  // Generous hit area
            float ebtnX = viewWidth_ - Theme::GlassTabBarMargin - ebtnW;
            float ebtnY = Theme::GlassTabBarMargin;
            if (x >= ebtnX && x <= ebtnX + ebtnW && y >= ebtnY && y <= ebtnY + ebtnH) {
                SetEditMode(!editMode_);
                consumedClick_ = true;
                return;
            }
        }

        // Album card click / edit mode interactions
        if (activeTab_ == GalleryTab::Albums && !inFolderDetail_) {
            auto ag = CalculateAlbumGridLayout(viewWidth_);
            float scroll = albumsScrollY_.GetValue();
            float startY = Theme::GalleryHeaderHeight + Theme::GalleryPadding;
            float worldY = y + scroll;

            if (editMode_) {
                // Check add card hit
                size_t addIdx = folderAlbums_.size();
                int addCol = static_cast<int>(addIdx) % ag.columns;
                int addRow = static_cast<int>(addIdx) / ag.columns;
                float addCardX = ag.paddingX + addCol * (ag.cardWidth + ag.gap);
                float addCardYPos = startY + addRow * (ag.cardTotalHeight + ag.gap);

                if (x >= addCardX && x <= addCardX + ag.cardWidth &&
                    worldY >= addCardYPos && worldY <= addCardYPos + ag.imageHeight) {
                    if (addAlbumCallback_) addAlbumCallback_();
                    consumedClick_ = true;
                    return;
                }

                // Check delete badge hit on album cards
                for (size_t i = 0; i < folderAlbums_.size(); ++i) {
                    int col = static_cast<int>(i) % ag.columns;
                    int row = static_cast<int>(i) / ag.columns;
                    float cardX = ag.paddingX + col * (ag.cardWidth + ag.gap);
                    float cardYPos = startY + row * (ag.cardTotalHeight + ag.gap);

                    // Badge position (top-left of card)
                    float badgeCx = cardX + Theme::EditBadgeOffset;
                    float badgeCy = cardYPos - scroll + Theme::EditBadgeOffset;
                    float hitRadius = Theme::EditBadgeRadius + 8.0f;  // Generous hit area

                    float dx = x - badgeCx;
                    float dy = y - badgeCy;
                    if (dx * dx + dy * dy <= hitRadius * hitRadius) {
                        // Start delete animation
                        deletingCardIndex_ = static_cast<int>(i);
                        deleteCardScale_.SetValue(1.0f);
                        deleteCardScale_.SetTarget(0.0f);
                        consumedClick_ = true;
                        return;
                    }
                }

                // In edit mode, block folder detail opens
                consumedClick_ = true;
                return;
            }

            // Normal mode: enter folder detail
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
    // Reject clicks in the glass tab bar region
    float glassBarTop = viewHeight_ - Theme::GlassTabBarHeight - Theme::GlassTabBarMargin;
    if (y >= glassBarTop) return std::nullopt;
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
