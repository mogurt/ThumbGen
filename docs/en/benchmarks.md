# Images and settings

[Documentation](README.md) · [简体中文](../benchmarks.md)

The homepage shows speed and format support for everyday images, PNG16, EXR and tiny-image batches.

## Image sources

Everyday images come from four public originals and three format derivatives. PNG16 uses twelve synthetic images; the tiny batch repeats one 256×192 PNG. EXR includes seven still images and an eight-frame Beachball sequence.

Download links, authors and licenses are in [image sources](../performance/public-assets.json).

## Output settings

One size fits within 425×260; two sizes add 960×960. Aspect ratio is preserved and small images are not enlarged. Previews are 8-bit.

Color processing, filters and compression differ between tools. Pillow resizes in 8-bit. FFmpeg JPEG uses q2 versus Q90 for the other tools; these are not equal-quality settings. The ImageMagick package has no OpenMP. The libvips EXR input uses a Magick loader with highlight color issues.

ImageMagick and libvips read six still images; Kapaa is unreadable. The libvips previews have highlight color issues. Beachball outputs from libvips, FFmpeg and ImageMagick use a different canvas.

## Downloads

- [Processing time and memory](../performance/timings.csv)
- [Image support and limitations](../performance/coverage.json)
- [Environment and parameters](../performance/settings.json)
- [Public images](public-assets.md)
