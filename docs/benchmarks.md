# 图片与参数

[文档目录](README.md) · [English](en/benchmarks.md)

首页展示普通图片、PNG16、EXR 和小图批处理的速度与格式支持。

## 图片来源

普通图片来自四份公开原图及三份格式派生文件。PNG16 为十二张合成图片，小图批次由同一张 256×192 PNG 的副本组成。EXR 包含七份静态图片和 Beachball 八帧序列。

文件下载地址、作者和许可见[图片来源清单](performance/public-assets.json)。

## 输出设置

单尺寸适配 425×260，双尺寸再加 960×960，保持比例，不放大小图。预览为 8-bit 输出。

不同工具的色彩处理、滤波和压缩设置不同。Pillow 使用 8-bit 缩放；FFmpeg JPEG 使用 q2，其他工具为 Q90，两者并非相同画质。所用 ImageMagick 不含 OpenMP；libvips 的 EXR 读取经由 Magick loader，存在高光颜色问题。

静态 EXR 中，ImageMagick 和 libvips 可读取六份图片，Kapaa 无法读取。libvips 的输出存在高光颜色异常；Beachball 序列在 libvips、FFmpeg 和 ImageMagick 中有输出，但画布尺寸不符。

## 数据下载

- [处理耗时与内存](performance/timings.csv)
- [图像支持与限制](performance/coverage.json)
- [环境与参数](performance/settings.json)
- [公开素材](public-assets.md)
