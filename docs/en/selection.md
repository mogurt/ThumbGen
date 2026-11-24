# Layers and image sequences

[Documentation](README.md) · [简体中文](../selection.md)

First inspect the channels and parts:

```sh
thumbgen inspect multilayer.exr --json
```

## Pick the main image

ThumbGen prefers clearly named Beauty, Combined or RGBA images, then top-level RGB or a unique color/grayscale group. Ambiguous candidates require an explicit choice. Selection does not depend on brightness, so a black frame does not cause a layer switch.

```sh
thumbgen convert multilayer.exr --output beauty.jpg --channels Beauty
thumbgen convert multilayer.exr --output rgb.jpg --channels RGB
thumbgen convert multipart.exr --output preview.jpg --part-name beauty
```

`--part N` selects a zero-based index and cannot be combined with `--part-name`. Use [data previews](data-preview.md) for depth, normals and IDs.

With `--visualize auto` and the default selection policy, a unique unprefixed RGB(A) group can resolve ambiguity between parts when all remaining candidates are recognized auxiliary AOVs such as shadows or ambient occlusion. Without a color candidate, the same mode can preview a Cryptomatte family across parts. A unique object family named `crypto_object` or `CryptoObject` takes precedence over other families, regardless of case. Other ambiguities require an explicit part, channel group or Cryptomatte family.

For supported PSD v1 files, automatic color selection uses the stored composite when you omit both part and channel group. You can also select the composite directly with `--part 0` and optionally specify a channel group. For Raw and RLE files, `inspect` still lists available layers and you can select one explicitly by index or name. For ZIP and ZIP with prediction, `inspect` shows the supported composite only.

## Keep sequences consistent

Give related batch jobs the same `sequence_id` to keep their selected layers and channels consistent. A later frame with missing or changed channels reports an error instead of silently selecting another image.

Use separate IDs for different cameras, views and render layers. The selection lasts only for that batch, not across program runs. Use named parts for multipart sequences.

See [batch processing](protocol.md).
