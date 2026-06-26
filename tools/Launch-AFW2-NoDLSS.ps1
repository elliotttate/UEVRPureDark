# Launch the AFW2 *NoDLSS* packaged build with the freshly-built UEVRBackend, deploy the
# decoupled AFW frame-resource plugin + real PDAFWPlugin, set the no-DLSS bridge env, inject,
# and wait for the UEVR MCP. Adapted from start_subnautica_uevr.ps1.
param(
    [string]$GameRoot   = "E:\UEProjects\AFW2\Packaged_NoDLSS_Shipping\Windows",
    [string]$LauncherExe= "E:\UEProjects\AFW2\Packaged_NoDLSS_Shipping\Windows\AFW2.exe",
    [string]$ShippingExe= "E:\UEProjects\AFW2\Packaged_NoDLSS_Shipping\Windows\AFW2\Binaries\Win64\AFW2-Win64-Shipping.exe",
    [string]$ProfileName= "AFW2-Win64-Shipping",
    [string]$BackendDll = "E:\Github\UEVRPureDark2\build\bin\uevr\UEVRBackend.dll",
    [string]$AfwPluginDll = "E:\Github\UEVRPureDark2\build\Release\afw_frame_resources.dll",
    [string]$PdafwDll   = "E:\Downloads\Game\PDAFWPlugin.dll",
    [string]$McpPluginDll = "E:\Github\uevr-mcp\plugin\build\Release\uevr_mcp.dll",
    [string]$RuntimeJson= "C:\Program Files\MetaXRSimulator\v201.0\meta_openxr_simulator.json",
    [int]$WindowWaitSeconds = 120,
    [int]$McpWaitSeconds = 120,
    [int]$InjectWaitMs = 30000
)
$ErrorActionPreference = "Stop"

function Assert-File([string]$p,[string]$label){ if(-not (Test-Path -LiteralPath $p -PathType Leaf)){ throw "$label not found: $p" } }
function Set-ConfigValue([string]$Path,[string]$Key,[string]$Value){
    $lines = @(); if(Test-Path -LiteralPath $Path){ $lines = @(Get-Content -LiteralPath $Path) }
    $pattern = "^{0}=" -f [regex]::Escape($Key); $replaced=$false
    $out = foreach($l in $lines){ if($l -match $pattern){ $replaced=$true; "$Key=$Value" } else { $l } }
    if(-not $replaced){ $out += "$Key=$Value" }
    $out | Set-Content -LiteralPath $Path -Encoding ASCII
}
function Copy-IfDifferent([string]$src,[string]$dst,[string]$label){
    if(-not (Test-Path -LiteralPath $src)){ Write-Warning "$label source missing: $src"; return }
    $need=$true
    if(Test-Path -LiteralPath $dst){ try { $need = (Get-FileHash -Algorithm SHA256 -LiteralPath $src).Hash -ne (Get-FileHash -Algorithm SHA256 -LiteralPath $dst).Hash } catch { $need=$true } }
    if($need){ Copy-Item -LiteralPath $src -Destination $dst -Force; Write-Host "Deployed $label -> $dst" } else { Write-Host "$label already current: $dst" }
}

Assert-File $LauncherExe "Launcher exe"
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

# --- 1. Stop any existing AFW2 processes (clean relaunch) ---
Get-Process -Name "AFW2-Win64-Shipping","AFW2" -ErrorAction SilentlyContinue | ForEach-Object { try{ Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }catch{} }
Start-Sleep -Milliseconds 800

# --- 2. Deploy plugins into the profile folder ---
$profileRoot = Join-Path $env:APPDATA "UnrealVRMod\$ProfileName"
$pluginDir   = Join-Path $profileRoot "plugins"
$configPath  = Join-Path $profileRoot "config.txt"
New-Item -ItemType Directory -Force -Path $pluginDir | Out-Null
Copy-IfDifferent $McpPluginDll (Join-Path $pluginDir "uevr_mcp.dll") "uevr_mcp.dll"
Copy-IfDifferent $AfwPluginDll (Join-Path $pluginDir "afw_frame_resources.dll") "afw_frame_resources.dll"
Copy-IfDifferent $PdafwDll     (Join-Path $pluginDir "PDAFWPlugin.dll") "PDAFWPlugin.dll (real)"
# Also place the real PDAFW next to the backend so the backend's /DELAYLOAD resolves the real one.
Copy-IfDifferent $PdafwDll     (Join-Path (Split-Path $BackendDll) "PDAFWPlugin.dll") "PDAFWPlugin.dll (beside backend)"

