# Preview images

Real ThumbGen previews, with no cropping or additional grading. The checkerboard is a display background for transparent pixels. Source images retain their BSD-3-Clause licenses; the license texts are included here.

See [image sources and settings](../../en/public-assets.md#homepage-preview-images) or [中文说明](../../public-assets.md#首页预览配图). Exact public source URLs, checksums and ThumbGen options are in [sources.json](sources.json).

To recreate the gallery, download the four source EXRs and two license texts listed in `sources.json` and `docs/performance/public-assets.json`, then run:

```sh
python scripts/public/render_features.py --thumbgen thumbgen --inputs path/to/public-images
```

The layout script uses Pillow and Matplotlib. ThumbGen itself does not need Python. Full preview PNGs are kept locally; the three paired images here are sized for the README.
