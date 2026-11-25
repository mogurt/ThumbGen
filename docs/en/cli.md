# Command reference

[Documentation](README.md) · [简体中文](../cli.md)

## Common commands

| Command | Purpose |
| --- | --- |
| `thumbgen convert INPUT --output FILE` | Make one preview |
| `thumbgen convert INPUT --output-dir DIR --size 425x260 --size 960` | Make multiple sizes |
| `thumbgen batch --manifest jobs.json --json` | Process a batch manifest |
| `thumbgen inspect INPUT --json` | List layers, channels and file information |
| `thumbgen capabilities --json` | List available formats and features |
| `thumbgen --version --json` | Show the version |

## Size and output

| Option | Meaning |
| --- | --- |
| `--size N` / `--size WxH` | Fit within a bounding box without upscaling; default 1024×1024 |
| `--output FILE` | Explicit filename, for a single size |
| `--output-dir DIR` | Multiple sizes; names retain the source extension, e.g. `frame.exr.960x960.jpg` |
| `--format jpeg\|png\|webp` | Output format; must match an explicit filename's extension |
| `--quality N` | JPEG / WebP quality, default 90; WebP output is lossy |
| `--preset fast\|balanced\|quality` | Speed versus detail, default balanced |
| `--background black\|white\|transparent` | Background, default black; JPEG cannot be transparent |
| `--existing error\|skip\|replace` | Existing-file behavior, default error |
| `--json` | Stream JSONL results for scripts |

Previews are 8-bit. PNG stores the preview pixels losslessly; `quality` does not change PNG visual quality.

## Color and layer selection

```sh
thumbgen convert multilayer.exr --output beauty.png --channels Beauty
thumbgen convert frame.exr --output preview.jpg --ocio-config config.ocio --input-space ACEScg --display DISPLAY --view VIEW
```

`--part N` selects a zero-based part index; `--part-name NAME` selects by name. They are mutually exclusive. Use `--selection-policy root_rgb` to prefer top-level RGB. See [layer selection](selection.md).

`--ocio-config` selects a color configuration; `--input-space` declares the input space. Use `--display` and `--view` together. See [color management](color-cms.md).

Data previews use `--visualize`, `--channels`, `--data-range MIN,MAX` and `--invalid-values black|magenta|error`. See [data previews](data-preview.md). For Deep color, use `--deep-mode flatten`; see [Deep EXR](public-deep.md).

Use `--visualize auto` to select a color, Cryptomatte or [Deep opacity preview](public-deep.md#automatic-opacity-preview) in one conversion. With no explicit part or channels, auto mode makes a best-effort choice among ambiguous flat EXR color or grayscale candidates and reports that choice as a warning. Use `--visualize colour` to require a strict color selection. See [layer selection and warning markers](selection.md) for ranking, errors and sequence locks.

## Resource budgets and timeouts

For one conversion:

```sh
thumbgen convert frame.exr --output preview.jpg --thread-budget 2 --memory-budget-mib 2048 --task-timeout-ms 60000
```

For batches, put these settings in the manifest's `execution` object:

```json
{"workers":4,"thread_budget":4,"memory_budget_mib":4096,"startup_timeout_ms":30000,"task_timeout_ms":60000}
```

`workers` defaults to 1. Multiple concurrent jobs require positive thread and memory budgets. `task_timeout_ms` defaults to 0 (unlimited); startup timeout defaults to 30000 ms. `max_decode_mib` limits decode buffers and defaults to 512. Memory budgets guide scheduling; they are not an operating-system-enforced process memory cap.

See [batch processing and scripting](protocol.md) for a complete manifest, results and exit codes.
