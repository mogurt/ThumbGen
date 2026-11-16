# 取消任务

[文档目录](README.md) · [English](en/cancellation.md)

在终端按 Ctrl+C 可以取消转换或批处理。Windows 也支持 Ctrl+Break；Linux 支持 SIGINT 和 SIGTERM。

取消后停止开始新任务，已经保存的输出会保留，未完成的输出标记为 `cancelled`。带 `--json` 时会尽量补齐结果与汇总，进程退出码为 130。

重新运行时，可以用 `--existing skip` 接着处理剩余文件；如果输入或参数已经改变，请选择 replace。详见[文件处理](files.md)。

强制结束进程、关闭控制台或系统关机可能留下临时文件，也可能无法输出完整结果。需要限制单个任务时长时，使用 `--task-timeout-ms`；超时后会继续后续任务。
