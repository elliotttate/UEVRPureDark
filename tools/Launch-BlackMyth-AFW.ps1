# Launch Black Myth: Wukong from STEAM with the sibling (UEVRPureDark) no-DLSS UEVR backend,
# deploy the decoupled AFW frame-resource plugin, set VR_RenderingMethod=3 + the no-DLSS bridge env,
# inject, and wait for the UEVR MCP. Adapted from Launch-AFW2-NoDLSS.ps1.
#
# Env propagation: a game launched by an ALREADY-RUNNING Steam inherits Steam's cached env, not ours.
# So we stop Steam, then start steam.exe -applaunch <appid> as OUR child with the env block on the PSI
# -> fresh Steam inherits our env -> the game (Steam's child) inherits it too.
param(
    [int]$AppId        = 2358720,
    [string]$SteamExe  = "C:\Program Files (x86)\Steam\steam.exe",
    [string]$ShippingExe= "E:\SteamLibrary\steamapps\common\BlackMythWukong\b1\Binaries\Win64\b1-Win64-Shipping.exe",
    [string]$ProfileName= "b1-Win64-Shipping",
    [string]$BackendDll = "E:\Github\UEVRPureDark\build\bin\uevr\UEVRBackend.dll",
    [string]$AfwPluginDll = "E:\Github\UEVRPureDark\build\Release\afw_frame_resources.dll",
    [string]$McpPluginDll = "E:\Github\uevr-mcp\plugin\build\Release\uevr_mcp.dll",
    [string]$RuntimeJson= "C:\Program Files\MetaXRSimulator\v201.0\meta_openxr_simulator.json",
    [int]$WindowWaitSeconds = 180,
    [int]$McpWaitSeconds = 150,
    [int]$InjectWaitMs = 30000,
    [int]$AfwDebugView = 1,
    [switch]$EnableRenderDoc,
    [string]$RenderDocDll = "E:\Github\UEVRPureDark\build\bin\uevr\renderdoc.dll"
)
$ErrorActionPreference = "Stop"

function Assert-File([string]$p,[string]$label){ if(-not (Test-Path -LiteralPath $p -PathType Leaf)){ throw "$label not found: $p" } }
function Set-ConfigValue([string]$Path,[string]$Key,[string]$Value){
    # Read raw + split on ANY line ending so a stray \r-only or missing-trailing-newline file
    # never collapses into one "line". @(...) around the loop forces an ARRAY, so the not-found
    # append below is an array-append, NOT a string concatenation (the bug that joined every
    # key=value onto the Frontend_RequestedRuntime line and made UEVR try OpenVR + disable AFW).
    $lines = @()
    if(Test-Path -LiteralPath $Path){ $lines = @([IO.File]::ReadAllText($Path) -split "`r`n|`n|`r" | Where-Object { $_ -ne '' }) }
    $pattern = "^{0}=" -f [regex]::Escape($Key); $replaced=$false
    $out = @(foreach($l in $lines){ if($l -match $pattern){ $replaced=$true; "$Key=$Value" } else { $l } })
    if(-not $replaced){ $out += "$Key=$Value" }
    [IO.File]::WriteAllText($Path, (($out -join "`n") + "`n"))
}
function Copy-IfDifferent([string]$src,[string]$dst,[string]$label){
    if(-not (Test-Path -LiteralPath $src)){ Write-Warning "$label source missing: $src"; return }
    $need=$true
    if(Test-Path -LiteralPath $dst){ try { $need = (Get-FileHash -Algorithm SHA256 -LiteralPath $src).Hash -ne (Get-FileHash -Algorithm SHA256 -LiteralPath $dst).Hash } catch { $need=$true } }
    if($need){ Copy-Item -LiteralPath $src -Destination $dst -Force; Write-Host "Deployed $label -> $dst" } else { Write-Host "$label already current: $dst" }
}

Assert-File $SteamExe    "Steam exe"
Assert-File $ShippingExe "Shipping exe"
Assert-File $BackendDll  "UEVR backend DLL"
Assert-File $RuntimeJson "OpenXR runtime JSON"

