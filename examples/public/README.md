# 批处理示例

这些清单演示多尺寸、连续帧、数据通道和 Cryptomatte 的常见用法。先把各清单的 input 改成自己的素材路径，再设置 outputs 中的保存位置。相对路径以清单目录为基准。

| 清单 | 用途 |
| --- | --- |
| [普通图片](colour.jobs.json) | 同一图片生成两个尺寸 |
| [连续帧](sequence.jobs.json) | 固定同一序列的主画面选择 |
| [数据通道](data.jobs.json) | 深度、法线、矢量与 ID 预览 |
| [Cryptomatte](cryptomatte.jobs.json) | 完整家族的覆盖率总览 |

```sh
thumbgen batch --manifest examples/public/colour.jobs.json --json
```

默认遇到已有目标文件会报错。重复运行可将 defaults.existing 改为 skip 或 replace，详见[文件处理](../../docs/files.md)。

找不到示例中的通道时，先用 `thumbgen inspect INPUT --json` 查看实际名称。公开素材可从[示例素材](../../docs/public-assets.md)中获取；完整参数见[批处理指南](../../docs/protocol.md)。
