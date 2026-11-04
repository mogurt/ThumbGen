param([string]$Name = 'CentOS-7.9', [string]$InstallDirectory = 'D:\WSL\CentOS-7.9')
$ErrorActionPreference = 'Stop'
$thumbRepo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$thumbNames = (wsl --list --quiet | Out-String).Replace([string][char]0, '') -split '\r?\n' | ForEach-Object { $_.Trim() }
if ($thumbNames -contains $Name) { throw "Distribution already exists: $Name" }
if (Test-Path -LiteralPath $InstallDirectory) { throw "Installation path already exists: $InstallDirectory" }
& python (Join-Path $PSScriptRoot 'fetch-centos-rootfs.py')
if ($LASTEXITCODE -ne 0) { throw 'Verified rootfs download failed' }
$thumbParent = Split-Path $InstallDirectory -Parent
New-Item -ItemType Directory -Force -Path $thumbParent | Out-Null
& wsl --import $Name $InstallDirectory (Join-Path $thumbRepo '.deps/linux/rootfs/rootfs.tar') --version 2
if ($LASTEXITCODE -ne 0) { throw 'WSL import failed' }
# Deliberately leave the user's default distribution unchanged.
