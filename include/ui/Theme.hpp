#pragma once

#include <d2d1.h>

namespace UltraImageViewer {
namespace UI {
namespace Theme {

    // Background
    constexpr D2D1_COLOR_F Background     = {0.06f, 0.06f, 0.06f, 1.0f};
    constexpr D2D1_COLOR_F Surface        = {0.13f, 0.13f, 0.14f, 1.0f};
    constexpr D2D1_COLOR_F SurfaceHover   = {0.20f, 0.20f, 0.22f, 1.0f};

    // Text
    constexpr D2D1_COLOR_F TextPrimary    = {0.95f, 0.95f, 0.95f, 1.0f};
    constexpr D2D1_COLOR_F TextSecondary  = {0.52f, 0.52f, 0.55f, 1.0f};

    // Accent
    constexpr D2D1_COLOR_F Accent         = {0.04f, 0.52f, 1.0f, 1.0f};

    // Viewer
    constexpr D2D1_COLOR_F ViewerBg       = {0.0f, 0.0f, 0.0f, 1.0f};
    constexpr D2D1_COLOR_F ViewerOverlay  = {0.0f, 0.0f, 0.0f, 0.5f};

    // Dimensions
    constexpr float ThumbnailGap = 6.0f;
    constexpr float ThumbnailCornerRadius = 8.0f;
    constexpr float TransitionCornerRadius = 8.0f;

    // Tab bar (legacy solid — kept for scroll math compatibility)
    constexpr float TabBarHeight = 56.0f;
    constexpr D2D1_COLOR_F TabBarBg = {0.08f, 0.08f, 0.09f, 0.97f};

    // Glass design (iOS 26 Liquid Glass)
    constexpr float GlassBlurSigma = 20.0f;
    constexpr float GlassTabBarHeight = 48.0f;
    constexpr float GlassTabBarMargin = 16.0f;
    constexpr float GlassTabBarCornerRadius = 24.0f;  // = height/2, full pill
    constexpr D2D1_COLOR_F GlassTintColor = {0.098f, 0.098f, 0.11f, 0.55f};
    constexpr D2D1_COLOR_F GlassBorderColor = {1.0f, 1.0f, 1.0f, 0.12f};
    constexpr D2D1_COLOR_F GlassHighlightColor = {1.0f, 1.0f, 1.0f, 0.20f};
    constexpr D2D1_COLOR_F GlassActivePillColor = {1.0f, 1.0f, 1.0f, 0.10f};
    constexpr D2D1_COLOR_F GlassActivePillBorder = {1.0f, 1.0f, 1.0f, 0.15f};
    constexpr D2D1_COLOR_F GlassTabTextActive = {1.0f, 1.0f, 1.0f, 0.95f};
    constexpr D2D1_COLOR_F GlassTabTextInactive = {1.0f, 1.0f, 1.0f, 0.55f};
    constexpr float GlassDisplacementScale = 10.0f;
    constexpr float GlassBackBtnHeight = 32.0f;
    constexpr float GlassBackBtnPadding = 12.0f;

    // Gallery
    constexpr float MinCellSize = 200.0f;
    constexpr float MaxCellSize = 400.0f;
    constexpr float GalleryPadding = 24.0f;
    constexpr float GalleryHeaderHeight = 100.0f;
    constexpr float SectionHeaderHeight = 48.0f;
    constexpr float SectionGap = 24.0f;

    // Album cards (responsive grid, same idea as photo thumbnails)
    constexpr float AlbumCardGap = 16.0f;
    constexpr float AlbumCornerRadius = 12.0f;
    constexpr float AlbumMinCardWidth = 160.0f;
    constexpr float AlbumMaxCardWidth = 220.0f;
    constexpr float AlbumTextHeight = 48.0f;

    // Animation configs (stiffness, damping, mass, restThreshold)
    // Snappy for transitions
    constexpr float TransitionStiffness = 300.0f;
    constexpr float TransitionDamping = 25.0f;
    // Smooth for scrolling
    constexpr float ScrollStiffness = 150.0f;
    constexpr float ScrollDamping = 22.0f;
    // Bouncy for rubber band
    constexpr float RubberBandStiffness = 400.0f;
    constexpr float RubberBandDamping = 30.0f;
    // Navigation push/pop (album enter/exit) — critically damped for snappy feel
    constexpr float NavigationStiffness = 500.0f;
    constexpr float NavigationDamping = 40.0f;

    // Performance tuning
    constexpr float FastScrollThreshold = 2000.0f;      // px/sec scroll velocity to trigger fast-scroll mode
    constexpr int MaxBitmapsPerFrame = 64;               // max GPU uploads (D2D bitmap creation) per frame
    constexpr int PersistSyncBudgetPerFrame = 200;       // max synchronous disk→GPU loads per frame
    constexpr int ThumbnailWorkerThreads = 4;            // background decode threads
    constexpr size_t ThumbnailCacheMaxBytes = 1024ULL * 1024 * 1024;  // 1GB LRU eviction threshold
    constexpr uint32_t ThumbnailMaxPx = 160;                         // max thumbnail decode resolution (px)
    constexpr float PrefetchScreens = 3.0f;              // prefetch N screens above/below viewport

} // namespace Theme
} // namespace UI
} // namespace UltraImageViewer
