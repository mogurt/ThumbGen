# 安装与运行

[文档目录](README.md) · [English](en/installation.md)

使用适合平台的完整 ThumbGen 程序目录。Windows 需要保留随附 DLL；Linux 需要保留 bin/lib 相对位置和执行权限。不要只复制单个可执行文件。

下载匹配平台的 `thumbgen-0.1.6-*.zip` 或 `.tar.gz`，先核对随附的 `SHA256SUMS`。两个压缩包都只有固定的 `ThumbGen/` 外层目录，里面直接是 `bin/`、`share/`、`licenses/`，Linux 另有 `lib/`；目录名不含版本或平台，无需解压后改名或移动。

Windows 解压到 `%LOCALAPPDATA%/Programs/`，得到 `%LOCALAPPDATA%/Programs/ThumbGen/bin/thumbgen.exe`。PowerShell 可运行：

```powershell
& "$env:LOCALAPPDATA/Programs/ThumbGen/bin/thumbgen.exe" --version --json
```

Linux 解压到 `$HOME/.local/opt/`，得到 `$HOME/.local/opt/ThumbGen/bin/thumbgen`：

```sh
mkdir -p "$HOME/.local/opt"
tar -xzf thumbgen-0.1.6-linux-x86_64-glibc217.tar.gz -C "$HOME/.local/opt"
"$HOME/.local/opt/ThumbGen/bin/thumbgen" --version --json
```

解压目标是安装目录的**父目录**，不要再套一层 `ThumbGen/`。可直接用完整路径调用；压缩包不会自动安装或修改 PATH。

运行时不需要 Python，也不需要单独安装图像处理软件。缺少动态库时，请检查是否完整解压，或重新获取匹配平台的程序包。

使用 `thumbgen capabilities --json` 查看当前程序可用格式。随后按[快速上手](getting-started.md)转换第一张图。

应用图标：Windows 的图标已嵌入 `thumbgen.exe`，独立 ICO 和原始 PNG 随包放在 `share/ThumbGen/branding/`。Linux 包提供 `share/icons/hicolor/<尺寸>/apps/thumbgen.png`（16–512 像素）；仅解压程序包不会自动注册到系统图标主题，桌面集成时可将这些文件安装到用户或系统的同名图标目录。ThumbGen 是命令行工具，不附带桌面启动项。
