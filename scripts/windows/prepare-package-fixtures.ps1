param([Parameter(Mandatory=$true)][string]$Directory, [string]$SdkCache)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'Use PowerShell 7 (pwsh)' }
$thumbRepo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (!$SdkCache) { $SdkCache = Join-Path $thumbRepo '.deps/windows-sdk' }
$SdkCache = [IO.Path]::GetFullPath($SdkCache)
$Directory = [IO.Path]::GetFullPath($Directory)
if (Test-Path -LiteralPath $Directory) { throw 'Fixture destination must not exist; existing data left unchanged' }
New-Item -ItemType Directory -Force (Split-Path -Parent $Directory) | Out-Null
$thumbVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$thumbVs = & $thumbVswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$thumbVs) { throw 'Visual Studio 2022 x64 C++ tools are required' }
& (Join-Path $thumbVs 'Common7/Tools/Launch-VsDevShell.ps1') -VsInstallationPath $thumbVs -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
$thumbBuild = Join-Path $SdkCache ('fixture-build-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $thumbBuild | Out-Null
$thumbSdk = Join-Path $SdkCache 'sdk'
$thumbSource = Join-Path $PSScriptRoot '../linux/sdk-smoke-fixtures.cpp'
& cl.exe /nologo /std:c++17 /EHsc /MD /O2 /utf-8 "/I$thumbSdk/include" "/I$thumbSdk/include/Imath" "/Fo$thumbBuild/fixture.obj" "/Fe$thumbBuild/fixture.exe" $thumbSource /link "/LIBPATH:$thumbSdk/lib" OpenImageIO.lib OpenImageIO_Util.lib
if ($LASTEXITCODE -ne 0) { throw 'Package fixture tool build failed' }
$env:PATH = "$thumbSdk/bin;" + $env:PATH
& "$thumbBuild/fixture.exe" $Directory
if ($LASTEXITCODE -ne 0) { throw 'Package fixture generation failed' }
$thumbHashes = @{}
foreach ($thumbName in @('synthetic.png','synthetic.exr','synthetic.jpg','synthetic.webp')) {
    $thumbHashes[$thumbName] = (Get-FileHash -LiteralPath (Join-Path $Directory $thumbName) -Algorithm SHA256).Hash.ToLowerInvariant()
}
@{ source_sha256=(Get-FileHash -LiteralPath $thumbSource -Algorithm SHA256).Hash.ToLowerInvariant(); files=$thumbHashes } | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $Directory 'fixture-manifest.json') -Encoding utf8
