<#
.SYNOPSIS
    Trigger one (or more) embedded-RenderDoc frame capture from a running,
    UEVR-injected game and report where the .rdc landed.

.DESCRIPTION
    UEVR watches for a small trigger file at %TEMP%\uevr_renderdoc_capture.req
    while the game runs. This script writes that file, waits for the resulting
    .rdc to appear and finish writing, then prints its path.

    The game must already be running with UEVR + RenderDoc injected (use
    UEVRRenderDocLauncher.exe). You do NOT need RenderDoc installed to capture;
    you only need it (qrenderdoc.exe) to open the .rdc afterwards.

.EXAMPLE
    .\Capture-RenderDoc.ps1

.EXAMPLE
    .\Capture-RenderDoc.ps1 -OutDir "C:\captures\mygame" -Frames 1
#>
param(
    [string]$OutDir = (Join-Path $env:TEMP "uevr_renderdoc"),
    [int]$Frames = 1,
    [int]$TimeoutSeconds = 60
)

$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stamp    = Get-Date -Format "yyyyMMdd_HHmmss"
$template = Join-Path $OutDir "uevr_$stamp"
$prefix   = [System.IO.Path]::GetFileName($template)
$sentinel = Join-Path $env:TEMP "uevr_renderdoc_capture.req"

function Write-Trigger {
    @($template, "frames=$Frames") | Set-Content -LiteralPath $sentinel -Encoding ASCII
}

Write-Trigger
Write-Host "Capture requested." -ForegroundColor Cyan
Write-Host "  trigger : $sentinel"
Write-Host "  template: $template"
Write-Host "Waiting up to $TimeoutSeconds s for the .rdc..."

$deadline  = (Get-Date).AddSeconds($TimeoutSeconds)
$nextRetry = (Get-Date).AddSeconds(3)
$rdc = $null

while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    $rdc = Get-ChildItem -LiteralPath $OutDir -Filter "$prefix*.rdc" -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if ($rdc) { break }

    # The watcher polls every 250 ms; re-arm occasionally in case the game
    # wasn't drawing yet when we first wrote the trigger.
    if ((Get-Date) -ge $nextRetry) { Write-Trigger; $nextRetry = (Get-Date).AddSeconds(3) }
}

if (-not $rdc) {
    Write-Host ""
    Write-Host "No .rdc appeared within $TimeoutSeconds s." -ForegroundColor Red
    Write-Host "Checklist:" -ForegroundColor Yellow
    Write-Host "  * Is the game running AND drawing frames?"
    Write-Host "  * Was it launched via UEVRRenderDocLauncher.exe?"
    Write-Host "  * Check the UEVR log: %APPDATA%\UnrealVRMod\<GameExe>\log.txt (search 'RenderDoc')."
    exit 1
}

# Wait for the file to stop growing so we don't report a half-written capture.
$last = -1
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 600
    $item = Get-Item -LiteralPath $rdc.FullName
    if ($item.Length -gt 0 -and $item.Length -eq $last) { break }
    $last = $item.Length
}

$final = Get-Item -LiteralPath $rdc.FullName
Write-Host ""
Write-Host "Capture complete!" -ForegroundColor Green
Write-Host ("  file: {0}" -f $final.FullName)
Write-Host ("  size: {0:N1} MB" -f ($final.Length / 1MB))
Write-Host ""
Write-Host "Open it in RenderDoc (qrenderdoc.exe) to inspect the frame." -ForegroundColor Cyan
