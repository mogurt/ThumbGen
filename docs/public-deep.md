# Deep EXR 预览

[文档目录](README.md) · [English](en/public-deep.md)

Deep 文件需要先展平才能生成普通图片。使用 `--deep-mode flatten` 明确开启；默认不自动展平。

```sh
thumbgen convert deep.exr --output preview.png --deep-mode flatten --input-space linear
thumbgen convert deep.exr --output preview.jpg --deep-mode flatten --background white
```

输入应为关联颜色，并使用正确的线性色彩空间。项目空间不是 `linear` 时，请换成实际配置里的名称。

## 支持范围

支持 Deep scanline / tiled 的 level 0，颜色、Alpha 和深度通道为 HALF / FLOAT。NONE、RLE、ZIPS 压缩可用；其他压缩的可用性请查询 `capabilities --json`。缺少必要通道、不支持的布局和超出样本限制会报告错误。

默认每像素最多 4096 个样本，可用 `--deep-samples-per-pixel` 调整。`--max-decode-mib` 默认 512，控制解码缓冲区；很大的 Deep 文件可能需要提高限额，同时为整幅画布和输出留出内存。

## 背景与透明度

黑底或白底适合查看复杂的透明与发光内容。透明 PNG / WebP 只支持共用 Alpha 的结构；独立通道透明率或零 Alpha 发光无法用普通透明图准确表达时，会明确报错。此时可尝试不透明背景。

Deep 颜色展平不能与深度、法线、ID 或 Cryptomatte 数据预览同时使用。多 part 和序列选择见[图层选择](selection.md)。
