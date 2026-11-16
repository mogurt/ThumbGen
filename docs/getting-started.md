# 快速上手

[文档目录](README.md) · [English](en/getting-started.md)

将完整的 ThumbGen 程序目录解压到电脑上，保留随附的动态库。把程序目录加入 PATH 后，就可以在终端运行 `thumbgen`；也可以直接使用程序的完整路径。运行时不需要 Python。

## 生成第一张预览

```sh
thumbgen convert image.exr --output preview.jpg --size 960
```

`--size 960` 表示放进 960×960 的边界框，保留比例，不裁切，也不放大小图。

一次生成两个尺寸：

```sh
thumbgen convert image.exr --output-dir previews --size 425x260 --size 960x960
```

默认输出 JPEG，黑色背景、质量 90。要保留透明度，选择 PNG 或 WebP：

```sh
thumbgen convert image.exr --output preview.png --background transparent
```

## 看看文件里有什么

```sh
thumbgen inspect image.exr --json
thumbgen capabilities --json
```

前者列出图层、通道和 part；后者列出当前程序可用的格式与功能。多层文件可用 `--channels Beauty` 指定主画面，详见[图层选择](selection.md)。

## 再次运行

默认不覆盖已有文件。加 `--existing skip` 保留旧图，或加 `--existing replace` 重新生成。

接下来可以了解[色彩管理](color-cms.md)、[批处理](protocol.md)，或查看完整的[命令参考](cli.md)。
