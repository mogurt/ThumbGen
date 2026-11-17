# Installation

[Documentation](README.md) · [简体中文](../installation.md)

Use the complete ThumbGen folder for your platform. Keep the supplied DLLs on Windows; preserve the bin/lib layout and executable permissions on Linux. Do not copy only the executable.

Add the program directory to PATH or use its full path. In PowerShell, run `& "C:/Tools/ThumbGen/thumbgen.exe" --version --json`; on Linux, run `./bin/thumbgen --version --json`.

Python and separate image-processing applications are not needed at runtime. For missing-library errors, check that extraction completed and the package matches your platform.

Run `thumbgen capabilities --json` to see available formats, then follow [getting started](getting-started.md) to convert your first image.

Application icons: Windows embeds the icon in `thumbgen.exe`; a standalone ICO and the original PNG are included in `share/ThumbGen/branding/`. Linux packages include `share/icons/hicolor/<size>/apps/thumbgen.png` (16–512 pixels). Extracting an archive does not register these with the system icon theme; desktop integrations can install them into the matching user or system icon directories. ThumbGen is a command-line tool and does not ship a desktop launcher.
