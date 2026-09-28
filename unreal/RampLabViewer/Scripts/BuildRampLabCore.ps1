$ErrorActionPreference = 'Stop'

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$BuildDirectory = Join-Path $ProjectRoot 'build-unreal-core-v143'

cmake -S $ProjectRoot -B $BuildDirectory -G 'Visual Studio 18 2026' -A x64 -T 'v143,version=14.44.35207' -DBUILD_TESTING=OFF
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $BuildDirectory --config Release --target airside_sim airside_autonomy airside_scenario airside_autonomy_scenario --parallel
exit $LASTEXITCODE
