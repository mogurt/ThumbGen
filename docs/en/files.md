# Existing files and reruns

[Documentation](README.md) · [简体中文](../files.md)

| Option | Behavior |
| --- | --- |
| `--existing error` | Default: report an error for an existing file |
| `--existing skip` | Keep existing files and skip them |
| `--existing replace` | Replace the old file after the new image finishes encoding |

```sh
thumbgen convert image.exr --output preview.jpg --existing replace
```

Skipping does not check whether an old preview matches your current settings. Use replace when inputs or color settings have changed.

## Interruptions and failures

An unfinished encode does not replace the old file with an incomplete image. Multiple sizes are saved independently: a later failure does not delete an earlier completed output.

An input still being written or changed during processing may produce `source_changed`. Wait for the source file to finish writing before running the conversion again.

On Windows, another program holding the target open can prevent replacement. Close that program and try again. See [cancellation](cancellation.md) and [batch results](protocol.md).
