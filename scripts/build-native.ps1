param([switch]$WithVips, [switch]$WithProbes)
$ErrorActionPreference = 'Stop'
$thumbRoot = Split-Path -Parent $PSScriptRoot
$thumbSdk = Join-Path $thumbRoot '.deps/oiio/Library'
if (-not (Test-Path -LiteralPath $thumbSdk)) {
    throw 'Native SDK missing. Create .deps/oiio using dependencies/native-win64.explicit.txt first.'
}
$thumbArgs = @('-S', $thumbRoot, '-B', (Join-Path $thumbRoot 'build'),
    '-G', 'Visual Studio 17 2022', '-A', 'x64', "-DCMAKE_PREFIX_PATH=$thumbSdk")
if ($WithVips) {
    $thumbVips = Join-Path $thumbRoot '.deps/vips/Library'
    if (-not (Test-Path -LiteralPath $thumbVips)) { throw 'Create .deps/vips from its explicit lock first.' }
    $thumbArgs += @('-DTHUMBGEN_BUILD_VIPS=ON', "-DVIPS_ROOT=$thumbVips")
} else { $thumbArgs += '-DTHUMBGEN_BUILD_VIPS=OFF' }
if ($WithProbes -or $WithVips) { $thumbArgs += '-DTHUMBGEN_BUILD_PROBES=ON' }
else { $thumbArgs += '-DTHUMBGEN_BUILD_PROBES=OFF' }
& cmake @thumbArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
& cmake --build (Join-Path $thumbRoot 'build') --config Release --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Native build failed' }
