param(
    [string]$CaptureTemplate = "",
    [int]$Frames = 1,
    [int]$TimeoutSeconds = 60,
    [string]$RenderDocRoot = "E:\Github\renderdoc",
    [string]$RenderDocCmd = ""
)

$ErrorActionPreference = "Stop"

function Wait-CaptureReady([string]$Path, [datetime]$Deadline) {
    $lastLength = -1
    while ((Get-Date) -lt $Deadline) {
        $item = Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue
        if ($null -ne $item -and $item.Length -gt 0) {
            $readable = $false
            $stream = $null
            try {
                $stream = [System.IO.File]::Open(
                    $item.FullName,
                    [System.IO.FileMode]::Open,
                    [System.IO.FileAccess]::Read,
                    [System.IO.FileShare]::Read)
                $readable = $true
            } catch {
                $readable = $false
            } finally {
                if ($null -ne $stream) {
                    $stream.Dispose()
                }
            }

            if ($readable -and $item.Length -eq $lastLength) {
                return $item
            }
            $lastLength = $item.Length
        }
        Start-Sleep -Milliseconds 500
    }

    throw "Capture appeared but was not stable/readable before timeout: $Path"
}

if ([string]::IsNullOrWhiteSpace($CaptureTemplate)) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $CaptureTemplate = Join-Path $env:TEMP "uevr_renderdoc_live\uevr_live_$stamp"
}

$captureDir = Split-Path -Parent $CaptureTemplate
if (![string]::IsNullOrWhiteSpace($captureDir)) {
    New-Item -ItemType Directory -Force -Path $captureDir | Out-Null
}

$sentinel = Join-Path $env:TEMP "uevr_renderdoc_capture.req"
function Write-CaptureRequest() {
    @(
        $CaptureTemplate
        "frames=$Frames"
    ) | Set-Content -LiteralPath $sentinel -Encoding ASCII
}

Write-CaptureRequest

Write-Host "Requested RenderDoc capture via $sentinel"
Write-Host "Template: $CaptureTemplate"

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$nextRetry = (Get-Date).AddSeconds(2)
$newest = $null
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    $candidateDir = if ([string]::IsNullOrWhiteSpace($captureDir)) { "." } else { $captureDir }
    $prefix = [System.IO.Path]::GetFileName($CaptureTemplate)
    $newest = Get-ChildItem -LiteralPath $candidateDir -Filter "$prefix*.rdc" -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1
    if ($null -ne $newest) {
        break
    }

    if ((Get-Date) -ge $nextRetry) {
        Write-CaptureRequest
        $nextRetry = (Get-Date).AddSeconds(2)
    }
}

if ($null -eq $newest) {
    throw "No .rdc appeared for template '$CaptureTemplate' within $TimeoutSeconds seconds. Is UEVRBackend running with RenderDoc watcher enabled?"
}

$newest = Wait-CaptureReady -Path $newest.FullName -Deadline $deadline
Write-Host "Validating capture: $($newest.FullName)"

$validate = Join-Path $PSScriptRoot "validate_renderdoc_capture.ps1"
if ([string]::IsNullOrWhiteSpace($RenderDocCmd)) {
    & $validate -Capture $newest.FullName -RenderDocRoot $RenderDocRoot
} else {
    & $validate -Capture $newest.FullName -RenderDocRoot $RenderDocRoot -RenderDocCmd $RenderDocCmd
}
