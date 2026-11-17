# 已有文件与重跑

[文档目录](README.md) · [English](en/files.md)

| 参数 | 行为 |
| --- | --- |
| `--existing error` | 默认选项，遇到已有文件时报错 |
| `--existing skip` | 保留已有文件并跳过 |
| `--existing replace` | 新图编码完成后替换旧文件 |

```sh
thumbgen convert image.exr --output preview.jpg --existing replace
```

跳过不会确认旧图是否对应当前参数。源图或色彩设置有变化时，请使用 replace。

## 转换中断或失败

编码未完成时不会用半张新图覆盖旧文件。一个输入生成多个尺寸时，各输出独立保存：后面的输出失败，不会删除前面已经完成的图。

源文件正在写入或处理中发生变化时，可能返回 `source_changed`。请等源文件写入完成后重新运行。

Windows 下，目标被其他程序占用可能导致替换失败。关闭占用程序后再试。详见[取消处理](cancellation.md)和[批处理结果](protocol.md)。
