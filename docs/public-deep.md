# Deep EXR 预览

[文档目录](README.md) · [English](en/public-deep.md)

要生成 Deep 彩色预览，需用 `--deep-mode flatten` 明确开启展平。

```sh
thumbgen convert deep.exr --output preview.png --deep-mode flatten --input-space linear
thumbgen convert deep.exr --output preview.jpg --deep-mode flatten --background white
```

输入应为关联颜色，并使用正确的线性色彩空间。项目空间不是 `linear` 时，请换成实际配置里的名称。

## 自动不透明度预览

```sh
thumbgen convert coverage.exr --output preview.jpg --visualize auto
```

常规选择找不到可用图像时，自动模式可预览唯一符合条件的 Deep part。该 part 只能含根层级的 `A`、`Z` 和可选的 `ZBack`（不区分大小写），且通道必须是 HALF 或 FLOAT。预览沿用 Deep 彩色展平的原生解码器、样本合成算法和限额。额外通道、分层通道、多个不透明度候选和声明了 Cryptomatte 系列的文件均不触发此回退。RGB Deep 仍需明确启用彩色展平。

批处理中，默认项和任务选项均省略 `deep_mode` 时才启用此回退。显式设置 `reject` 会关闭它，包括从批处理默认项继承的设置。显式指定 part 或通道、使用 `--visualize colour` 或 `--deep-mode flatten` 也会关闭此回退。

累计覆盖率直接映射到灰度。零覆盖为黑，完全覆盖为白，50% 覆盖在 8 位输出中约为 128。画布以显示窗口为准，数据窗口外的覆盖率为零。黑底和白底设置使用同一灰度刻度。透明 PNG 或 WebP 使用白色，Alpha 为覆盖率。覆盖率不经过 OCIO 处理，包括继承或显式配置的变换。

结果包含 `deep.policy = "deep-opacity-sweep-v1"` 及 `TG_EXR_AUTO_DEEP_OPACITY` 告警。参见[图层选择与告警标记](selection.md)。

## 支持范围

支持 Deep scanline / tiled 的 level 0，颜色、Alpha 和深度通道为 HALF / FLOAT。NONE、RLE、ZIPS 压缩可用；其他压缩的可用性请查询 `capabilities --json`。缺少必要通道、不支持的布局和超出样本限制会报告错误。

默认每像素最多 4096 个样本，可用 `--deep-samples-per-pixel` 调整。`--max-decode-mib` 默认 512，控制解码缓冲区；很大的 Deep 文件可能需要提高限额，同时为整幅画布和输出留出内存。

## 背景与透明度

黑底或白底适合查看复杂的透明与发光内容。透明 PNG / WebP 只支持共用 Alpha 的结构；独立通道透明率或零 Alpha 发光无法用普通透明图准确表达时，会明确报错。此时可尝试不透明背景。

Deep 颜色展平不能与深度、法线、ID 或 Cryptomatte 数据预览同时使用。多 part 和序列选择见[图层选择](selection.md)。
