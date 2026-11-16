# 深度、法线与 ID 预览

[文档目录](README.md) · [English](en/data-preview.md)

数据通道不是普通颜色。使用 `--visualize` 指定含义，再用 `--channels` 选择通道。数据预览不应用 OCIO、自动曝光或逐帧范围调整。

| 模式 | 用法 |
| --- | --- |
| depth | 一个深度通道，按固定范围映射到灰度 |
| normal | XYZ / RGB 法线，默认范围 [-1,1] |
| vector | XY / XYZ、RG / RGB 或 UV，显式指定范围 |
| id | 整数 ID 稳定配色，0 为黑色 |
| id_rgb | 已有 RGB 标签，指定原始数值范围 |
| cryptomatte | 完整家族的覆盖率总览，见 [Cryptomatte](cryptomatte.md) |

```sh
thumbgen convert frame.exr --output depth.png --visualize depth --channels Z --data-range 0,1000
thumbgen convert frame.exr --output normal.png --visualize normal --channels N
thumbgen convert frame.exr --output position.png --visualize vector --channels P --data-range -1000,1000
thumbgen convert frame.exr --output ids.png --visualize id --channels object_id
```

`0,1000` 是源单位的示例，不是默认值。连续帧使用同一范围才能比较远近。深度、矢量和 RGB 标签需要显式范围；整数 ID 不需要范围。范围之外的有效值会截断到显示范围。

无效数值默认显示品红色，也可选 `--invalid-values black` 或 `--invalid-values error`。ID 使用最近邻缩放，避免混合相邻类别；配色不是可逆的 ID 编码。需要保留预览色值时使用 PNG。

批处理用 `visualization`、`channels`、`data_range`、`invalid_values` 指定相同选项，使用 `sequence_id` 保持序列映射一致。
