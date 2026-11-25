# Layers and image sequences

[Documentation](README.md) · [简体中文](../selection.md)

First inspect the channels and parts:

```sh
thumbgen inspect multilayer.exr --json
```

## Pick the main image

The default selection policy prefers clearly named Beauty, Combined or RGBA images, then top-level RGB within each part. `--visualize colour`, the CLI default, requires an explicit choice when the selection remains ambiguous. Selection uses channel metadata, so a black frame keeps the same mapping.

```sh
thumbgen convert multilayer.exr --output beauty.jpg --channels Beauty
thumbgen convert multilayer.exr --output rgb.jpg --channels RGB
thumbgen convert multipart.exr --output preview.jpg --part-name beauty
```

`--part N` selects a zero-based index and cannot be combined with `--part-name`. Use [data previews](data-preview.md) for depth, normals and IDs.

With `--visualize auto` and no explicit part or channels, ThumbGen makes a best-effort choice among flat EXR color or grayscale candidates. It first applies the requested selection policy. Under the default policy, it can select a unique unprefixed RGB(A) group without a warning when the other candidates are recognized auxiliary AOVs such as shadow, ambient occlusion or Fresnel. If multiple candidates still remain, it chooses the unique unprefixed RGB(A) group if there is one, or the first eligible candidate in part and group order. A fallback warning means the chosen view may differ from the intended view. Check `selection.candidates` and `selection.chosen.reason`, or set the part and channels explicitly for a known view.

Without a color candidate, automatic mode can preview a Cryptomatte family across flat parts. Under the default policy, a unique `crypto_object` or `CryptoObject` family (case-insensitive) takes precedence over other families. Auto can also select a unique [Deep opacity mapping](public-deep.md#automatic-opacity-preview) when `deep_mode` is omitted. If automatic selection cannot resolve a supported mapping, the job fails with its original error. Data-only inputs require their dedicated visualization, and unknown layouts require inspection or an upstream conversion. Color management, decoding and output errors retain their own diagnostics. ThumbGen does not switch mappings or retry another candidate after a failure.

### Monitor automatic selection

Both text and JSONL CLI modes write one standalone stderr line per result that reports a fallback or unresolved selection. JSONL also carries the same string in the result's `warnings` array. Count the standalone stderr lines or their warning-level log entries so JSON mirrors do not inflate counts.

```text
TG_EXR_AUTO_SELECTION_FALLBACK selected_part=1 selected_group="RGB" candidate_count=3
TG_EXR_AUTO_SELECTION_UNRESOLVED error_code=visualization_required
TG_EXR_AUTO_DEEP_OPACITY selected_part=0 preview=grayscale_coverage
```

`selected_part` is zero-based, `selected_group` is a JSON string, and `candidate_count` counts the eligible choices after the selection policy. The opacity marker records the selected mapping once per job, including jobs with multiple outputs or a subsequent decode or output error.

```sh
grep -E '^TG_EXR_AUTO_(SELECTION_(FALLBACK|UNRESOLVED)|DEEP_OPACITY) ' thumbgen.stderr.log
```

For supported PSD v1 files, automatic color selection uses the stored composite when you omit both part and channel group. You can also select the composite directly with `--part 0` and optionally specify a channel group. For Raw and RLE files, `inspect` still lists available layers and you can select one explicitly by index or name. For ZIP and ZIP with prediction, `inspect` shows the supported composite only.

## Keep sequences consistent

Give related batch jobs the same `sequence_id` to keep their selected layers and channels consistent. The first frame with an eligible selection can establish the lock, including a fallback selection. The lock takes precedence on later frames. A later frame with a missing or changed mapping reports an error.

Use separate IDs for different cameras, views and render layers. The selection lasts only for that batch, not across program runs. Use named parts for multipart sequences.

See [batch processing](protocol.md).
