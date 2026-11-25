# 选择图层与连续帧

[文档目录](README.md) · [English](en/selection.md)

先查看文件中的通道和 part：

```sh
thumbgen inspect multilayer.exr --json
```

## 指定主画面

默认策略优先 Beauty、Combined、RGBA 等明确命名的主画面，其次优先各 part 内的顶层 RGB。命令行默认使用 `--visualize colour`，候选仍有歧义时须手动指定。选择依据是通道元数据，画面全黑也会沿用同一映射。

```sh
thumbgen convert multilayer.exr --output beauty.jpg --channels Beauty
thumbgen convert multilayer.exr --output rgb.jpg --channels RGB
thumbgen convert multipart.exr --output preview.jpg --part-name beauty
```

`--part N` 也可按从 0 开始的索引选择，但不能与 `--part-name` 同时使用。深度、法线、ID 等数据通道请使用[数据预览](data-preview.md)。

使用 `--visualize auto` 且未指定 part 或通道时，ThumbGen 尽力从平面 EXR 的颜色或灰度候选中选出预览。它先应用指定的选择策略。默认策略下，如果其他候选都是阴影、环境光遮蔽或 Fresnel 等已识别的辅助 AOV，可直接使用唯一一组无前缀 RGB(A)，无需告警。如果仍有多个候选，则优先使用唯一一组无前缀 RGB(A)，否则按现有 part 和通道组顺序使用第一个合适的候选。出现回退告警时，预览可能与预期画面不同。可查看 `selection.candidates` 和 `selection.chosen.reason`，或显式指定所需画面的 part 和通道组。

没有颜色候选时，自动模式可跨平面 part 预览 Cryptomatte 家族。默认策略优先多个家族中唯一的 `crypto_object` 或 `CryptoObject`，名称不区分大小写。省略 `deep_mode` 时，自动模式也可选择唯一的 [Deep 不透明度映射](public-deep.md)。自动模式无法选出受支持的映射时，任务以原错误失败。纯数据输入需要专用预览模式，未知布局需要检查通道或在上游转换。色彩管理、解码和输出失败沿用各自的错误诊断。失败后不会切换映射或尝试其他候选。

### 监控自动选择

文本和 JSONL 模式都会为发生回退或无法选择的任务在标准错误中输出一条独立告警。JSONL 结果的 `warnings` 数组中也包含同一字符串。统计时请匹配标准错误中的独立告警行或对应的警告级别日志，避免重复统计 JSON 副本。

```text
TG_EXR_AUTO_SELECTION_FALLBACK selected_part=1 selected_group="RGB" candidate_count=3
TG_EXR_AUTO_SELECTION_UNRESOLVED error_code=visualization_required
TG_EXR_AUTO_DEEP_OPACITY selected_part=0 preview=grayscale_coverage
```

`selected_part` 是从 0 开始的索引，`selected_group` 是 JSON 字符串，`candidate_count` 是应用选择策略后剩余的合格候选数。不透明度告警每个任务记录一次，即使任务有多个输出，或在选择后出现解码或输出错误。

```sh
grep -E '^TG_EXR_AUTO_(SELECTION_(FALLBACK|UNRESOLVED)|DEEP_OPACITY) ' thumbgen.stderr.log
```

受支持的 PSD v1 文件在未指定 part 或通道组、自动选择颜色图像时使用存储的合成图。也可使用 `--part 0` 直接选择合成图，按需附加通道组。Raw 和 RLE 文件的 `inspect` 仍会列出可用图层，可按索引或名称显式选择。ZIP 和带预测的 ZIP 文件的 `inspect` 仅显示受支持的合成图。

## 连续帧保持一致

在批处理任务中使用相同的 `sequence_id`，即可固定同一组图层与通道。首个选出合格映射的帧可建立锁定，回退选出的映射也可以锁定。后续帧优先沿用锁定映射，映射缺失或变更时报告错误。

不同相机、视图或渲染层请使用不同的 `sequence_id`。它只在同一批任务中生效，不会记住上一次运行的选择。多 part 序列请使用命名 part。

参见[批处理](protocol.md)。
