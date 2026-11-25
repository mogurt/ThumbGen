# ThumbGen

[English](README.en.md) · [文档目录](docs/README.md) · [CLI 参考](docs/cli.md)

**原生 C++ 的速度，OCIO 的颜色，Crypto / Deep / Z 的预览能力。**

ThumbGen 为普通图片和复杂 EXR 快速生成缩略图。一次解码输出多个尺寸，并行处理大批图片，运行时不需要 Python。

为制作管线提供快速、懂 EXR 的预览能力。

`0.1.6` · [Apache-2.0](LICENSE)

## 适用场景

面向既需要批量处理速度，又需要深入读取 EXR 图层、通道与色彩信息的制作流程：

- **渲染农场与结果检查**：批量生成多尺寸帧预览，浏览颜色、法线、深度和 Crypto，辅助检查渲染结果与 AOV。
- **美术、合成与技术美术管线**：按项目 OCIO 配置生成预览，查看多层 EXR、数据通道和受支持的 Deep 内容。
- **资产入库、版本管理与交付**：为 EXR、贴图和 HDR 素材生成列表与详情预览，方便查找素材、浏览版本和查看交付内容。
- **桌面工具与云端平台集成**：通过 CLI 和 JSONL 接入文件选择器、发布面板或远程素材库，在文件所在机器生成预览，减少浏览时的大文件传输。

## 能做些什么

| 能力 | 可以做什么 |
| --- | --- |
| **快速** | 一次解码、多尺寸输出，支持并行批处理 |
| **原生 C++** | C++17 + OpenImageIO，可直接部署的命令行工具 |
| **[OCIO](docs/color-cms.md)** | 使用项目色彩配置，指定输入空间、display 和 view |
| **[Crypto](docs/cryptomatte.md)** | 将 Cryptomatte 的 ID 与 coverage 转成稳定配色的总览 |
| **[Deep](docs/public-deep.md)** | 显式展平受支持的 Deep EXR，生成普通预览图 |
| **[Z 深度](docs/data-preview.md)** | 按固定范围生成灰度深度预览，保持序列帧映射一致 |

PNG、JPEG、TIFF 等常见图片也能处理；支持 Alpha、方向、多 part 和多通道选层。需要接脚本时，直接读取逐文件的 JSONL 进度和结果。

## 速度与格式支持

**快速生成预览，也能处理复杂 EXR。** 支持多通道选择、透明度和完整显示画布。下图对比不同工具的处理耗时，同时标注格式支持与输出限制。

![五工具对比：EXR、普通图片与小图批处理的耗时及输出限制](docs/assets/five-tool-overview.svg)

上图中，EXR 和普通图片每张生成小、大两张预览，小图批次每张只生成一张。预览保持原始比例，不拉伸。不同工具的色彩、滤波与压缩设置有所区别。[完整数据、输出尺寸与图片来源 →](docs/benchmarks.md)

## 看看预览效果

### Cryptomatte：看清物体分布与透明边缘

![两个场景的 Cryptomatte 缩略图，展示稳定的 ID 配色、毛发边缘与覆盖率透明度](docs/assets/features/cryptomatte.png)

将 ID 与覆盖率转成稳定配色的总览，方便检查场景中的物体与边缘；不是按名称提取单个 matte。[Cryptomatte 用法 →](docs/cryptomatte.md)

### Depth：数据通道也能直接看

![同一 EXR 的颜色预览与 Z 深度灰度预览，深度范围固定为源单位 9 到 11](docs/assets/features/depth.png)

选择 Z 通道并指定范围，生成直观的灰度预览。连续帧沿用相同范围，远近映射保持一致。[深度与数据预览 →](docs/data-preview.md)

### OpenColorIO：缩略图跟随项目色彩

![同一线性 Rec.709 EXR 在 sRGB 显示下，分别应用 Un-tone-mapped 与 ACES 1.0 SDR Video 变换](docs/assets/features/ocio.png)

使用项目 OCIO 配置，指定输入空间、Display 和 View。上图只切换显示变换，展示不同的高光与色调呈现。[色彩管理 →](docs/color-cms.md)

以上均为 ThumbGen 实际输出，使用可公开分发的 EXR 素材。[素材来源与图片设置 →](docs/public-assets.md#首页预览配图)

## 平台与格式

支持 Windows x64 和 Linux x64，已在 CentOS 7.9、Rocky Linux 9 等环境使用。

可读取 OpenEXR、PNG、JPEG、TIFF、WebP、TGA、BMP、Radiance HDR、DPX 和受支持的 PSD 合成图，输出 JPEG、PNG 和有损 WebP。支持有界解码的布局与预算设置见[大图预览](docs/large-previews.md)。可用 `thumbgen capabilities --json` 查看当前程序的格式支持。

## 快速开始

保留完整程序目录和随附动态库，加入 PATH，或直接用程序的完整路径运行。[安装说明 →](docs/installation.md)

```sh
thumbgen convert frame.exr --output preview.jpg --size 960
thumbgen convert frame.exr --output-dir previews --size 425x260 --size 960x960
thumbgen convert frame.exr --output preview.png --background transparent
```

图片保持比例，不放大小图。默认保留已有文件并提示错误，需要覆盖时加 `--existing replace`。

多层图片或项目色彩配置：

```sh
thumbgen inspect multilayer.exr --json
thumbgen convert multilayer.exr --output beauty.jpg --channels Beauty
thumbgen convert frame.exr --output preview.jpg --ocio-config config.ocio --input-space ACEScg --display DISPLAY --view VIEW
```

数据通道和 Deep 预览：

```sh
thumbgen convert depth.exr --output depth.png --visualize depth --channels Z --data-range 0,1000
thumbgen convert crypto.exr --output crypto.png --visualize cryptomatte --channels CryptoObject
thumbgen convert deep.exr --output deep.png --deep-mode flatten
```

通道名和深度范围请按实际文件调整。Cryptomatte 生成彩色总览，不按对象名称提取 matte。

## 批量处理

把文件和输出尺寸写进任务清单，然后运行：

```sh
thumbgen batch --manifest jobs.json --json
```

每张图可以生成多个尺寸，也可以并行处理多张图。JSONL 结果方便脚本跟踪进度。[清单示例与结果处理 →](docs/protocol.md)

## 继续了解

[快速上手](docs/getting-started.md) · [命令参考](docs/cli.md) · [色彩管理](docs/color-cms.md) · [全部指南](docs/README.md)

遇到问题，欢迎带上版本、平台、命令和可分享的小素材反馈；保密资产与项目色彩配置请留在本地。

## 许可证

ThumbGen 使用 [Apache-2.0](LICENSE) 许可证。第三方依赖保留各自的许可证，相关材料见 [dependencies](dependencies/)。
