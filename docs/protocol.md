# 批处理与脚本接入

[文档目录](README.md) · [English](en/protocol.md)

将任务保存为 `jobs.json`。相对路径以清单所在目录为基准：

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

`defaults` 设置通用转换选项，任务的 `options` 可以覆盖它；输出可以单独指定 size、format、quality 和 existing。`id` 必须唯一，用来关联返回结果。`sequence_id` 可选，用于[连续帧选择](selection.md)。

混合颜色图和 Cryptomatte 时，可设置 `"defaults": {"visualization": "auto"}`，一次批处理即可按文件选择预览。对已知 AOV，可在任务的 `options` 直接指定模式和通道。接入端可检查 `capabilities --json` 的 `visualization_modes` 是否包含 `auto`。

## 读取结果

`--json` 将 UTF-8 JSON 按行输出到 stdout，诊断文字写到 stderr。每个任务一条 `type: result`，最后一条 `type: summary`。并行时按完成顺序返回，使用 `id` 关联，不要依赖行号。

[EXR 自动选择标记](selection.md#监控自动选择)同时出现在结果的 `warnings` 数组和 stderr 独立行中。统计告警事件时请使用 stderr 中的独立行。

| status | 含义 |
| --- | --- |
| success | 转换成功 |
| partial | 部分输出成功，部分失败 |
| failed | 任务失败，查看 error 或各输出的 error |
| skipped | 按已有文件策略跳过 |
| cancelled | 用户取消，已保存的输出仍保留 |

结果中的 `outputs` 包含各输出状态，`warnings` 给出需要注意的信息。错误处理应读取 `error.code`，不要解析提示文字。`summary.output_counts` 统计所有目标，包括尚未尝试的输出。

## 退出码

| 退出码 | 含义 |
| --- | --- |
| 0 | 完成，允许显式跳过 |
| 1 | 程序内部错误 |
| 2 | 参数或清单无效 |
| 3 | 部分完成，但存在失败 |
| 4 | 全部失败 |
| 130 | 已接受取消请求 |

脚本可结合各输出状态与退出码，决定需要继续处理的文件。已有文件处理见[文件处理](files.md)，并行预算见[命令参考](cli.md)。
