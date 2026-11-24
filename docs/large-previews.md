# 大图预览

[文档目录](README.md) · [English](en/large-previews.md)

```sh
thumbgen convert renders/frame.exr --output-dir previews --size 960x960 --size 425x260 --preset balanced --max-decode-mib 512 --thread-budget 1 --memory-budget-mib 1024
```

使用 `balanced` 时，符合条件的输入可一次解码并生成最多两种尺寸，只保留有限的原生解码数据和缩放缓冲区。通常从 800 万源像素开始启用。具有位移或裁切显示窗口的平面 EXR 在较小尺寸也可使用。输出按给定尺寸等比缩小，最多保留原尺寸。

| 输入格式 | 支持有界颜色预览的布局 |
| --- | --- |
| JPEG | 8 位灰度、RGB 和 YCbCr。多扫描和渐进式图像还需为完整系数表预留内存。Adobe CMYK/YCCK 使用支持 ICC 的整图路径。 |
| PNG | 原生通道匹配的 8 位、16 位 RGB/RGBA，包括转为 Alpha 的 RGB 透明度。Adam7 需保留完整原生帧。 |
| TIFF | 无压缩、Deflate、LZW 或 PackBits 的 8 位、16 位无符号交错 RGB/RGBA，支持条带和瓦片。所选页需为压缩数据及解码后的条带或一行瓦片预留内存。 |
| 平面 OpenEXR | 扫描线或单层瓦片，所选 RGB/RGBA 通道均为 HALF 或均为 FLOAT，通道采样率为 1。支持 NONE、RLE、ZIPS、ZIP、PIZ 压缩、part 选择及带负坐标的数据窗口和显示窗口。 |
| Radiance HDR | RGBE 行解码为浮点。 |
| DPX | 无压缩的单元素无符号 RGB，支持符合打包与行对齐要求的 8、10、12、16 位布局。 |
| BMP | 无压缩的 24 位 RGB 和 32 位 RGB(A)。 |
| TGA | 无压缩、非交错的 24 位 RGB 和 32 位 RGBA。解码器保留完整原生帧。 |
| WebP | 有损静态 RGB/RGBA。解码器保留压缩输入和原生帧。无损和动画可在满足解码限制时使用整图路径。 |

有界布局每边最多 65,536 像素，格式本身可能有更低的限制。EXR 的限制包括 8 MiB 元数据分配、最多 1,048,576 个偏移表条目和 64 MiB 解包块。缩放工作缓冲区也有上限。其他布局、预设、更多输出尺寸以及数据或 Deep 预览，在满足对应限制时沿用现有处理路径。输入校验失败或超出计划限制时，该任务会在写出预览前报告错误。

`--max-decode-mib` 默认为 512，限制每个任务的解码计划。有界预览的计划包含原生数据、解码器临时区、色彩与缩放缓冲区、输出缓冲区以及所需的 ICC 预留。独立进程的共享 `--memory-budget-mib` 覆盖控制器与空闲进程的固定预留，以及活跃任务的增量估算与余量。这些是应用级限制和估算。`decode_limit_exceeded` 表示解码计划超限，`task_memory_exceeds_budget` 表示控制器无法准入该任务。JSON 输出中的 `execution.admission_mib` 是任务获准后的增量内存估算。

OCIO 和 ICC 的行为见[色彩管理](color-cms.md)，批处理预算选项见[命令参考](cli.md)。
