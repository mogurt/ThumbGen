# Depth, normal and ID previews

[Documentation](README.md) · [简体中文](../data-preview.md)

Data channels are not ordinary colors. Declare their meaning with `--visualize` and select them with `--channels`. These previews do not apply OCIO, automatic exposure or per-frame range adjustments.

| Mode | Use |
| --- | --- |
| depth | One depth channel mapped to grayscale over a fixed range |
| normal | XYZ / RGB normals, default range [-1,1] |
| vector | XY / XYZ, RG / RGB or UV, with an explicit range |
| id | Stable colors for integer IDs; zero is black |
| id_rgb | Existing RGB labels, with their original numeric range |
| cryptomatte | A complete family's coverage overview; see [Cryptomatte](cryptomatte.md) |

```sh
thumbgen convert frame.exr --output depth.png --visualize depth --channels Z --data-range 0,1000
thumbgen convert frame.exr --output normal.png --visualize normal --channels N
thumbgen convert frame.exr --output position.png --visualize vector --channels P --data-range -1000,1000
thumbgen convert frame.exr --output ids.png --visualize id --channels object_id
```

`0,1000` is an example in source units, not a default. Keep the same range across frames to compare depth. Depth, vectors and RGB labels require an explicit range; integer IDs do not. Valid values outside the range are clipped for display.

Invalid values appear magenta by default. Choose `--invalid-values black` or `--invalid-values error` instead if needed. IDs use nearest-neighbor resizing to avoid mixing categories. Preview colors cannot be decoded back into unique IDs; choose PNG to preserve the preview colors.

Batch options are `visualization`, `channels`, `data_range` and `invalid_values`. Use `sequence_id` to keep sequence mappings consistent.
