# Getting started

[Documentation](README.md) · [简体中文](../getting-started.md)

Extract the complete ThumbGen folder, keeping its shared libraries alongside the program. Add the program directory to PATH, or invoke the executable by its full path. Python is not required to run ThumbGen.

## Make your first preview

```sh
thumbgen convert image.exr --output preview.jpg --size 960
```

`--size 960` fits the image within 960×960, preserving its aspect ratio without cropping or upscaling.

Generate two sizes at once:

```sh
thumbgen convert image.exr --output-dir previews --size 425x260 --size 960x960
```

The default is JPEG, quality 90, with a black background. Choose PNG or WebP to keep transparency:

```sh
thumbgen convert image.exr --output preview.png --background transparent
```

## Explore an input

```sh
thumbgen inspect image.exr --json
thumbgen capabilities --json
```

The first command lists layers, channels and parts; the second lists formats and features available in your copy of ThumbGen. Use `--channels Beauty` for a specific image layer. See [layer selection](selection.md).

## Run it again

Existing files are not overwritten by default. Use `--existing skip` to keep them or `--existing replace` to regenerate them.

Next, explore [color management](color-cms.md), [batch processing](protocol.md), or the [command reference](cli.md).
