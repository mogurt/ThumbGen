# Color management

[Documentation](README.md) · [简体中文](../color-cms.md)

ThumbGen supports OpenColorIO project configurations and embedded ICC profiles in supported images. It does not auto-expose or stretch brightness separately for each image, helping keep sequences consistent.

## Use a project configuration

```sh
thumbgen convert frame.exr --output preview.jpg --ocio-config config.ocio --input-space ACEScg --display DISPLAY --view VIEW
```

Replace `DISPLAY` and `VIEW` with names from your configuration and set them together. Configuration priority is `--ocio-config`, then the `OCIO` environment variable, then the built-in configuration. Choose `--ocio-config ocio://default` to explicitly use the built-in one.

Input space comes from `--input-space` first, otherwise file metadata. With no suitable metadata, EXR/HDR is assumed linear and other images sRGB, with a warning. EXR does not necessarily mean ACEScg; declare a known project space explicitly.

## Embedded ICC

Supported single-image PNG, JPEG, TIFF and WebP inputs automatically use RGB / grayscale ICC profiles. An explicit input space overrides the embedded profile, so use it only when you know the source colors.

Four-channel CMYK TIFF and supported 8-bit baseline Adobe CMYK/YCCK JPEG inputs with reliable ICC profiles can produce RGB previews. Missing profiles, unsupported ink layouts or damaged ICC data cause errors. Assigning an RGB space to CMYK is not a substitute for proper conversion.

CMYK/Multichannel PSD/PSB, Deep ICC, multipage/multiframe ICC and other special containers have additional restrictions. Use `capabilities --json` for available features and `inspect --json` for input information.

## Transparency and output

Use `--background transparent` to retain transparency in PNG/WebP; choose black or white for JPEG. Color previews account for Alpha during resizing and honor EXR display canvases and image orientation.

[Data previews](data-preview.md) bypass OCIO and ICC and use your chosen numeric range.
