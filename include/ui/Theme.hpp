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

    // Tab bar
    constexpr float TabBarHeight = 56.0f;
    constexpr D2D1_COLOR_F TabBarBg = {0.08f, 0.08f, 0.09f, 0.97f};

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

} // namespace Theme
} // namespace UI
} // namespace UltraImageViewer
