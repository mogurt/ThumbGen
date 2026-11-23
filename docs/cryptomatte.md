# Cryptomatte 预览

[文档目录](README.md) · [English](en/cryptomatte.md)

将 Cryptomatte 的 ID 和 coverage 显示为稳定配色的总览，方便快速检查物体分布与边缘覆盖。它不是按对象名称提取 matte 的工具。

```sh
thumbgen inspect crypto.exr --json
thumbgen convert crypto.exr --output crypto.png --visualize cryptomatte --channels CryptoObject --background transparent
```

`--channels` 填写 inspect 返回的完整家族名，不是单个 rank 层名。程序会读取整个家族，将各层覆盖率叠加成预览；透明度来自 coverage。

## 支持范围

需要有效的 Cryptomatte 元数据，以及从 00 开始完整、连续的编号层。编号层可以分布在多个 flat part，参与的 part 必须有相同的数据窗口、显示窗口和方向。跨 part 的家族只需指定家族名。若指定 `--part` 或 `--part-name`，该 part 必须包含整个家族。ID 必须为 FLOAT32，coverage 可以是 FLOAT32 或 HALF。

同一批任务可混合普通预览和 Cryptomatte 预览。为 Cryptomatte 文件单独设置任务的 `options.visualization` 为 `cryptomatte`，并将 `options.channels` 设为 inspect 返回的家族名。

使用稳定的 ID 配色，连续帧不会按图像内容重新随机配色。无效值可用 `--invalid-values black|magenta|error` 处理，默认品红色。

本功能不读取外部 manifest 来按名字选择对象，也不处理 Deep Cryptomatte。需要保留颜色和透明边缘时建议输出 PNG。
