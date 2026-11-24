# 色彩管理

[文档目录](README.md) · [English](en/color-cms.md)

ThumbGen 支持 OpenColorIO 项目配置，也能处理受支持图片中的嵌入 ICC。默认不自动曝光，不按每张图单独拉伸亮度，方便连续帧保持一致。

## 使用项目配置

```sh
thumbgen convert frame.exr --output preview.jpg --ocio-config config.ocio --input-space ACEScg --display DISPLAY --view VIEW
```

将 `DISPLAY` 和 `VIEW` 换成项目配置中的实际名称，两者要一起设置。配置优先使用 `--ocio-config`，其次是环境变量 `OCIO`，最后是内置配置。使用 `--ocio-config ocio://default` 可以明确选择内置配置。

输入空间先采用 `--input-space`，否则读取文件元数据。信息缺失时，EXR/HDR 假定为线性，其他图片假定为 sRGB，并给出警告。EXR 不一定是 ACEScg，已知项目空间时请显式指定。

## 嵌入 ICC

受支持的单张 PNG、JPEG、TIFF、WebP 会自动应用 RGB 或灰度 ICC。自动选择输入空间且未指定配置、显示器或视图时，这类文件使用 `ocio://default`，不受 `OCIO` 环境变量影响。显式选择配置或显示器与视图时，遵循上文的项目配置规则。显式指定输入空间会覆盖嵌入配置。

用于 ICC 转换的项目配置须能解析标准非线性场景参考空间 `srgb_rec709_scene`。可直接用标准名声明该色彩空间，也可声明对应别名并设置同名 interop ID。CMYK ICC 仍遵循显式指定或从环境继承的项目配置。

带可靠 ICC 的四通道 CMYK TIFF，以及受支持的 8-bit baseline Adobe CMYK/YCCK JPEG，可以转为 RGB 预览。没有可靠 profile、分色布局不支持或 ICC 损坏时会报告错误；给 CMYK 强行指定 RGB 空间不能替代正确转换。

PSD/PSB 的 CMYK/Multichannel、Deep ICC、多页/多帧 ICC 以及其他特殊容器有额外限制。可以用 `capabilities --json` 查看功能，用 `inspect --json` 查看文件信息。

## 透明度与输出

PNG/WebP 保留透明度需要 `--background transparent`；JPEG 请选择黑底或白底。普通颜色预览会在缩放时正确处理 Alpha，EXR 的显示画布与方向也会应用到输出。

[数据预览](data-preview.md)不使用 OCIO 或 ICC，数值范围由你指定。