if (-not ("UevrLocalInjector" -as [type])) {
    Add-Type @"
using System; using System.Collections.Generic; using System.Diagnostics; using System.Linq;
using System.Runtime.InteropServices; using System.Text;
public static class UevrLocalInjector {
    const uint PROCESS_ALL_ACCESS=0x1F0FFF; const uint MEM_COMMIT_RESERVE=0x3000; const uint MEM_RELEASE=0x8000;
    const uint PAGE_READWRITE=0x04; const uint WAIT_TIMEOUT=0x102;
    [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint a,bool i,int p);
    [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr VirtualAllocEx(IntPtr h,IntPtr a,uint s,uint t,uint p);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool VirtualFreeEx(IntPtr h,IntPtr a,uint s,uint t);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool WriteProcessMemory(IntPtr h,IntPtr a,byte[] b,uint s,out uint w);
    [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr m,string n);
    [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Ansi)] static extern IntPtr GetModuleHandleA(string n);
    [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr CreateRemoteThread(IntPtr h,IntPtr a,uint s,IntPtr f,IntPtr p,uint fl,out uint t);
    [DllImport("kernel32.dll",SetLastError=true)] static extern uint WaitForSingleObject(IntPtr h,uint ms);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetExitCodeThread(IntPtr h,out uint c);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool CloseHandle(IntPtr h);
    public static string Inject(int pid,string dll,int waitMs){
        var hp=OpenProcess(PROCESS_ALL_ACCESS,false,pid); if(hp==IntPtr.Zero) return "ERR OpenProcess "+Marshal.GetLastWin32Error();
        IntPtr rp=IntPtr.Zero;
        try{ var b=Encoding.ASCII.GetBytes(dll+"\0");
            rp=VirtualAllocEx(hp,IntPtr.Zero,(uint)b.Length,MEM_COMMIT_RESERVE,PAGE_READWRITE); if(rp==IntPtr.Zero) return "ERR VirtualAllocEx "+Marshal.GetLastWin32Error();
            uint w; if(!WriteProcessMemory(hp,rp,b,(uint)b.Length,out w)) return "ERR WriteProcessMemory "+Marshal.GetLastWin32Error();
            var k=GetModuleHandleA("kernel32.dll"); var ll=GetProcAddress(k,"LoadLibraryA"); if(ll==IntPtr.Zero) return "ERR GetProcAddress";
            uint tid; var ht=CreateRemoteThread(hp,IntPtr.Zero,0,ll,rp,0,out tid); if(ht==IntPtr.Zero) return "ERR CreateRemoteThread "+Marshal.GetLastWin32Error();
            try{ var wr=WaitForSingleObject(ht,(uint)waitMs); if(wr==WAIT_TIMEOUT) return "ERR timeout";
                uint c; GetExitCodeThread(ht,out c); return c!=0?("OK LoadLibraryA=0x"+c.ToString("X")+" tid="+tid):"ERR LoadLibraryA=0"; }
            finally{ CloseHandle(ht); } }
        finally{ if(rp!=IntPtr.Zero) VirtualFreeEx(hp,rp,0,MEM_RELEASE); CloseHandle(hp); }
    }
    public static bool HasModule(int pid,string c){ try{ var p=Process.GetProcessById(pid); foreach(ProcessModule m in p.Modules){ if(m.ModuleName.IndexOf(c,StringComparison.OrdinalIgnoreCase)>=0) return true; } }catch{} return false; }
    public static string GetModulePath(int pid,string c){ try{ var p=Process.GetProcessById(pid); foreach(ProcessModule m in p.Modules){ if(m.ModuleName.IndexOf(c,StringComparison.OrdinalIgnoreCase)>=0) return m.FileName; } }catch{} return null; }
    public static string[] ListModulesMatching(int pid,string[] cs){ var h=new List<string>(); try{ var p=Process.GetProcessById(pid); foreach(ProcessModule m in p.Modules){ if(cs.Any(s=>m.ModuleName.IndexOf(s,StringComparison.OrdinalIgnoreCase)>=0)) h.Add(m.ModuleName); } }catch{} return h.ToArray(); }
}
"@
}

function Get-TargetProcess([string]$exe){
    $name=[IO.Path]::GetFileNameWithoutExtension($exe); $full=[IO.Path]::GetFullPath($exe)
    Get-Process -Name $name -ErrorAction SilentlyContinue |
        Where-Object { try{ [IO.Path]::GetFullPath($_.Path) -ieq $full }catch{ $false } } |
        Where-Object { try{ -not $_.HasExited }catch{ $true } } |
        Sort-Object StartTime -Descending | Select-Object -First 1
}

# --- 1. Stop the game + Steam (so a fresh Steam inherits our env block) ---
Get-Process -Name "b1-Win64-Shipping","b1" -ErrorAction SilentlyContinue | ForEach-Object { try{ Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }catch{} }
Get-Process -Name "steam","steamwebhelper" -ErrorAction SilentlyContinue | ForEach-Object { try{ Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }catch{} }
Start-Sleep -Seconds 3

# --- 2. Deploy plugins into the profile folder ---
$profileRoot = Join-Path $env:APPDATA "UnrealVRMod\$ProfileName"
$pluginDir   = Join-Path $profileRoot "plugins"
$configPath  = Join-Path $profileRoot "config.txt"
New-Item -ItemType Directory -Force -Path $pluginDir | Out-Null
Copy-IfDifferent $McpPluginDll (Join-Path $pluginDir "uevr_mcp.dll") "uevr_mcp.dll"
Copy-IfDifferent $AfwPluginDll (Join-Path $pluginDir "afw_frame_resources.dll") "afw_frame_resources.dll"

# --- 3. Config: AFW rendering + debug view ---
# Force OpenXR so UEVR never attempts OpenVR (which auto-launches SteamVR). Without this the
# runtime is "unset" and UEVR tries OpenVR first, spawning SteamVR before falling back to OpenXR.
Set-ConfigValue $configPath "Frontend_RequestedRuntime" "openxr_loader.dll"
Set-ConfigValue $configPath "VR_RenderingMethod" "3"   # AFW path
Set-ConfigValue $configPath "VR_FramewarpMode"   "3"   # Combined warp
Set-ConfigValue $configPath "VR_AFWDebugView"    "$AfwDebugView"
Set-ConfigValue $configPath "FrameworkConfig_MenuOpen" "false"

# --- 4. Env block: MetaXR sim + decoupled (no-DLSS) AFW frame-resource bridge ---
$envBlock = [ordered]@{
    XR_RUNTIME_JSON                              = $RuntimeJson
    UEVR_FRAME_RESOURCES                         = "1"
    UEVR_FRAME_RESOURCES_LOG                     = "2"
    UEVR_FRAME_RESOURCES_SELFTEST               = "1"
    UEVR_FRAME_RESOURCES_ENABLE_RTPOOL          = "1"
    UEVR_FRAME_RESOURCES_ENABLE_D3D12BIND       = "1"
    UEVR_FRAME_RESOURCES_ENABLE_DLSS_OBSERVER   = "0"   # DLSS off / absent
    UEVR_FRAME_RESOURCES_FORCE_VELOCITY         = "1"
    UEVR_FRAME_RESOURCES_DUMP_EVERY             = "30"
    # BMW tuning: the per-eye velocity (1124x1176, matches depth) is smaller than a desktop/spectator
    # velocity (2308x1444) that latches the high-water gate. Allow the depth-matched (low-res vs
    # high-water) velocity through, and disable the snapshot so the stale 2308 copy can't override it.
    UEVR_FRAME_RESOURCES_ALLOW_LOW_RES_VELOCITY = "1"
    UEVR_FRAME_RESOURCES_SNAPSHOT_VELOCITY      = "0"
    UEVR_AFW_FRAME_RESOURCES                     = "1"
    UEVR_AFW_FRAME_RESOURCES_LEGACY_FALLBACK    = "0"
    UEVR_AFW_FRAME_RESOURCES_VELOCITY           = "1"
    UEVR_AFW_DERIVED_PROJECTIONS                 = "1"
    UEVR_AFW_FULL_SOURCE_VIEWPORT               = "0"
    UEVR_AFW_PREFILL_WARP_OUTPUT                = "1"
    UEVR_AFW_PREFER_ENGINE_MV                   = "1"
    UEVR_AFW_VELDUMP                            = "1"
}

# Optional: late-load renderdoc.dll + bootstrap the in-app API so the backend's capture watcher
# (sentinel %TEMP%\uevr_renderdoc_capture.req) can TriggerMultiFrameCapture. refresh_hooks lets it
# capture even though renderdoc is loaded after the device is created ("work under RenderDoc").
if ($EnableRenderDoc) {
    $envBlock["UEVR_RENDERDOC_BOOTSTRAP"]  = "1"
    $envBlock["UEVR_LOAD_RENDERDOC_DLL"]   = "1"
    if (Test-Path -LiteralPath $RenderDocDll) { $envBlock["UEVR_RENDERDOC_DLL"] = $RenderDocDll }
    Write-Host "RenderDoc bootstrap ENABLED (dll: $RenderDocDll)"
}

# --- 5. Launch Steam (our child) with the env block, -applaunch the game ---
$psi = [System.Diagnostics.ProcessStartInfo]::new()
$psi.FileName = $SteamExe
$psi.Arguments = "-applaunch $AppId"
$psi.UseShellExecute = $false
foreach($k in $envBlock.Keys){ [Environment]::SetEnvironmentVariable($k,[string]$envBlock[$k],"Process"); $psi.Environment[$k]=[string]$envBlock[$k] }
$steam = [System.Diagnostics.Process]::Start($psi)
Write-Host "Started steam pid=$($steam.Id) -applaunch $AppId"

# --- 6. Wait for the shipping process + main window ---
$deadline=(Get-Date).AddSeconds($WindowWaitSeconds); $proc=$null
do { $proc=Get-TargetProcess $ShippingExe; if($proc){break}; Start-Sleep -Milliseconds 750 } while((Get-Date)-lt $deadline)
if(-not $proc){ throw "Timed out waiting for $ShippingExe" }
Write-Host "Found shipping pid=$($proc.Id)"
$wd=(Get-Date).AddSeconds($WindowWaitSeconds)
do { try{ $proc.Refresh(); if($proc.HasExited){break}; if($proc.MainWindowHandle -ne [IntPtr]::Zero){break} }catch{break}; Start-Sleep -Milliseconds 500 } while((Get-Date)-lt $wd)
Write-Host "Main window: $($proc.MainWindowHandle -ne [IntPtr]::Zero)  exited=$($proc.HasExited)"
# Give the game a moment to finish early init before injecting
Start-Sleep -Seconds 5

# --- 7. Inject the backend ---
$injectResult = $null
if([UevrLocalInjector]::HasModule($proc.Id,"UEVRBackend")){ $injectResult="UEVRBackend already loaded" }
else { $injectResult=[UevrLocalInjector]::Inject($proc.Id,[IO.Path]::GetFullPath($BackendDll),$InjectWaitMs) }
Write-Host "Inject: $injectResult"
$loaded=[UevrLocalInjector]::GetModulePath($proc.Id,"UEVRBackend")
Write-Host "Loaded backend path: $loaded"

# --- 8. Wait for plugins + MCP ---
$md=(Get-Date).AddSeconds(40)
do { $mods=[UevrLocalInjector]::ListModulesMatching($proc.Id,@("UEVR","uevr_mcp","afw_frame")); if($mods -match "uevr_mcp"){break}; Start-Sleep -Milliseconds 500 } while((Get-Date)-lt $md)
$mcpReady=$false; $dl=(Get-Date).AddSeconds($McpWaitSeconds)
do { try{ $null=Invoke-RestMethod -Uri "http://127.0.0.1:8899/api/game_info" -TimeoutSec 5; $mcpReady=$true; break }catch{ Start-Sleep -Milliseconds 1000 } } while((Get-Date)-lt $dl)
Write-Host "MCP ready: $mcpReady"

[ordered]@{
    pid          = $proc.Id
    profileRoot  = $profileRoot
    backendDll   = $BackendDll
    loadedBackend= $loaded
    backendMatch = ($loaded -and ([string]::Equals($loaded,[IO.Path]::GetFullPath($BackendDll),[StringComparison]::OrdinalIgnoreCase)))
    injectResult = $injectResult
    modules      = @($mods)
    mcpReady     = $mcpReady
    logTxt       = (Join-Path $profileRoot "log.txt")
} | ConvertTo-Json -Depth 6
