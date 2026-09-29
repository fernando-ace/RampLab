param(
  [string]$RampLabRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
  [string]$CMakeBin = 'C:\Users\Ferna\AppData\Local\Microsoft\WinGet\Packages\Kitware.CMake_Microsoft.Winget.Source_8wekyb3d8bbwe\cmake-4.4.3-windows-x86_64\bin'
)

$ErrorActionPreference = 'Stop'
$cmakeExe = Join-Path $CMakeBin 'cmake.exe'
if (-not (Test-Path -LiteralPath $cmakeExe)) {
  throw "CMake 4.4.3 was not found at $cmakeExe; pass -CMakeBin with the directory containing cmake.exe."
}
$env:PATH = "$CMakeBin;$env:PATH"
Set-Location -LiteralPath (Join-Path $RampLabRoot 'ros2_ws')
colcon build --merge-install --packages-up-to ramplab_ros2_bridge ramplab_ros2_controller `
  --cmake-args -G 'Visual Studio 18 2026' -A x64 `
  "-DRAMPLAB_SOURCE_DIR=$RampLabRoot" `
  "-DRAMPLAB_BUILD_DIR=$RampLabRoot\build-final-msvc"
if ($LASTEXITCODE -ne 0) { throw "colcon build failed with exit code $LASTEXITCODE" }
