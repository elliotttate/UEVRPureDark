# Launch Black Myth: Wukong DIRECTLY (steam_appid.txt + Steam running) through UEVRRenderDocLauncher.exe
# so renderdoc.dll is injected into the SUSPENDED process BEFORE the D3D12 device is created (full capture,
# not the DEGRADED late-load that stalls UEVR init). The launcher inherits THIS shell's environment, so the
# frame-resource env set below propagates into the game. Navigation + capture are handled separately.
param(
    [int]$AppId        = 2358720,
    [string]$SteamExe  = "C:\Program Files (x86)\Steam\steam.exe",
    [string]$Win64Dir  = "E:\SteamLibrary\steamapps\common\BlackMythWukong\b1\Binaries\Win64",
    [string]$ProfileName= "b1-Win64-Shipping",
    [string]$Launcher  = "E:\Github\UEVRPureDark\build\bin\uevr\UEVRRenderDocLauncher.exe",
    [string]$BackendDll = "E:\Github\UEVRPureDark\build\bin\uevr\UEVRBackend.dll",
    [string]$RenderDocDll = "E:\Github\UEVRPureDark\build\bin\uevr\renderdoc.dll",
    [string]$AfwPluginDll = "E:\Github\UEVRPureDark\build\Release\afw_frame_resources.dll",
    [string]$McpPluginDll = "E:\Github\uevr-mcp\plugin\build\Release\uevr_mcp.dll",
    [string]$RuntimeJson= "C:\Program Files\MetaXRSimulator\v201.0\meta_openxr_simulator.json",
    [int]$AfwDebugView = 0
)
$ErrorActionPreference = "Stop"
$exe = Join-Path $Win64Dir "b1-Win64-Shipping.exe"
function Assert-File([string]$p,[string]$l){ if(-not(Test-Path -LiteralPath $p -PathType Leaf)){ throw "$l not found: $p" } }
function Set-ConfigValue([string]$Path,[string]$Key,[string]$Value){
    $lines=@(); if(Test-Path -LiteralPath $Path){ $lines=@(Get-Content -LiteralPath $Path) }
    $pat="^{0}=" -f [regex]::Escape($Key); $rep=$false
    $out=foreach($l in $lines){ if($l -match $pat){ $rep=$true; "$Key=$Value" } else { $l } }
    if(-not $rep){ $out+="$Key=$Value" }
    $out | Set-Content -LiteralPath $Path -Encoding ASCII
}
function Copy-IfDifferent([string]$s,[string]$d,[string]$l){
    if(-not(Test-Path -LiteralPath $s)){ Write-Warning "$l missing: $s"; return }
    $need=$true; if(Test-Path -LiteralPath $d){ try{ $need=(Get-FileHash -Algorithm SHA256 -LiteralPath $s).Hash -ne (Get-FileHash -Algorithm SHA256 -LiteralPath $d).Hash }catch{$need=$true} }
    if($need){ Copy-Item -LiteralPath $s -Destination $d -Force; Write-Host "Deployed $l" } else { Write-Host "$l current" }
}

Assert-File $Launcher "RenderDoc launcher"; Assert-File $exe "Shipping exe"
Assert-File $BackendDll "Backend"; Assert-File $RenderDocDll "renderdoc.dll"; Assert-File $RuntimeJson "OpenXR json"

# 1. Stop any old game; ensure Steam running; write steam_appid.txt for direct-launch DRM
Get-Process -Name "b1-Win64-Shipping","b1" -ErrorAction SilentlyContinue | ForEach-Object { try{ Stop-Process -Id $_.Id -Force }catch{} }
if(-not (Get-Process steam -ErrorAction SilentlyContinue)){ Start-Process $SteamExe; Start-Sleep -Seconds 10 }
Set-Content -Path (Join-Path $Win64Dir "steam_appid.txt") -Value "$AppId" -NoNewline -Encoding ascii
Start-Sleep -Milliseconds 800

