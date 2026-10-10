param(
    [switch]$SkipBuild,
    [string[]]$UnrealArguments = @('-game', '-windowed', '-ResX=1600', '-ResY=900', '-NoSplash')
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$ViewerRoot = Join-Path $ProjectRoot 'unreal\RampLabViewer'
$UProject = Join-Path $ViewerRoot 'RampLabViewer.uproject'
$PowerShell = (Get-Process -Id $PID).Path
$Editor = if ($env:UE_EDITOR) { $env:UE_EDITOR } else { 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' }
$BuildBat = 'C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat'

if (-not (Test-Path -LiteralPath $Editor -PathType Leaf)) { throw "Unreal Editor not found: $Editor (set UE_EDITOR to its executable path)" }
if (-not (Test-Path -LiteralPath $BuildBat -PathType Leaf)) { throw "Unreal Build.bat not found: $BuildBat" }

& $PowerShell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'ValidateCesiumAccess.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Cesium token discovery or asset access validation failed.' }

if (-not (Test-Path -LiteralPath (Join-Path $ViewerRoot 'Plugins\CesiumForUnreal\CesiumForUnreal.uplugin'))) {
    & $PowerShell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'InstallCesium.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Cesium plugin installation failed.' }
}

& $PowerShell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'BuildRampLabCore.ps1')
if ($LASTEXITCODE -ne 0) { throw 'RampLab core build failed.' }

if (-not $SkipBuild) {
    & $BuildBat RampLabViewerEditor Win64 Development "-Project=$UProject" -WaitMutex -NoHotReload
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

& $Editor $UProject @UnrealArguments
exit $LASTEXITCODE
