$ErrorActionPreference = 'Stop'

# Cesium's official 2.29.1 release uses the "57" package for both UE 5.7 and 5.8.
$version = '2.29.1'
$packageName = 'CesiumForUnreal-57-v2.29.1.zip'
$expectedSha256 = 'fd9779095ef314a26bd3c24feff2807a65ffe56e0e25b0b926f70f3124c59e35'
$downloadUrl = "https://github.com/CesiumGS/cesium-unreal/releases/download/v$version/$packageName"
$projectRoot = Split-Path -Parent $PSScriptRoot
$pluginsRoot = Join-Path $projectRoot 'Plugins'
$destination = Join-Path $pluginsRoot 'CesiumForUnreal'

if (Test-Path -LiteralPath (Join-Path $destination 'CesiumForUnreal.uplugin')) {
    Write-Host "Cesium for Unreal $version is already installed at $destination"
    exit 0
}

$workRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("RampLab-CesiumInstall-" + [Guid]::NewGuid().ToString('N'))
$archive = Join-Path $workRoot $packageName
$extractRoot = Join-Path $workRoot 'Extracted'
New-Item -ItemType Directory -Force -Path $workRoot, $pluginsRoot | Out-Null

Write-Host "Downloading official Cesium for Unreal $version package..."
& curl.exe --fail --location --retry 3 --output $archive $downloadUrl
if ($LASTEXITCODE -ne 0) { throw "Cesium download failed with exit code $LASTEXITCODE" }

$actualSha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualSha256 -ne $expectedSha256) {
    throw "Cesium archive SHA-256 mismatch. Expected $expectedSha256, received $actualSha256"
}

Expand-Archive -LiteralPath $archive -DestinationPath $extractRoot
$source = Get-ChildItem -LiteralPath $extractRoot -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'CesiumForUnreal.uplugin') } |
    Select-Object -First 1
if ($null -eq $source) { throw 'The release archive did not contain CesiumForUnreal.uplugin at its top level.' }

Move-Item -LiteralPath $source.FullName -Destination $destination
Write-Host "Installed Cesium for Unreal $version at $destination"

$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$resolvedWorkRoot = [System.IO.Path]::GetFullPath($workRoot)
if ($resolvedWorkRoot.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    [System.IO.Directory]::Delete($resolvedWorkRoot, $true)
}
