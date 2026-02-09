#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <vector>
#include <filesystem>
#include <optional>
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

    // Current tab
    GalleryTab GetActiveTab() const { return activeTab_; }
    bool IsInFolderDetail() const { return inFolderDetail_; }

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
    void RenderTabBar(ID2D1DeviceContext* ctx);

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
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> tabBarBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> tabBarGradientBrush_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> tabFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> albumTitleFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> albumCountFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> backButtonFormat_;

    // Mouse hover
    float hoverX_ = -1.0f;
    float hoverY_ = -1.0f;

    // Skip rendering this cell index (image is "lifted" into viewer)
    std::optional<size_t> skipIndex_;

    bool resourcesCreated_ = false;
    void EnsureResources(Rendering::Direct2DRenderer* renderer);
};

} // namespace UI
} // namespace UltraImageViewer
