# Batch processing and scripting

[Documentation](README.md) · [简体中文](../protocol.md)

Save jobs as `jobs.json`. Relative paths resolve from the manifest's directory:

```json
{
  "schema_version": 1,
  "defaults": {"quality": 90, "existing": "replace"},
  "execution": {"workers": 4, "thread_budget": 4, "memory_budget_mib": 4096},
  "jobs": [{
    "id": "frame-0001",
    "sequence_id": "shot-beauty",
    "input": "frames/beauty.0001.exr",
    "outputs": [
      {"path": "previews/0001.jpg", "size": [960, 960]},
      {"path": "previews/0001.small.jpg", "size": [425, 260]}
    ]
  }]
}
```

```sh
thumbgen batch --manifest jobs.json --json
```

`defaults` provides shared conversion options; each job's `options` can override them. Outputs can set their own size, format, quality and existing-file behavior. Use a unique `id` to match results. Optional `sequence_id` keeps [sequence selection](selection.md) consistent.

Set `"defaults": {"visualization": "auto"}` to select a color, Cryptomatte or [Deep opacity preview](public-deep.md#automatic-opacity-preview) for each eligible file in one batch. An explicit `deep_mode: "reject"` in defaults or job options disables the opacity fallback. When the AOV is known, set its mode and channels directly in the job's `options`. Integrations can check for `auto` in the `visualization_modes` returned by `capabilities --json`.

## Read results

`--json` streams UTF-8 JSON lines to stdout and diagnostics to stderr. Each job produces one `type: result`; the final record is `type: summary`. Concurrent results arrive in completion order, so match by `id`, not line number.

[Automatic EXR selection markers](selection.md#monitor-automatic-selection) appear in the result's `warnings` array and as standalone stderr lines. Use the stderr lines when counting warning events.

| status | Meaning |
| --- | --- |
| success | Conversion completed |
| partial | Some outputs succeeded and others failed |
| failed | Job failed; check the job or output error |
| skipped | Skipped by the existing-file policy |
| cancelled | User cancelled; saved outputs remain |

`outputs` contains per-output status and `warnings` contains notices. Handle errors by `error.code`, not by parsing message text. `summary.output_counts` includes all targets, including outputs not attempted.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Completed, allowing explicit skips |
| 1 | Internal error |
| 2 | Invalid arguments or manifest |
| 3 | Some work completed, with failures |
| 4 | All work failed |
| 130 | Cancellation accepted |

Use output statuses and the exit code to decide which files to process next. See [file handling](files.md) and the [command reference](cli.md) for resource budgets.
