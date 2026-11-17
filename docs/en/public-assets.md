# Sample images

[Documentation](README.md) · [简体中文](../public-assets.md)

Try public samples to explore EXR, layers and transparency. Check the license for each download.

| Source | Content |
| --- | --- |
| [OpenEXR sample images](https://github.com/AcademySoftwareFoundation/openexr-images) | Regular EXR, Deep, multipart images and sequences |
| [Psyop Cryptomatte](https://github.com/Psyop/Cryptomatte) | Cryptomatte samples and format information |
| [Poly Haven](https://polyhaven.com/) | CC0 HDRIs and textures |

For image URLs, licenses and checksums, see [public-assets.json](../performance/public-assets.json). See [images and settings](benchmarks.md) for conversion options.

## Homepage preview images

| Image pair | Public source | Processing |
| --- | --- | --- |
| Cryptomatte | [Psyop Cornell Box and Bunny samples](https://github.com/Psyop/Cryptomatte/tree/968d5e4b6171e29ba5f89d554117132a164e747e/sample_images) | Full `uCryptoWildcard` and `uCryptoAsset` families; these are two different scenes |
| Depth | [OpenEXR Beachball, first frame](https://github.com/AcademySoftwareFoundation/openexr-images/tree/e38ffb0790f62f05a6f083a6fa4cac150b3b7452/Beachball) | RGB and Z from the same file; fixed depth range 9–11 in source units, black near and white far |
| OCIO | [OpenEXR StillLife](https://github.com/AcademySoftwareFoundation/openexr-images/tree/e38ffb0790f62f05a6f083a6fa4cac150b3b7452/ScanLines) | Declared Linear Rec.709 (sRGB) input, sRGB display; Un-tone-mapped versus ACES 1.0 - SDR Video |

ThumbGen 0.1.1 generated the previews, which are proportionally laid out without cropping or additional grading. Checkerboards indicate transparency. OCIO uses the built-in `ocio://cg-config-v2.2.0_aces-v1.3_ocio-v2.4` configuration. Both Views are valid choices, rather than an incorrect/correct comparison. Depth is a data preview and does not use OCIO.

[Image settings and source checksums](../assets/features/sources.json) · [Psyop license](../assets/features/Cryptomatte-BSD-3-Clause.txt) · [OpenEXR license](../assets/features/OpenEXR-BSD-3-Clause.txt)

Do not upload confidential projects, unlicensed assets or project color configurations. A small shareable input is usually the most useful way to report an issue.
