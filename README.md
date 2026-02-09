<p align="center">
  <img src="assets/icon_preview.jpg" width="128" style="border-radius: 24px;" />
</p>

<h1 align="center">拾光 Afterglow</h1>

<p align="center">
  <em>在时间的碎片里，拾起那些被光照亮的瞬间。</em><br/>
  <sub>The light that lingers after the moment has passed.</sub>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-Windows%2010%2F11-blue?style=flat-square" />
  <img src="https://img.shields.io/badge/renderer-Direct2D-orange?style=flat-square" />
  <img src="https://img.shields.io/badge/C%2B%2B-20-green?style=flat-square" />
  <img src="https://img.shields.io/badge/license-MIT-lightgrey?style=flat-square" />
</p>

---

A native Windows image viewer built for stillness. No clutter, no noise — just your photos, and the quiet passing of time.

一个为 Windows 打造的原生图片浏览器。没有多余的按钮，没有臃肿的界面——只有你的照片，和安静流淌的时间。

## Features

- **Gallery** — auto-scans your photo library, grouped by year and month, like flipping through a film journal
- **Album Folders** — `Ctrl+D` to curate your own collections, multiple folders woven into one garden
- **Smooth** — Direct2D hardware-accelerated rendering with spring animations, 60fps
- **Instant** — background async scanning & lazy thumbnail loading, never interrupts your browsing
- **Formats** — JPG / PNG / BMP / GIF / TIFF / WebP / HEIC / AVIF / ICO / JXR
- **Lightweight** — pure C++20, no third-party frameworks, runs out of the box

## Shortcuts

| Key | Action |
|-----|--------|
| `Ctrl+D` | Add album folder |
| `Ctrl+O` | Open image file |
| `Esc` | Back to gallery |
| `←` `→` | Previous / Next |
| Scroll | Gallery scroll / Image zoom |
| Drag & Drop | View dropped images |

## Build

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output at `build/bin/Release/ultra_image_viewer.exe`

## Architecture

```
src/
├── core/              # Application, ImageDecoder, ImagePipeline, CacheManager
├── rendering/         # Direct2D renderer, texture management
├── ui/                # GalleryView, ImageViewer, ViewManager
├── animation/         # Spring-based animation engine
└── image_processing/  # Color correction, sharpening, RAW decode
```

## Album Folders

On first launch, Afterglow scans your system Pictures / Desktop / Downloads folders automatically.

Press `Ctrl+D` to add your own album folders. Add as many as you like — all photos merge into a single timeline, sorted by date. Your folder list is saved and restored on next launch.

## System Requirements

- **OS**: Windows 10 / 11
- **CPU**: x64
- **GPU**: DirectX 11 Feature Level 10+
- **RAM**: 4GB+

## License

MIT License. See [LICENSE](LICENSE) for details.

---

<p align="center">
  <sub>拾起光，就是拾起记忆。</sub><br/>
  <sub>To gather light is to gather memory.</sub>
</p>