# --- 3. Ensure AFW rendering config ---
Set-ConfigValue $configPath "VR_RenderingMethod" "3"   # AFW path
Set-ConfigValue $configPath "VR_FramewarpMode"   "3"   # Combined warp
Set-ConfigValue $configPath "VR_AFWDebugView"    "1"
Set-ConfigValue $configPath "FrameworkConfig_MenuOpen" "false"

# --- 4. Env block: MetaXR sim + decoupled (no-DLSS) AFW frame-resource bridge ---
$envBlock = [ordered]@{
    XR_RUNTIME_JSON                              = $RuntimeJson
    # frame-resource plugin (depth+velocity WITHOUT DLSS)
    UEVR_FRAME_RESOURCES                         = "1"
    UEVR_FRAME_RESOURCES_LOG                     = "2"
    UEVR_FRAME_RESOURCES_SELFTEST               = "1"
    UEVR_FRAME_RESOURCES_ENABLE_RTPOOL          = "1"
    UEVR_FRAME_RESOURCES_ENABLE_D3D12BIND       = "1"
    UEVR_FRAME_RESOURCES_ENABLE_DLSS_OBSERVER   = "0"   # DLSS off / absent
    UEVR_FRAME_RESOURCES_FORCE_VELOCITY         = "1"
    UEVR_FRAME_RESOURCES_DUMP_EVERY             = "30"
    # AFW bridge consumption
    UEVR_AFW_FRAME_RESOURCES                     = "1"
    UEVR_AFW_FRAME_RESOURCES_LEGACY_FALLBACK    = "0"
    UEVR_AFW_FRAME_RESOURCES_VELOCITY           = "1"
    UEVR_AFW_DERIVED_PROJECTIONS                 = "1"
    UEVR_AFW_FULL_SOURCE_VIEWPORT               = "0"
    UEVR_AFW_PREFILL_WARP_OUTPUT                = "1"
}

# --- 5. Launch the bootstrap with the env block ---
$psi = [System.Diagnostics.ProcessStartInfo]::new()
$psi.FileName = $LauncherExe
$psi.WorkingDirectory = $GameRoot
$psi.UseShellExecute = $false
foreach($k in $envBlock.Keys){ [Environment]::SetEnvironmentVariable($k,[string]$envBlock[$k],"Process"); $psi.Environment[$k]=[string]$envBlock[$k] }
$launcher = [System.Diagnostics.Process]::Start($psi)
Write-Host "Started launcher pid=$($launcher.Id)"

# --- 6. Wait for the shipping process + main window ---
$deadline=(Get-Date).AddSeconds($WindowWaitSeconds); $proc=$null
do { $proc=Get-TargetProcess $ShippingExe; if($proc){break}; Start-Sleep -Milliseconds 500 } while((Get-Date)-lt $deadline)
if(-not $proc){ throw "Timed out waiting for $ShippingExe" }
Write-Host "Found shipping pid=$($proc.Id)"
$wd=(Get-Date).AddSeconds($WindowWaitSeconds)
do { try{ $proc.Refresh(); if($proc.HasExited){break}; if($proc.MainWindowHandle -ne [IntPtr]::Zero){break} }catch{break}; Start-Sleep -Milliseconds 500 } while((Get-Date)-lt $wd)
Write-Host "Main window: $($proc.MainWindowHandle -ne [IntPtr]::Zero)  exited=$($proc.HasExited)"

# --- 7. Inject the backend ---
$injectResult = $null
if([UevrLocalInjector]::HasModule($proc.Id,"UEVRBackend")){ $injectResult="UEVRBackend already loaded" }
else { $injectResult=[UevrLocalInjector]::Inject($proc.Id,[IO.Path]::GetFullPath($BackendDll),$InjectWaitMs) }
Write-Host "Inject: $injectResult"
$loaded=[UevrLocalInjector]::GetModulePath($proc.Id,"UEVRBackend")
Write-Host "Loaded backend path: $loaded"

# --- 8. Wait for plugins + MCP ---
$md=(Get-Date).AddSeconds(40)
do { $mods=[UevrLocalInjector]::ListModulesMatching($proc.Id,@("UEVR","uevr_mcp","PDAFW","afw_frame")); if($mods -match "uevr_mcp"){break}; Start-Sleep -Milliseconds 500 } while((Get-Date)-lt $md)
$mcpReady=$false; $mcpInfo=$null; $dl=(Get-Date).AddSeconds($McpWaitSeconds)
do { try{ $mcpInfo=Invoke-RestMethod -Uri "http://127.0.0.1:8899/api/game_info" -TimeoutSec 5; $mcpReady=$true; break }catch{ Start-Sleep -Milliseconds 1000 } } while((Get-Date)-lt $dl)
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
