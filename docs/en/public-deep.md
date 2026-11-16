# Deep EXR previews

[Documentation](README.md) · [简体中文](../public-deep.md)

Deep files must be flattened to make ordinary images. Enable this explicitly with `--deep-mode flatten`; it is not automatic.

```sh
thumbgen convert deep.exr --output preview.png --deep-mode flatten --input-space linear
thumbgen convert deep.exr --output preview.jpg --deep-mode flatten --background white
```

Inputs should contain associated color in the correct linear color space. Replace `linear` with the actual name in your project configuration when needed.

## Supported inputs

Deep scanline / tiled level 0 is supported with HALF / FLOAT color, Alpha and depth channels. NONE, RLE and ZIPS compression are available; query `capabilities --json` for other compression support. Missing channels, unsupported layouts and exceeded sample limits produce errors.

The default limit is 4096 samples per pixel; adjust it with `--deep-samples-per-pixel`. `--max-decode-mib` defaults to 512 and controls decode buffers. Large Deep files may need a higher limit, with additional memory for the full canvas and output.

## Background and transparency

Black or white backgrounds work well for complex transparency and emissive content. Transparent PNG / WebP requires shared Alpha. Independent channel transparency or zero-Alpha emission that cannot be represented accurately in an ordinary transparent image causes an explicit error. Try an opaque background in that case.

Deep color flattening cannot be combined with depth, normal, ID or Cryptomatte data previews. See [layer selection](selection.md) for multipart files and sequences.
