# 命令参考

[文档目录](README.md) · [English](en/cli.md)

## 常用命令

| 命令 | 用途 |
| --- | --- |
| `thumbgen convert INPUT --output FILE` | 生成单张预览 |
| `thumbgen convert INPUT --output-dir DIR --size 425x260 --size 960` | 一次生成多个尺寸 |
| `thumbgen batch --manifest jobs.json --json` | 按清单批量转换 |
| `thumbgen inspect INPUT --json` | 查看图层、通道和文件信息 |
| `thumbgen capabilities --json` | 查看可用格式和功能 |
| `thumbgen --version --json` | 查看版本 |

## 尺寸与输出

| 参数 | 说明 |
| --- | --- |
| `--size N` / `--size WxH` | 等比适配边界框，不放大小图；默认 1024×1024 |
| `--output FILE` | 显式文件名，只能搭配一个尺寸 |
| `--output-dir DIR` | 生成多个尺寸，文件名保留源扩展名，如 `frame.exr.960x960.jpg` |
| `--format jpeg\|png\|webp` | 输出格式；显式文件名的扩展名须匹配 |
| `--quality N` | JPEG / WebP 质量，默认 90；WebP 为有损输出 |
| `--preset fast\|balanced\|quality` | 速度与细节取舍，默认 balanced |
| `--background black\|white\|transparent` | 背景，默认 black；JPEG 不支持 transparent |
| `--existing error\|skip\|replace` | 已有文件处理方式，默认 error |
| `--json` | 将结果按 JSONL 输出，便于脚本读取 |

预览为 8-bit 输出。PNG 无损保存预览像素，`quality` 不影响 PNG 的视觉质量。

## 颜色与图像选择

```sh
thumbgen convert multilayer.exr --output beauty.png --channels Beauty
thumbgen convert frame.exr --output preview.jpg --ocio-config config.ocio --input-space ACEScg --display DISPLAY --view VIEW
```

`--part N` 使用零基 part 索引，`--part-name NAME` 按名称选择，二者互斥。`--selection-policy root_rgb` 可优先顶层 RGB。详见[图层选择](selection.md)。

`--ocio-config` 选择色彩配置，`--input-space` 指定输入空间；`--display` 和 `--view` 要成对使用。详见[色彩管理](color-cms.md)。

数据预览使用 `--visualize`、`--channels`、`--data-range MIN,MAX` 和 `--invalid-values black|magenta|error`，详见[数据预览](data-preview.md)。Deep 颜色使用 `--deep-mode flatten`，详见[Deep EXR](public-deep.md)。

使用 `--visualize auto` 在一次转换中选择颜色或 Cryptomatte 预览。默认策略优先选择明确命名的主画面，其次优先选择各 part 内的顶层 RGB。若未指定 part 或通道，且多个 EXR part 仍有颜色候选，自动模式可在其余候选都是阴影、环境光遮蔽或 Fresnel 等可识别的辅助通道组时，选用唯一一组无前缀的 RGB 或 RGBA。

没有颜色候选时，自动模式可以跨 part 预览单个 Cryptomatte 家族。默认选择策略下，多个家族中唯一的 `crypto_object` 或 `CryptoObject` 优先（不区分大小写）。无法消除歧义或所选布局无效时会报错。指定 part、通道或 Deep 展平时，自动模式遵循显式选择。色彩管理选项只用于颜色预览。

## 执行预算与时限

单文件转换可以限制线程、内存预算与处理时间：

```sh
thumbgen convert frame.exr --output preview.jpg --thread-budget 2 --memory-budget-mib 2048 --task-timeout-ms 60000
```

批处理在清单的 `execution` 中设置：

```json
{"workers":4,"thread_budget":4,"memory_budget_mib":4096,"startup_timeout_ms":30000,"task_timeout_ms":60000}
```

`workers` 默认为 1；多个并行任务需要同时设置正数线程预算和内存预算。`task_timeout_ms` 默认 0，表示不限时；启动时限默认 30000 毫秒。`max_decode_mib` 限制解码缓冲区，默认 512。内存预算用于安排并行任务，不是操作系统强制的进程内存上限。

完整的清单示例、结果和退出码见[批处理与脚本接入](protocol.md)。
