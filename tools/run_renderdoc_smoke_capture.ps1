param(
    [string]$Launcher = "",
    [string]$Smoke = "",
    [string]$CaptureTemplate = "",
    [int]$SmokeSeconds = 25,
    [int]$CaptureTimeoutSeconds = 30,
    [int]$StartupDelaySeconds = 6,
    [string]$RenderDocRoot = "E:\Github\renderdoc",
    [string]$RenderDocCmd = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$binDir = Join-Path $repoRoot "build-renderdoc\bin\uevr"

if ([string]::IsNullOrWhiteSpace($Launcher)) {
    $Launcher = Join-Path $binDir "UEVRRenderDocLauncher.exe"
}
if ([string]::IsNullOrWhiteSpace($Smoke)) {
    $Smoke = Join-Path $binDir "UEVRRenderDocSmoke.exe"
}
if ([string]::IsNullOrWhiteSpace($CaptureTemplate)) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $CaptureTemplate = Join-Path $env:TEMP "uevr_renderdoc_smoke\smoke_$stamp"
}

if (!(Test-Path -LiteralPath $Launcher)) {
    throw "Launcher not found: $Launcher"
}
if (!(Test-Path -LiteralPath $Smoke)) {
    throw "Smoke app not found: $Smoke"
}
$resolvedSmoke = (Resolve-Path -LiteralPath $Smoke).Path

function Stop-SmokeProcess() {
    Get-CimInstance Win32_Process -Filter "Name = 'UEVRRenderDocSmoke.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ExecutablePath -eq $resolvedSmoke } |
        ForEach-Object {
            Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
        }
}

$captureDir = Split-Path -Parent $CaptureTemplate
if (![string]::IsNullOrWhiteSpace($captureDir)) {
    New-Item -ItemType Directory -Force -Path $captureDir | Out-Null
}

$args = @(
    "--exe", $Smoke,
    "--wait",
    "--",
    "--seconds", "$SmokeSeconds"
)

Write-Host "Launching smoke app through UEVRRenderDocLauncher"
Write-Host "Launcher: $Launcher"
Write-Host "Smoke: $Smoke"
$process = Start-Process -FilePath $Launcher -ArgumentList $args -PassThru

try {
    Start-Sleep -Seconds $StartupDelaySeconds

    $captureScript = Join-Path $PSScriptRoot "capture_and_validate_renderdoc.ps1"
    if ([string]::IsNullOrWhiteSpace($RenderDocCmd)) {
        & $captureScript -CaptureTemplate $CaptureTemplate -TimeoutSeconds $CaptureTimeoutSeconds -RenderDocRoot $RenderDocRoot
    } else {
        & $captureScript -CaptureTemplate $CaptureTemplate -TimeoutSeconds $CaptureTimeoutSeconds -RenderDocRoot $RenderDocRoot -RenderDocCmd $RenderDocCmd
    }

    $process.WaitForExit([Math]::Max(1000, ($SmokeSeconds + 10) * 1000)) | Out-Null
    if (!$process.HasExited) {
        throw "Smoke launcher did not exit after validation"
    }
    if ($process.ExitCode -ne 0) {
        throw "Smoke launcher exited with code $($process.ExitCode)"
    }
} finally {
    if ($null -ne $process -and !$process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    Stop-SmokeProcess
}
