# Chart data

| File | Contents |
| --- | --- |
| [timings.csv](timings.csv) | Processing time, memory and output size |
| [aggregates.json](aggregates.json) | Values used by the charts |
| [coverage.json](coverage.json) | Image support and limitations |
| [issue-timings.json](issue-timings.json) | Processing time for outputs with color or canvas issues |
| [settings.json](settings.json) | Hardware, software versions and output settings |
| [public-assets.json](public-assets.json) | Image downloads, authors, licenses and checksums |

[Images and settings](../en/benchmarks.md) · [中文说明](../benchmarks.md)

Download the source images:

```sh
python benchmarks/five_tool_study/fetch_assets.py --output comparison-inputs
```

Synthetic images use the [PNG generator](../../benchmarks/five_tool_study/generate_assets.py). Conversion options are in the [tool adapters](../../benchmarks/five_tool_study/adapters.py).

Redraw the charts with Matplotlib:

```sh
python scripts/public/render_benchmarks.py
```
