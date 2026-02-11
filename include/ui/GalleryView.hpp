#pragma once

#include <d2d1_1.h>
#include <d2d1effects.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <vector>
#include <filesystem>
#include <optional>
#include <functional>
#include "../animation/SpringAnimation.hpp"
#include "../animation/AnimationEngine.hpp"
#include "../rendering/Direct2DRenderer.hpp"
#include "../core/ImagePipeline.hpp"

namespace UltraImageViewer {
namespace UI {

enum class GalleryTab { Photos, Albums };

struct FolderAlbum {
    std::filesystem::path folderPath;
    std::wstring displayName;
    size_t imageCount = 0;
    std::filesystem::path coverImage;  // First image used as cover
};

class GalleryView {
public:
    GalleryView();
    ~GalleryView();

    void Initialize(Rendering::Direct2DRenderer* renderer,
                    Core::ImagePipeline* pipeline,
                    Animation::AnimationEngine* engine);

    // Set images grouped by date (phone gallery style)
    void SetImagesGrouped(const std::vector<Core::ScannedImage>& scannedImages);

    // Set flat image list (for Ctrl+O / drag-drop / command-line)
    void SetImages(const std::vector<std::filesystem::path>& paths);

    const std::vector<std::filesystem::path>& GetImages() const { return images_; }

    // Get the currently active image list (Photos tab: all, FolderDetail: filtered)
    const std::vector<std::filesystem::path>& GetActiveImages() const;

    // Scanning state display
    void SetScanningState(bool isScanning, size_t count);

    void Render(Rendering::Direct2DRenderer* renderer);
    void Update(float deltaTime);

    // Interaction
    void OnMouseWheel(float delta);
    void OnMouseDown(float x, float y);
    void OnMouseMove(float x, float y);
    void OnMouseUp(float x, float y);
    bool WasDragging() const { return hasDragged_; }
    bool ConsumedClick() const { return consumedClick_; }

    // Hit testing — returns index into GetActiveImages()
    struct HitResult {
        size_t index;
        D2D1_RECT_F rect;
    };
    std::optional<HitResult> HitTest(float x, float y) const;

    // Layout info
    void SetViewSize(float width, float height);

    // Get the rect for a given image index (for hero transition)
    std::optional<D2D1_RECT_F> GetCellScreenRect(size_t index) const;

    float GetScrollY() const { return scrollY_.GetValue(); }

    // Skip rendering a specific cell (for viewer: image is "lifted" from gallery)
    void SetSkipIndex(std::optional<size_t> index) { skipIndex_ = index; }

    // Manual open mode (Ctrl+O / drag-drop)
    void SetManualOpenMode(bool enabled);
    void SetBackToLibraryCallback(std::function<void()> cb);

    // Current tab
    GalleryTab GetActiveTab() const { return activeTab_; }
    bool IsInFolderDetail() const { return inFolderDetail_; }

    // Edit mode (iOS jiggle management)
    bool IsInEditMode() const { return editMode_; }
    void SetEditMode(bool enabled);
    void SetDeleteAlbumCallback(std::function<void(const std::filesystem::path&)> cb);
    void SetAddAlbumCallback(std::function<void()> cb);
    void SetFolderVisitCallback(std::function<void(const std::filesystem::path&)> cb);

    // Public types needed by rendering helpers
    struct Section {
        std::wstring title;
        size_t startIndex = 0;
        size_t count = 0;
    };

    struct GridLayout {
        int columns;
        float cellSize;
        float gap;
        float paddingX;
    };

    struct AlbumGridLayout {
        int columns;
        float cardWidth;
        float gap;
        float paddingX;
        float imageHeight;      // = cardWidth (1:1)
        float cardTotalHeight;  // imageHeight + textArea
    };

    struct SectionLayoutInfo {
        float headerY;    // Y position of section header (world space)
        float contentY;   // Y position of first cell (world space)
        int rows;          // Number of rows in this section
    };

    static std::wstring FormatNumber(size_t n);

private:

    GridLayout CalculateGridLayout(float viewWidth) const;
    AlbumGridLayout CalculateAlbumGridLayout(float viewWidth) const;
    void ComputeSectionLayouts(const GridLayout& grid) const;

    // Rendering sub-methods
    void RenderPhotosTab(Rendering::Direct2DRenderer* renderer, ID2D1DeviceContext* ctx,
                         float contentHeight);
    void RenderAlbumsTab(Rendering::Direct2DRenderer* renderer, ID2D1DeviceContext* ctx,
                         float contentHeight);
    void RenderFolderDetail(Rendering::Direct2DRenderer* renderer, ID2D1DeviceContext* ctx,
                            float contentHeight);

    // Glass rendering
    void RenderGlassElement(ID2D1DeviceContext* ctx, ID2D1Bitmap* contentBitmap,
                            const D2D1_ROUNDED_RECT& pill,
                            ID2D1SolidColorBrush* tintBrush,
                            ID2D1SolidColorBrush* borderBrush);
    void RenderGlassTabBar(ID2D1DeviceContext* ctx, ID2D1Bitmap* contentBitmap);
    void RenderGlassBackButton(ID2D1DeviceContext* ctx, ID2D1Bitmap* contentBitmap);
    void RenderGlassFolderHeader(ID2D1DeviceContext* ctx, ID2D1Bitmap* contentBitmap);
    void EnsureOffscreenBitmap(Rendering::Direct2DRenderer* renderer);
    void EnsureGlassEffects(ID2D1DeviceContext* ctx);
    void GenerateDisplacementMap(ID2D1DeviceContext* ctx, float width, float height, float cornerRadius);

    // Albums helpers
    void BuildFolderAlbums(const std::vector<Core::ScannedImage>& scannedImages);
    void EnterFolderDetail(size_t albumIndex);
    void ExitFolderDetail();

