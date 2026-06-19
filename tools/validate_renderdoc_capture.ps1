param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Capture,

    [string]$RenderDocRoot = "E:\Github\renderdoc",
    [string]$Platform = "x64",
    [string]$Configuration = "Development",
    [string]$OutDir = "",
    [string]$RenderDocCmd = ""
)

$ErrorActionPreference = "Stop"

function Resolve-ExistingPath([string[]]$Candidates) {
    foreach ($candidate in $Candidates) {
        if (![string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

$capturePath = Resolve-ExistingPath @($Capture)
if ($null -eq $capturePath) {
    throw "Capture not found: $Capture"
}
if ([System.IO.Path]::GetExtension($capturePath).ToLowerInvariant() -ne ".rdc") {
    throw "Expected a .rdc capture, got: $capturePath"
}

$renderdoccmdPath = $RenderDocCmd
if ([string]::IsNullOrWhiteSpace($renderdoccmdPath)) {
    $renderdoccmdPath = Resolve-ExistingPath @(
        (Join-Path $RenderDocRoot "$Platform\$Configuration\renderdoccmd.exe"),
        (Join-Path $RenderDocRoot "$Platform\Development\renderdoccmd.exe"),
        (Join-Path $RenderDocRoot "$Platform\Release\renderdoccmd.exe"),
        "C:\Program Files\RenderDoc\renderdoccmd.exe",
        "C:\Program Files (x86)\RenderDoc\renderdoccmd.exe"
    )
} elseif (Test-Path -LiteralPath $renderdoccmdPath) {
    $renderdoccmdPath = (Resolve-Path -LiteralPath $renderdoccmdPath).Path
}

if ($null -eq $renderdoccmdPath -or !(Test-Path -LiteralPath $renderdoccmdPath)) {
    throw "renderdoccmd.exe not found. Pass -RenderDocCmd or -RenderDocRoot."
}

if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $base = [System.IO.Path]::GetFileNameWithoutExtension($capturePath)
    $OutDir = Join-Path $env:TEMP "uevr_renderdoc_validate\$base`_$stamp"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$logPath = Join-Path $OutDir "renderdoccmd_index_capture.log"
$output = & $renderdoccmdPath index-capture --out $OutDir $capturePath 2>&1
$exitCode = $LASTEXITCODE
$output | Set-Content -LiteralPath $logPath -Encoding UTF8

$result = [ordered]@{
    ok = ($exitCode -eq 0)
    capture = $capturePath
    renderdoccmd = $renderdoccmdPath
    out_dir = (Resolve-Path -LiteralPath $OutDir).Path
    exit_code = $exitCode
    log = $logPath
}

$result | ConvertTo-Json -Depth 4

if ($exitCode -ne 0) {
    exit $exitCode
}
