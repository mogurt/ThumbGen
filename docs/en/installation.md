# Installation

[Documentation](README.md) · [简体中文](../installation.md)

Use the complete ThumbGen folder for your platform. Keep the supplied DLLs on Windows; preserve the bin/lib layout and executable permissions on Linux. Do not copy only the executable.

Download the matching `thumbgen-0.1.6-*.zip` or `.tar.gz` and check the accompanying `SHA256SUMS`. Both archives contain one fixed `ThumbGen/` directory, with `bin/`, `share/`, `licenses/` and, on Linux, `lib/` immediately underneath. The directory name is the same on both platforms and across versions, so you can use it directly after extraction.

On Windows, extract into `%LOCALAPPDATA%/Programs/` to get `%LOCALAPPDATA%/Programs/ThumbGen/bin/thumbgen.exe`. In PowerShell:

```powershell
& "$env:LOCALAPPDATA/Programs/ThumbGen/bin/thumbgen.exe" --version --json
```

On Linux, extract into `$HOME/.local/opt/` to get `$HOME/.local/opt/ThumbGen/bin/thumbgen`:

```sh
mkdir -p "$HOME/.local/opt"
tar -xzf thumbgen-0.1.6-linux-x86_64-glibc217.tar.gz -C "$HOME/.local/opt"
"$HOME/.local/opt/ThumbGen/bin/thumbgen" --version --json
```

Extract into the installation directory's **parent**, avoiding `ThumbGen/ThumbGen/`. Use the full executable path; the archives do not install automatically or modify PATH.

Python and separate image-processing applications are not needed at runtime. For missing-library errors, check that extraction completed and the package matches your platform.

Run `thumbgen capabilities --json` to see available formats, then follow [getting started](getting-started.md) to convert your first image.

Application icons: Windows embeds the icon in `thumbgen.exe`; a standalone ICO and the original PNG are included in `share/ThumbGen/branding/`. Linux packages include `share/icons/hicolor/<size>/apps/thumbgen.png` (16–512 pixels). Extracting an archive does not register these with the system icon theme; desktop integrations can install them into the matching user or system icon directories. ThumbGen is a command-line tool and does not ship a desktop launcher.