# 2. Deploy plugins + config
$profileRoot = Join-Path $env:APPDATA "UnrealVRMod\$ProfileName"
$pluginDir = Join-Path $profileRoot "plugins"; $configPath = Join-Path $profileRoot "config.txt"
New-Item -ItemType Directory -Force -Path $pluginDir | Out-Null
Copy-IfDifferent $McpPluginDll (Join-Path $pluginDir "uevr_mcp.dll") "uevr_mcp.dll"
Copy-IfDifferent $AfwPluginDll (Join-Path $pluginDir "afw_frame_resources.dll") "afw_frame_resources.dll"
Set-ConfigValue $configPath "Frontend_RequestedRuntime" "openxr_loader.dll"
Set-ConfigValue $configPath "VR_RenderingMethod" "3"
Set-ConfigValue $configPath "VR_FramewarpMode" "3"
Set-ConfigValue $configPath "VR_AFWDebugView" "$AfwDebugView"
Set-ConfigValue $configPath "FrameworkConfig_MenuOpen" "false"

# 3. Frame-resource env (inherited by the launcher -> the suspended game). NO UEVR_RENDERDOC_* here;
#    the launcher sets those itself (BOOTSTRAP + PREHOOK_D3D12 + LAUNCHED_SUSPENDED + READY_EVENT).
$envBlock = [ordered]@{
    XR_RUNTIME_JSON                            = $RuntimeJson
    UEVR_FRAME_RESOURCES                       = "1"
    UEVR_FRAME_RESOURCES_LOG                   = "2"
    UEVR_FRAME_RESOURCES_SELFTEST              = "1"
    UEVR_FRAME_RESOURCES_ENABLE_RTPOOL         = "1"
    UEVR_FRAME_RESOURCES_ENABLE_D3D12BIND      = "1"
    UEVR_FRAME_RESOURCES_ENABLE_DLSS_OBSERVER  = "0"
    UEVR_FRAME_RESOURCES_FORCE_VELOCITY        = "1"
    UEVR_FRAME_RESOURCES_DUMP_EVERY            = "30"
    UEVR_FRAME_RESOURCES_ALLOW_LOW_RES_VELOCITY= "1"
    UEVR_FRAME_RESOURCES_SNAPSHOT_VELOCITY     = "0"
    UEVR_AFW_FRAME_RESOURCES                   = "1"
    UEVR_AFW_FRAME_RESOURCES_LEGACY_FALLBACK   = "0"
    UEVR_AFW_FRAME_RESOURCES_VELOCITY          = "1"
    UEVR_AFW_DERIVED_PROJECTIONS               = "1"
    UEVR_AFW_FULL_SOURCE_VIEWPORT              = "0"
    UEVR_AFW_PREFILL_WARP_OUTPUT               = "1"
}
foreach($k in $envBlock.Keys){ Set-Item -Path "Env:$k" -Value ([string]$envBlock[$k]) }

# 4. Run the suspended renderdoc launcher (inherits our env). Returns after it resumes the game.
Write-Host "Launching via UEVRRenderDocLauncher (suspended, renderdoc-first)..."
& $Launcher --exe $exe --backend $BackendDll --renderdoc $RenderDocDll --cwd $Win64Dir --ready-timeout-ms 60000 2>&1 | ForEach-Object { Write-Host "  [launcher] $_" }
Write-Host "launcher exit=$LASTEXITCODE"

# 5. Wait for the game + MCP
$deadline=(Get-Date).AddSeconds(120); $proc=$null
do { $proc=Get-Process -Name "b1-Win64-Shipping" -ErrorAction SilentlyContinue | Select-Object -First 1; if($proc){break}; Start-Sleep -Milliseconds 750 } while((Get-Date)-lt $deadline)
$mcpReady=$false; if($proc){ $dl=(Get-Date).AddSeconds(90); do { try{ Invoke-RestMethod -Uri "http://127.0.0.1:8899/api/game_info" -TimeoutSec 4 | Out-Null; $mcpReady=$true; break }catch{ Start-Sleep -Milliseconds 1000 } } while((Get-Date)-lt $dl) }
[ordered]@{ pid=$(if($proc){$proc.Id}else{$null}); mcpReady=$mcpReady; profileRoot=$profileRoot; sentinel=(Join-Path $env:TEMP "uevr_renderdoc_capture.req"); logTxt=(Join-Path $profileRoot "log.txt") } | ConvertTo-Json
