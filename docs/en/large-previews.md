# Large image previews

[Documentation](README.md) · [简体中文](../large-previews.md)

```sh
thumbgen convert renders/frame.exr --output-dir previews --size 960x960 --size 425x260 --preset balanced --max-decode-mib 512 --thread-budget 1 --memory-budget-mib 1024
```

The balanced preset can decode eligible sources once for up to two preview sizes, keeping the resize buffers and a bounded portion of the native source in memory. This route normally starts at 8 million source pixels. Flat EXR with a shifted or cropped display window can use it at smaller sizes. Preview sizes are aspect-ratio-preserving bounds and never enlarge the source.

| Input | Layouts eligible for bounded colour previews |
| --- | --- |
| JPEG | 8-bit grayscale, RGB and YCbCr. Multiscan and progressive images also reserve their complete coefficient tables. Adobe CMYK/YCCK uses the profile-aware full-image route. |
| PNG | 8- or 16-bit RGB/RGBA with matching native channels, including RGB with transparency expanded to alpha. Adam7 retains a complete native frame and needs a larger budget. |
| TIFF | Interleaved unsigned 8- or 16-bit RGB/RGBA, stripped or tiled, using uncompressed, Deflate, LZW or PackBits storage. The selected page pays for compressed scratch and the decoded strip or tile row. |
| Flat OpenEXR | Scanline or single-level tiled files with uniformly HALF or FLOAT selected RGB/RGBA channels, unit sampling, and NONE, RLE, ZIPS, ZIP or PIZ compression. Supports part selection and data and display windows, including negative origins. |
| Radiance HDR | RGBE rows decoded to float. |
| DPX | Uncompressed, single-element unsigned RGB at 8, 10, 12 or 16 bits with supported packing and row alignment. |
| BMP | Uncompressed 24-bit RGB and 32-bit RGB(A). |
| TGA | Uncompressed, non-interleaved 24-bit RGB and 32-bit RGBA. The decoder retains a native frame. |
| WebP | Still lossy RGB/RGBA. The decoder retains compressed input and a native frame. Lossless and animated inputs use the checked full-image path where the layout fits its decoder limits. |

The bounded layouts use extents of at most 65,536 pixels per axis, with smaller limits where the format requires them. EXR bounds include an 8 MiB metadata allocation limit, a 1,048,576-entry offset table and a 64 MiB unpacked chunk limit. Preview workspace is also capped. Unsupported layouts, other presets, additional output sizes, and data/Deep previews use the applicable existing path when its limits allow. If the file fails validation or its plan exceeds a limit, the affected job reports an error before writing a preview.

`--max-decode-mib` (default 512) limits the per-job decode plan. For bounded previews this includes native rows or retained frames, codec scratch, colour and resize buffers, preview output buffers, and ICC reservation where applicable. `--memory-budget-mib` sets the isolated controller's shared admission budget. It covers a fixed reserve for the controller and idle workers plus each admitted job's estimated incremental usage and margin. They are application limits and estimates. `decode_limit_exceeded` means the decode plan did not fit, and `task_memory_exceeds_budget` means the controller could not admit the task. In JSON output, `execution.admission_mib` shows the estimated incremental memory per admitted job.

See [color management](color-cms.md) for OCIO and ICC behavior and [the CLI reference](cli.md) for batch budget options.
