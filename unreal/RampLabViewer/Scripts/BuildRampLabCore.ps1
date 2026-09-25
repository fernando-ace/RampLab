$ErrorActionPreference = 'Stop'

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$BuildDirectory = Join-Path $ProjectRoot 'build-unreal-core'

cmake -S $ProjectRoot -B $BuildDirectory -G 'Visual Studio 18 2026' -A x64 -DBUILD_TESTING=OFF
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $BuildDirectory --config Release --parallel
exit $LASTEXITCODE
