# Cryptomatte previews

[Documentation](README.md) · [简体中文](../cryptomatte.md)

Turn Cryptomatte IDs and coverage into a stable-color overview for checking object regions and edge coverage. This is not a named-matte extraction tool.

```sh
thumbgen inspect crypto.exr --json
thumbgen convert crypto.exr --output crypto.png --visualize cryptomatte --channels CryptoObject --background transparent
```

Set `--channels` to the complete family name returned by inspect, not an individual rank layer. ThumbGen combines coverage across the family into one preview, with Alpha derived from coverage.

## Supported inputs

Inputs need valid Cryptomatte metadata and complete, consecutively numbered layers starting at 00. Layers may span flat parts with matching data windows, display windows and orientation. Select a multipart family by name. When using `--part` or `--part-name`, the selected part must contain the entire family. IDs must be FLOAT32. Coverage may be FLOAT32 or HALF.

A batch can mix ordinary and Cryptomatte previews. On each Cryptomatte job, set `options.visualization` to `cryptomatte` and `options.channels` to the family name reported by inspect.

ID colors are stable across frames rather than randomized from image content. Invalid values use `--invalid-values black|magenta|error`, defaulting to magenta.

External manifests are not used to select named objects. Deep Cryptomatte is not supported. PNG is recommended for retaining preview colors and transparent edges.
