# Deep EXR previews

[Documentation](README.md) · [简体中文](../public-deep.md)

Deep color requires explicit flattening with `--deep-mode flatten`.

```sh
thumbgen convert deep.exr --output preview.png --deep-mode flatten --input-space linear
thumbgen convert deep.exr --output preview.jpg --deep-mode flatten --background white
```

Inputs should contain associated color in the correct linear color space. Replace `linear` with the actual name in your project configuration when needed.

## Automatic opacity preview

```sh
thumbgen convert coverage.exr --output preview.jpg --visualize auto
```

If normal selection finds no eligible image, auto mode can preview one unambiguous Deep part containing only base-layer `A`, `Z` and optional `ZBack` (case-insensitive). It requires HALF or FLOAT channels and uses the same native reader, sample sweep and limits as Deep color. Extra or layered channels, multiple opacity candidates and files declaring Cryptomatte families are excluded. RGB Deep still requires explicit color flattening.

Omit `deep_mode` from both batch defaults and job options to enable this fallback. An explicit `reject` disables it, including when inherited from batch defaults. Explicit part or channel selectors, `--visualize colour` and `--deep-mode flatten` also leave it disabled.

The preview maps accumulated coverage directly to grayscale. Zero coverage is black, one is white, and 50% coverage encodes near 128 in 8-bit output. The display window defines the canvas and areas outside the data window have zero coverage. Both opaque background settings use this same grayscale scale. A transparent PNG or WebP uses white with coverage in the alpha channel. Coverage bypasses all OCIO transforms, including inherited and explicitly configured transforms.

The result reports `deep.policy = "deep-opacity-sweep-v1"` and a `TG_EXR_AUTO_DEEP_OPACITY` warning. See [automatic selection and sequence locks](selection.md#monitor-automatic-selection).

## Supported inputs

Deep scanline / tiled level 0 is supported with HALF / FLOAT color, Alpha and depth channels. NONE, RLE and ZIPS compression are available; query `capabilities --json` for other compression support. Missing channels, unsupported layouts and exceeded sample limits produce errors.

The default limit is 4096 samples per pixel; adjust it with `--deep-samples-per-pixel`. `--max-decode-mib` defaults to 512 and controls decode buffers. Large Deep files may need a higher limit, with additional memory for the full canvas and output.

## Background and transparency

Black or white backgrounds work well for complex transparency and emissive content. Transparent PNG / WebP requires shared Alpha. Independent channel transparency or zero-Alpha emission that cannot be represented accurately in an ordinary transparent image causes an explicit error. Try an opaque background in that case.

Deep color flattening cannot be combined with depth, normal, ID or Cryptomatte data previews. See [layer selection](selection.md) for multipart files and sequences.
