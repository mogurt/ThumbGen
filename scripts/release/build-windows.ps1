param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Output,
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$Commit
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$Source = [IO.Path]::GetFullPath($Source)
$Output = [IO.Path]::GetFullPath($Output)
$work = Join-Path $env:RUNNER_TEMP 'thumbgen-release'
New-Item -ItemType Directory -Path $work | Out-Null
$sdk = "$work/sdk"
$color = "$work/color"
$bounded = "$work/bounded"

# The published notice bundle covers exactly these pinned dependency inputs.
git -C $Source diff --quiet v0.1.1 $Commit -- dependencies scripts/build-color-sdk.py scripts/dependencies
$conda = Join-Path $env:CONDA 'Scripts/conda.exe'
& $conda create --yes --copy --prefix "$work/tools" --file "$Source/dependencies/windows-build-tools.explicit.txt"
& "$Source/scripts/windows/build-sdk.ps1" -CacheRoot $sdk -Nasm "$work/tools/Library/bin/nasm.exe"
$overlays = "$Source/scripts/dependencies/build_verified_overlay.py"
python $overlays --kind color --root $color --jpeg-cache "$sdk/oiio-deps/libjpeg-turbo"
python $overlays --kind bounded --root $bounded --source-cache "$sdk/oiio-deps/OpenEXR" --sdk $sdk

$prefix = "$color/sdk;$bounded/prefix;$sdk/sdk;$sdk/oiio-deps/dist"
cmake -S $Source -B "$work/product" -G 'Visual Studio 17 2022' -A x64 `
    -DBUILD_TESTING=OFF -DTHUMBGEN_VERIFY_DEPENDENCY_VERSIONS=ON "-DCMAKE_PREFIX_PATH=$prefix" `
    "-DOpenEXR_DIR=$bounded/prefix/lib/cmake/OpenEXR" "-DCMAKE_INSTALL_PREFIX=$work/install"
cmake --build "$work/product" --config Release --parallel 4
cmake --install "$work/product" --config Release
gh release download v0.1.1 --repo $env:GITHUB_REPOSITORY --dir $work --pattern 'thumbgen-0.1.1-windows-x64.zip'
python "$PSScriptRoot/assemble.py" windows --source $Source --install "$work/install" --sdk $sdk `
    --color "$color/sdk" --bounded "$bounded/prefix" --candidate "$work/thumbgen-0.1.1-windows-x64.zip" `
    --output $Output --version $Version --commit $Commit
python "$PSScriptRoot/verify.py" "$Output/thumbgen-$Version-windows-x64.zip" --version $Version --commit $Commit
