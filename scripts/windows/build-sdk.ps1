param(
    [string]$CacheRoot,
    [string]$Nasm,
    [string]$SourceCache,
    [string]$OiioSource,
    [ValidateRange(1, 32)][int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'Use PowerShell 7 (pwsh) for native build log handling' }
$thumbRepo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (!$CacheRoot) { $CacheRoot = Join-Path $thumbRepo '.deps/windows-sdk' }
$CacheRoot = [IO.Path]::GetFullPath($CacheRoot)
if (!$Nasm) { $Nasm = Join-Path $thumbRepo '.deps/tools/nasm/Library/bin/nasm.exe' }
if (!(Test-Path -LiteralPath $Nasm -PathType Leaf)) { throw 'Restore dependencies/windows-build-tools.explicit.txt, or pass -Nasm' }
$thumbVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$thumbVs = & $thumbVswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$thumbVs) { throw 'Visual Studio 2022 x64 C++ tools are required' }
& (Join-Path $thumbVs 'Common7/Tools/Launch-VsDevShell.ps1') -VsInstallationPath $thumbVs -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
$thumbNinja = Join-Path $thumbVs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
if (!(Test-Path -LiteralPath $thumbNinja)) { throw 'Visual Studio CMake/Ninja component is required' }
$env:PATH = (Split-Path -Parent $Nasm) + ';' + (Split-Path -Parent $thumbNinja) + ';' + $env:PATH
$env:CMAKE_BUILD_PARALLEL_LEVEL = "$Jobs"
$env:CMAKE_GENERATOR = 'Ninja'
$env:GIT_TERMINAL_PROMPT = '0'
# Do not discover the development Conda SDK or a user package registry.
foreach ($thumbVariable in @('CMAKE_PREFIX_PATH','CMAKE_LIBRARY_PATH','CMAKE_INCLUDE_PATH','OpenImageIO_ROOT','nlohmann_json_ROOT')) {
    Remove-Item "Env:$thumbVariable" -ErrorAction SilentlyContinue
}
$thumbLogs = Join-Path $CacheRoot ('logs/run-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Force $thumbLogs | Out-Null
$thumbPrepare = @((Join-Path $PSScriptRoot 'prepare_sdk_sources.py'), '--cache', $CacheRoot)
if ($SourceCache) { $thumbPrepare += @('--source-cache', $SourceCache) }
if ($OiioSource) { $thumbPrepare += @('--oiio-source', $OiioSource) }
& python @thumbPrepare 2>&1 | Tee-Object (Join-Path $thumbLogs 'sources.log')
if ($LASTEXITCODE -ne 0) { throw 'Pinned SDK source preparation failed' }
& python (Join-Path $PSScriptRoot 'sdk_patches.py') "$CacheRoot/oiio-source"
if ($LASTEXITCODE -ne 0) { throw 'Pinned SDK patch application failed' }
$thumbBuildArgs = @('-S', "$CacheRoot/oiio-source", '-B', "$CacheRoot/oiio-build", '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_STANDARD=17', "-DCMAKE_INSTALL_PREFIX=$CacheRoot/sdk",
    "-DOpenImageIO_LOCAL_DEPS_ROOT=$CacheRoot/oiio-deps", "-DOIIO_LOCAL_DEPS_PATH=$CacheRoot/oiio-deps/dist",
    '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF', '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF',
    '-DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=OFF', "-DCMAKE_MAKE_PROGRAM=$thumbNinja",
    "-DCMAKE_C_COMPILER=$((Get-Command cl.exe).Source)", "-DCMAKE_CXX_COMPILER=$((Get-Command cl.exe).Source)",
    '-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /W3', '-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /W3 /GR /EHsc /utf-8',
    '-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG', '-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG',
    "-DGIT_EXECUTABLE=$((Get-Command git.exe).Source)",
    "-DCMAKE_RC_COMPILER=$((Get-Command rc.exe).Source)", "-DCMAKE_MT=$((Get-Command mt.exe).Source)",
    "-DMSVC_REDIST_DIR=$env:VCToolsRedistDir",
    '-DOpenImageIO_BUILD_MISSING_DEPS=all', '-DOpenImageIO_BUILD_LOCAL_DEPS=all',
    '-DOpenImageIO_REQUIRED_DEPS=PNG;WebP', '-DOpenImageIO_DEPENDENCY_BUILD_ALLOW_UNVERIFIED_TAGS=OFF',
    '-DOpenImageIO_DEPENDENCY_DOWNLOAD_RETRIES=1', '-DOpenImageIO_DEPENDENCY_DOWNLOAD_RETRY_DELAY=2',
    '-DOIIO_BUILD_TOOLS=OFF', '-DOIIO_BUILD_TESTS=OFF', '-DBUILD_TESTING=OFF', '-DINSTALL_DOCS=OFF',
    '-DUSE_PYTHON=OFF', '-DUSE_QT=OFF', '-DUSE_SIMD=sse2', '-DOIIO_USE_HWY=OFF', '-DSTOP_ON_WARNING=OFF',
    "-DCMAKE_ASM_NASM_COMPILER=$Nasm")
foreach ($thumbPackage in @('Freetype','OpenCV','TBB','DCMTK','FFmpeg','GIF','Libheif','LibRaw','OpenJPEG','openjph','OpenVDB','Ptex','R3DSDK','Nuke','OpenGL','Qt5','Qt6','JXL','libuhdr')) {
    $thumbBuildArgs += "-DUSE_$thumbPackage=OFF"
}
$thumbBuildArgs = @($thumbBuildArgs | ForEach-Object { $_.Replace([char]92, [char]47) })
@{ arguments=$thumbBuildArgs; source_lock_sha256=(Get-FileHash (Join-Path $thumbRepo 'dependencies/windows-source-sdk.json') -Algorithm SHA256).Hash; recipe_sha256=(Get-FileHash $PSCommandPath -Algorithm SHA256).Hash; nasm_version=(& $Nasm -v); cmake_version=(& cmake --version | Select-Object -First 1) } | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $thumbLogs 'request.json') -Encoding utf8
& cmake @thumbBuildArgs 2>&1 | Tee-Object (Join-Path $thumbLogs 'configure.log')
if ($LASTEXITCODE -ne 0) { throw 'Minimal SDK configure failed; logs retained' }
& cmake --build "$CacheRoot/oiio-build" 2>&1 | Tee-Object (Join-Path $thumbLogs 'build.log')
if ($LASTEXITCODE -ne 0) { throw 'Minimal SDK build failed; logs retained' }
& cmake --install "$CacheRoot/oiio-build" 2>&1 | Tee-Object (Join-Path $thumbLogs 'install.log')
if ($LASTEXITCODE -ne 0) { throw 'Minimal SDK install failed' }
& cmake -S "$CacheRoot/json-source" -B "$CacheRoot/json-build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$CacheRoot/sdk" -DJSON_BuildTests=OFF
if ($LASTEXITCODE -ne 0) { throw 'JSON configure failed' }
& cmake --install "$CacheRoot/json-build"
if ($LASTEXITCODE -ne 0) { throw 'JSON install failed' }
& python (Join-Path $PSScriptRoot 'record_sdk.py') --cache $CacheRoot --request (Join-Path $thumbLogs 'request.json')
if ($LASTEXITCODE -ne 0) { throw 'SDK provenance/SIMD validation failed' }