    // Folder detail section layout helpers
    void ComputeFolderDetailSectionLayouts(const GridLayout& grid) const;

    // Tab state
    GalleryTab activeTab_ = GalleryTab::Photos;

    // Photos tab scrolling
    Animation::SpringAnimation scrollY_;
    float scrollVelocity_ = 0.0f;
    bool isDragging_ = false;
    float dragStartY_ = 0.0f;
    float dragStartScroll_ = 0.0f;
    float dragStartX_ = 0.0f;
    bool hasDragged_ = false;
    bool consumedClick_ = false;  // OnMouseUp handled tab/album/back click
    float lastDragY_ = 0.0f;

    // View size
    float viewWidth_ = 1280.0f;
    float viewHeight_ = 720.0f;

    // Max scroll (photos tab)
    float maxScroll_ = 0.0f;

    // Data
    std::vector<std::filesystem::path> images_;   // Flat list of all image paths
    std::vector<Section> sections_;                // Grouped sections

    // Folder albums data
    std::vector<FolderAlbum> folderAlbums_;
    std::vector<Core::ScannedImage> allScannedImages_;  // Keep for folder detail filtering

    // Albums tab scrolling
    Animation::SpringAnimation albumsScrollY_;
    float albumsMaxScroll_ = 0.0f;

    // Folder detail mode
    bool inFolderDetail_ = false;
    size_t openFolderIndex_ = 0;
    std::vector<std::filesystem::path> folderDetailImages_;
    std::vector<Section> folderDetailSections_;
    Animation::SpringAnimation folderDetailScrollY_;
    float folderDetailMaxScroll_ = 0.0f;
    mutable std::vector<SectionLayoutInfo> folderDetailSectionLayouts_;
    mutable float folderDetailCachedTotalHeight_ = 0.0f;

    // Folder detail navigation transition
    Animation::SpringAnimation folderSlide_;  // 0=albums grid, 1=folder detail
    bool folderTransitionActive_ = false;
    bool folderTransitionForward_ = true;

    // Tab indicator animation
    Animation::SpringAnimation tabSlide_;  // 0=Photos, 1=Albums

    Core::ImagePipeline* pipeline_ = nullptr;
    Animation::AnimationEngine* engine_ = nullptr;

    // Section layouts (cached)
    mutable std::vector<SectionLayoutInfo> sectionLayouts_;
    mutable float cachedTotalHeight_ = 0.0f;
    mutable float cachedLayoutWidth_ = 0.0f;
    mutable GridLayout cachedGrid_ = {};

    // Scanning state
    bool isScanning_ = false;
    size_t scanCount_ = 0;
    float scanBarPhase_ = 0.0f;  // Sinusoidal sweep phase [0, 1)

    // Rendering resources
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> cellBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> secondaryBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accentBrush_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> sectionFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> countFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> countRightFormat_;  // Right-aligned count
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> scrollIndicatorBrush_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> tabFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> albumTitleFormat_;

    // Glass effect resources
    Microsoft::WRL::ComPtr<ID2D1Effect> glassBlurEffect_;
    Microsoft::WRL::ComPtr<ID2D1Effect> glassDisplaceEffect_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> offscreenBitmap_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> displacementMap_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> glassTintBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> glassBorderBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> glassHighlightBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> glassActivePillBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> glassActivePillBorderBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> glassTabTextBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> glassTabTextInactiveBrush_;
    uint32_t offscreenW_ = 0, offscreenH_ = 0;
    float displacementMapW_ = 0.0f, displacementMapH_ = 0.0f;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> albumCountFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> backButtonFormat_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwFactory_;  // Cached for per-frame text layout

    // Manual open mode (Ctrl+O / drag-drop replaces gallery)
    bool manualOpenMode_ = false;
    std::function<void()> backToLibraryCallback_;

    // Fast-scroll detection
    float scrollVelocitySmoothed_ = 0.0f;
    bool isFastScrolling_ = false;

    // Mouse hover
    float hoverX_ = -1.0f;
    float hoverY_ = -1.0f;

    // Skip rendering this cell index (image is "lifted" into viewer)
    std::optional<size_t> skipIndex_;

    // Edit mode (iOS jiggle management)
    bool editMode_ = false;
    float editModeTime_ = 0.0f;
    std::vector<float> jigglePhases_;
    Animation::SpringAnimation editBadgeScale_;  // 0↔1 for badge pop-in/out
    int deletingCardIndex_ = -1;
    Animation::SpringAnimation deleteCardScale_; // 1→0 for card shrink
    std::function<void(const std::filesystem::path&)> deleteAlbumCallback_;
    std::function<void()> addAlbumCallback_;
    std::function<void(const std::filesystem::path&)> folderVisitCallback_;

    void RenderGlassEditButton(ID2D1DeviceContext* ctx, ID2D1Bitmap* contentBitmap);
    void RenderDeleteBadge(ID2D1DeviceContext* ctx, float cx, float cy, float scale);
    void RenderAddCard(ID2D1DeviceContext* ctx, float x, float y, float w, float h, float cornerRadius);

    // Edit mode D2D resources
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> editBadgeBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> editBadgeIconBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> addCardBorderBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> addCardIconBrush_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> editButtonFormat_;

    // Frame budget (dual-rendering-inspired: content vs glass overlay split)
    LARGE_INTEGER frameStart_ = {};
    LARGE_INTEGER frameBudgetDeadline_ = {};
    LARGE_INTEGER framePerfFreq_ = {};

    bool resourcesCreated_ = false;
    void EnsureResources(Rendering::Direct2DRenderer* renderer);
};

} // namespace UI
} // namespace UltraImageViewer
