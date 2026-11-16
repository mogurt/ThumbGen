# Cryptomatte previews

[Documentation](README.md) · [简体中文](../cryptomatte.md)

Turn Cryptomatte IDs and coverage into a stable-color overview for checking object regions and edge coverage. This is not a named-matte extraction tool.

```sh
thumbgen inspect crypto.exr --json
thumbgen convert crypto.exr --output crypto.png --visualize cryptomatte --channels CryptoObject --background transparent
```

Set `--channels` to the complete family name returned by inspect, not an individual rank layer. ThumbGen combines coverage across the family into one preview, with Alpha derived from coverage.

## Supported inputs

Inputs need valid Cryptomatte metadata and complete, consecutively numbered layers in one flat part. IDs must be FLOAT32; coverage may be FLOAT32 or HALF. Missing ranks, ranks split across parts and unsupported ID types produce errors.

ID colors are stable across frames rather than randomized from image content. Invalid values use `--invalid-values black|magenta|error`, defaulting to magenta.

External manifests are not used to select named objects. Deep Cryptomatte is not supported. PNG is recommended for retaining preview colors and transparent edges.
