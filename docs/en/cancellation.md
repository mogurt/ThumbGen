# Cancelling jobs

[Documentation](README.md) · [简体中文](../cancellation.md)

Press Ctrl+C in the terminal to cancel a conversion or batch. Windows also supports Ctrl+Break; Linux supports SIGINT and SIGTERM.

ThumbGen stops starting new work, keeps saved outputs and marks unfinished outputs as `cancelled`. With `--json`, it attempts to finish the result stream and summary. The exit code is 130.

Rerun with `--existing skip` to continue remaining files. If inputs or settings have changed, choose replace instead. See [file handling](files.md).

Force-killing the process, closing the console or shutting down the system may leave temporary files or incomplete result records. To limit individual job duration, use `--task-timeout-ms`; later jobs can continue after a timeout.
