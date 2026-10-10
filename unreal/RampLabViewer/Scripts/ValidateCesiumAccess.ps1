$ErrorActionPreference = 'Stop'

function Read-TokenFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    foreach ($Line in Get-Content -LiteralPath $Path) {
        $Entry = $Line.Trim()
        if ($Entry.StartsWith('export ')) { $Entry = $Entry.Substring(7).Trim() }
        if ($Entry -notmatch '^RAMPLAB_CESIUM_ION_TOKEN\s*=') { continue }
        $Value = ($Entry -split '=', 2)[1].Trim()
        if ($Value.Length -ge 2 -and (($Value.StartsWith('"') -and $Value.EndsWith('"')) -or ($Value.StartsWith("'") -and $Value.EndsWith("'")))) {
            $Value = $Value.Substring(1, $Value.Length - 2)
        } else {
            $Value = ($Value -split '#', 2)[0].Trim()
        }
        if ($Value -and $Value -notin @('your_token_here', '<your-token>')) { return $Value }
        return $null
    }
    return $null
}

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$GitCommonDir = (& git -C $ProjectRoot rev-parse --path-format=absolute --git-common-dir 2>$null).Trim()
$PrimaryRoot = if ($GitCommonDir) { Split-Path -Parent $GitCommonDir } else { $ProjectRoot }
$Candidates = @(
    @{ Path = (Join-Path $ProjectRoot '.local.env'); Source = 'current checkout local environment' },
    @{ Path = (Join-Path $PrimaryRoot '.local.env'); Source = 'primary checkout local environment' }
)
$Token = $null
$Source = $null
foreach ($Candidate in $Candidates) {
    $Token = Read-TokenFile $Candidate.Path
    if ($Token) { $Source = $Candidate.Source; break }
}
if (-not $Token -and $env:RAMPLAB_CESIUM_ION_TOKEN) { $Token = $env:RAMPLAB_CESIUM_ION_TOKEN; $Source = 'process environment' }
if (-not $Token) {
    $SecureCandidates = @(
        @{ Path = (Join-Path $ProjectRoot 'unreal\RampLabViewer\.env.local'); Source = 'current project local environment' },
        @{ Path = (Join-Path $ProjectRoot 'unreal\RampLabViewer\Config\CesiumIon.local.ini'); Source = 'current secure Cesium configuration' },
        @{ Path = (Join-Path $PrimaryRoot 'unreal\RampLabViewer\.env.local'); Source = 'primary checkout local environment' },
        @{ Path = (Join-Path $PrimaryRoot 'unreal\RampLabViewer\Config\CesiumIon.local.ini'); Source = 'primary secure Cesium configuration' }
    )
    foreach ($Candidate in $SecureCandidates) {
        $Token = Read-TokenFile $Candidate.Path
        if ($Token) { $Source = $Candidate.Source; break }
    }
}
if (-not $Token) {
    Write-Output 'Cesium ion token: missing'
    exit 2
}

Write-Output 'Cesium ion token: loaded'
Write-Output "Cesium ion token source: $Source"
Add-Type -AssemblyName System.Net.Http
$Client = New-Object System.Net.Http.HttpClient
$Client.Timeout = [TimeSpan]::FromSeconds(25)
$Client.DefaultRequestHeaders.Authorization = [System.Net.Http.Headers.AuthenticationHeaderValue]::new('Bearer', $Token)
$Token = $null
$Failures = 0
foreach ($Asset in @(1, 2)) {
    $Response = $null
    try {
        $Response = $Client.GetAsync("https://api.cesium.com/v1/assets/$Asset").GetAwaiter().GetResult()
        $StatusCode = [int]$Response.StatusCode
        Write-Output "Cesium asset $Asset permission check: HTTP $StatusCode"
        if ($StatusCode -lt 200 -or $StatusCode -ge 300) { $Failures++ }
    } catch {
        Write-Output "Cesium asset $Asset permission check: network error"
        $Failures++
    }
    if ($Response) { $Response.Dispose(); $Response = $null }
}
$Client.Dispose()
if ($Failures -gt 0) { exit 3 }
