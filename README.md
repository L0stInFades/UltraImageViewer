<p align="center">
  <img src="assets/icon_preview.jpg" width="128" style="border-radius: 24px;" />
</p>

<h1 align="center">拾光 Shiguang</h1>

<p align="center">
  <em>在时间的碎片里，拾起那些被光照亮的瞬间。</em><br/>
  <sub>Gathering light from the fragments of time.</sub>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-Windows%2010%2F11-blue?style=flat-square" />
  <img src="https://img.shields.io/badge/renderer-Direct2D-orange?style=flat-square" />
  <img src="https://img.shields.io/badge/C%2B%2B-20-green?style=flat-square" />
  <img src="https://img.shields.io/badge/license-MIT-lightgrey?style=flat-square" />
</p>

---

一个为 Windows 打造的原生图片浏览器。没有多余的按钮，没有臃肿的界面——只有你的照片，和安静流淌的时间。

A native Windows image viewer. No clutter, no noise — just your photos, and the quiet passing of time.

## Features

- **Gallery** — 自动扫描系统相册，按年月分组，像翻阅一本时光簿
- **Album Folders** — `Ctrl+D` 指定你的相册文件夹，多个文件夹汇聚成一座花园
- **Smooth** — Direct2D 硬件加速渲染，弹簧动画过渡，60fps 丝滑
- **Instant** — 后台异步扫描与缩略图加载，不打断你的浏览
- **Formats** — JPG / PNG / BMP / GIF / TIFF / WebP / HEIC / AVIF / ICO / JXR
- **Light** — 纯 C++20，无第三方框架依赖，启动即用

## Shortcuts

| Key | Action |
|-----|--------|
| `Ctrl+D` | 添加相册文件夹 |
| `Ctrl+O` | 打开图片文件 |
| `Esc` | 返回画廊 |
| `←` `→` | 上一张 / 下一张 |
| 滚轮 | 画廊滚动 / 图片缩放 |
| 拖放 | 拖入图片直接查看 |

## Build

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

产物位于 `build/bin/Release/ultra_image_viewer.exe`

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

首次启动自动扫描系统 Pictures / Desktop / Downloads 等目录。

按 `Ctrl+D` 可以指定自己的相册文件夹。支持多次添加，所有文件夹的照片会合并显示、按日期分组。文件夹列表会自动保存，下次启动时恢复。

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
