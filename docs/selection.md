# 选择图层与连续帧

[文档目录](README.md) · [English](en/selection.md)

先查看文件中的通道和 part：

```sh
thumbgen inspect multilayer.exr --json
```

## 指定主画面

默认优先 Beauty、Combined、RGBA 等明确命名的主画面，再尝试顶层 RGB 或唯一颜色/灰度组。多个同级候选无法区分时会提示选择，不按亮度猜测，也不会因画面全黑换层。

```sh
thumbgen convert multilayer.exr --output beauty.jpg --channels Beauty
thumbgen convert multilayer.exr --output rgb.jpg --channels RGB
thumbgen convert multipart.exr --output preview.jpg --part-name beauty
```

`--part N` 也可按从 0 开始的索引选择，但不能与 `--part-name` 同时使用。深度、法线、ID 等数据通道请使用[数据预览](data-preview.md)。

## 连续帧保持一致

在批处理任务中使用相同的 `sequence_id`，即可固定同一组图层与通道。后续帧缺失或改变通道时报告错误，而不会悄悄选择另一张图。

不同相机、视图或渲染层请使用不同的 `sequence_id`。它只在同一批任务中生效，不会记住上一次运行的选择。多 part 序列请使用命名 part。

参见[批处理](protocol.md)。
