# AFW Frame Resource Bridge Status - Black Flicker / Alternating Eye Artifact

Date: 2026-06-18  
Repo: `E:\Github\UEVRPureDark`  
Test game: `E:\AFW2\Windows`  
Correct launch path: `E:\AFW2\Windows\AFW2.exe`  
Do not direct-launch: `E:\AFW2\Windows\Engine\Binaries\Win64\UnrealGame-Win64-Shipping.exe`

## ROOT CAUSE FOUND + FIX APPLIED (2026-06-18)

The two-position oscillation was a **motion-vector SCALE bug, not (only) a resource-selection bug**, and the earlier diagnosis was inverted.

**What was wrong**

- The bridge computed `mvScale = color_resolution / velocity_resolution` (`D3D12Component.cpp`). That gave `~3.296` for the `633x534` velocity and `1.0` for the `2087x1760` velocity.
- UE motion vectors are stored in **resolution-INDEPENDENT** (normalized clip/NDC) units. Scaling motion magnitude by the resolution ratio is wrong — it inflated the `633` buffer's motion ~3.3x.
- With a moving camera, PDAFW's `InMotionScale` applied that inflated motion → the whole image warped to a different position. As the plugin alternated between the two RG16 buffers, mvScale alternated `3.296` ↔ `1.0` → **the image oscillated between two positions**.

**Inverted assumption corrected**

- `2087x1760 fmt=34` is `R16G16_FLOAT` — a **2-channel** buffer. Color targets are never RG16. So `2087x1760` **IS the real scene velocity** (at the guard-band scene render resolution), and `633x534` is a downsampled/auxiliary velocity (motion-blur/TAA dilation).
- Therefore the doc's old "expected good = 633x534 / bad = 2087x1760" was backwards. The plugin's existing area-tiebreak + sticky correctly prefer the **highest-resolution** velocity (`2087`), which is what you want.

**Fix applied (`D3D12Component.cpp`, in `UEVRBackend.dll` rebuilt 2026-06-18)**

- Default `mvScale` is now the provider's reported scale (`1.0`), **not** the resolution ratio. Result: motion magnitude is identical regardless of which RG16 buffer the plugin selects, so the two-position oscillation collapses even if selection isn't perfectly stable.
- `UEVR_AFW_VELOCITY_SCALE_X/Y` still override if PDAFW's decode wants a different constant. **Leave them UNSET for the first retest** (default 1.0).
- No plugin change needed — area-tiebreak + sticky already prefer the full-res velocity.

**Retest expectation**

- Velocity logs may still show either size; that no longer matters because mvScale stays constant. The image should stop jumping between two positions.
- Remaining moving-object black trailing edges and the temporal-side band are SEPARATE (PDAFW disocclusion/hole-fill + guard-band projection routing), not this scale bug.

## Goal

Build and test a decoupled AFW frame-resource module/plugin that can expose depth and motion-vector resources to UEVR/PDAFW without depending on DLSS and without turning this into a broad fork of UEVR itself.

The intended shape is:

1. A standalone module/plugin provides an API for frame resources.
2. The UEVR AFW path calls that API only when enabled.
3. DLSS can be completely absent or disabled.
4. The bridge is opt-in through environment variables/config.
5. Original UEVR code changes stay narrowly scoped to AFW resource consumption and projection/source routing.

## Current Binary State

Current active backend:

- `E:\Github\UEVRPureDark\build\bin\uevr\UEVRBackend.dll`
- Size: `11759104`
- Last write: `2026-06-18 13:39:25`
- SHA-256: `2A8FBFAAEE777B25C19171AA9F65192C8B135D8217A272B324638AACC9662899`

Current active PDAFW:

- `E:\Github\UEVRPureDark\build\bin\uevr\PDAFWPlugin.dll`
- Size: `512000`
- Last write: `2026-06-18 13:26:12`
- SHA-256: `D2E6612EAF742F3BB1479855664DD87F1D75287994DAD1587EA5FEC8C117E28D`
- Source copied from: `E:\Downloads\PDAFWPlugin.dll`
- Previous PDAFW backup: `E:\Github\UEVRPureDark\build\bin\uevr\PDAFWPlugin.dll.prev-20260618-132719`

Current active standalone plugin:

- Build output: `E:\Github\UEVRPureDark\build\Release\afw_frame_resources.dll`
- Deployed copy: `C:\Users\ellio\AppData\Roaming\UnrealVRMod\UnrealGame-Win64-Shipping\plugins\afw_frame_resources.dll`
- Size: `237568`
- Last write: `2026-06-18 13:40:02`
- SHA-256 deployed copy: `A40FB7B1E403E3E5C325C5BE8CD5B59DA1E81A9CD7D3DEA9B52DCB7AE20C4A88`

Current process state:

- No `AFW2.exe` process was running when this note was written.
- No `UnrealGame-Win64-Shipping.exe` process was running when this note was written.

## Current UEVR Profile State

Profile path:

- `C:\Users\ellio\AppData\Roaming\UnrealVRMod\UnrealGame-Win64-Shipping\config.txt`

Relevant values:

```ini
VR_RenderingMethod=3
VR_FramewarpMode=3
VR_HorizontalProjectionOverride=1
VR_GrowRectangleForProjectionCropping=true
```

Interpretation:

- `VR_RenderingMethod=3` is the AFW rendering path under test.
- `VR_FramewarpMode=3` is Combined warping.
- `VR_HorizontalProjectionOverride=1` is Symmetric horizontal projection override.
- `VR_GrowRectangleForProjectionCropping=true` enables the existing wider-render/crop machinery.

## Current Test Environment

Use MetaXR Simulator:

```powershell
$env:XR_RUNTIME_JSON='C:\Program Files\MetaXRSimulator\v201.0\meta_openxr_simulator.json'
```

Frame-resource bridge env used for tests:

```powershell
$env:UEVR_FRAME_RESOURCES='1'
$env:UEVR_FRAME_RESOURCES_LOG='2'
$env:UEVR_FRAME_RESOURCES_SELFTEST='1'
$env:UEVR_FRAME_RESOURCES_ENABLE_RTPOOL='1'
$env:UEVR_FRAME_RESOURCES_ENABLE_D3D12BIND='1'
$env:UEVR_FRAME_RESOURCES_ENABLE_DLSS_OBSERVER='0'
$env:UEVR_FRAME_RESOURCES_FORCE_VELOCITY='1'
$env:UEVR_FRAME_RESOURCES_DUMP_EVERY='30'
# Optional diagnostic only; default is 2.
# Use 6-8 only to test whether intermittent velocity staleness is causing AFW fallback/flicker.
$env:UEVR_FRAME_RESOURCES_MAX_STALE_FRAMES='6'

$env:UEVR_AFW_FRAME_RESOURCES='1'
$env:UEVR_AFW_FRAME_RESOURCES_LEGACY_FALLBACK='0'
$env:UEVR_AFW_FRAME_RESOURCES_VELOCITY='1'
$env:UEVR_AFW_DERIVED_PROJECTIONS='1'
$env:UEVR_AFW_FULL_SOURCE_VIEWPORT='0'
$env:UEVR_AFW_PREFILL_WARP_OUTPUT='1'
```

Important notes:

- `UEVR_FRAME_RESOURCES_ENABLE_DLSS_OBSERVER=0` means DLSS discovery is off.
- `UEVR_AFW_FRAME_RESOURCES_VELOCITY=1` enables motion-vector consumption by AFW.
- For depth-only tests, set `UEVR_AFW_FRAME_RESOURCES_VELOCITY=0`.
- `UEVR_AFW_FULL_SOURCE_VIEWPORT=0` is currently intentional because full-source viewport caused a severe two-position split.
- Velocity scale override exists but is not currently set:
  - `UEVR_AFW_VELOCITY_SCALE_X`
  - `UEVR_AFW_VELOCITY_SCALE_Y`

## Issue We Are Facing

There are two closely related visual artifacts.

### 1. Black temporal-side band / strip

The first screenshot showed a dark vertical strip at the outer edge of the synthesized eye. This lines up with cross-eye reprojection disocclusion / monocular FOV non-overlap:

- AFW renders one eye natively.
- The other eye is synthesized from the native eye.
- The source eye never saw some pixels needed by the destination eye, especially on the temporal edge.
- Correct depth helps interior reprojection quality, but it cannot invent source pixels outside the source eye's visible frustum.

The existing UEVR projection override system already has the ingredients for a guard band, but AFW initially did not route its projection/source feed through that wider render/crop path.

### 2. Flickering black alternate eye / two-position oscillation

The newer screenshots originally showed a black silhouette/edge artifact that alternated between eyes and appeared to oscillate between two positions in each rendered eye.

Current corrected read:

- The two-position jump was primarily a motion-vector scale bug.
- Logs previously showed AFW copying velocity from two different resource sizes:
  - `633x534`, format `34`, with `mvScale=3.296...`
  - `2087x1760`, format `34`, with `mvScale=1,1`
- `2087x1760 fmt=34` is `R16G16_FLOAT`, a real two-channel velocity-shaped target at the guard-band scene render resolution.
- `633x534` is likely a downsampled or auxiliary velocity target.
- UE motion vectors are resolution-independent normalized/clip-space values, so applying `color_resolution / velocity_resolution` as a magnitude scale was wrong.
- The fix makes `mvScale` default to the provider scale (`1,1`) regardless of velocity texture resolution.

Retest result:

- The rebuilt backend kept `mvScale=1,1` for both observed velocity sizes.
- The static simulator preview no longer showed the severe whole-image two-position jump.
- Numpad 6 motion-vector visualization shows a visible velocity field on the debug half of the view, including object/edge signal around the mannequin.
- The immediate problem is no longer "motion vectors are missing." Remaining artifacts are more likely velocity direction/convention, disocclusion/hole-fill behavior inside PDAFW, or the separate temporal-side guard-band issue.

## What Has Been Built

### Standalone frame-resource plugin

New plugin tree:

- `E:\Github\UEVRPureDark\plugins\afw_frame_resources`

Purpose:

- Discover frame depth and velocity resources independently of DLSS.
- Export a small API via `uevr_frame_resources_get_api`.
- Let UEVR consume resources only when explicitly enabled.

Providers included:

- D3D12 bind tracking provider.
- Render-target pool provider.
- Optional DLSS observer provider, currently disabled and not required.

Self-test result:

```text
[FrameResources] core selftest: 7/7 PASS
```

### UEVR bridge path

New bridge files:

- `E:\Github\UEVRPureDark\src\mods\vr\AFWFrameResourcesBridge.cpp`
- `E:\Github\UEVRPureDark\src\mods\vr\AFWFrameResourcesBridge.hpp`

Purpose:

- Dynamically discover and load the frame-resource plugin.
- Copy plugin-provided depth and velocity into the PDAFW frame buffers.
- Keep the path disabled unless env/config opts into it.

### AFW projection/source routing changes

Touched areas:

- `E:\Github\UEVRPureDark\src\mods\vr\runtimes\OpenXR.cpp`
- `E:\Github\UEVRPureDark\src\mods\vr\D3D12Component.cpp`

Purpose:

- Allow AFW projections to use the same overridden FOV shape as the normal guard-band projection path.
- Keep this behind `UEVR_AFW_DERIVED_PROJECTIONS=1`.
- Avoid assuming a DIBR depth tracker exists.
- Avoid requiring DLSS.

Expected log when this path is active:

```text
AFW using derived projection FOVs ...
AFW guard-band source feed ... full_viewport=0
```

## What We Have Tried

### 1. Direct game exe launch

Result:

- Failed with Unreal ICU fatal error.

Error:

```text
ICU data directory was not discovered:
../../../Engine/Content/Internationalization
```

Conclusion:

- Direct-launching `UnrealGame-Win64-Shipping.exe` is wrong for this packaged layout.
- Use `E:\AFW2\Windows\AFW2.exe` as the bootstrap launcher.

### 2. Depth-only bridge

Config:

```powershell
$env:UEVR_AFW_FRAME_RESOURCES='1'
$env:UEVR_AFW_FRAME_RESOURCES_VELOCITY='0'
```

Result:

- Depth copies were stable.
- No access violation reproduced during the stability pass.
- The black silhouette/edge artifact remained around moving objects.

Conclusion:

- Depth alone is not enough for the moving-object artifact.
- This is consistent with motion-vector-dependent AFW behavior.

### 3. Derived AFW projections from UEVR projection override

Config:

```ini
VR_HorizontalProjectionOverride=1
VR_GrowRectangleForProjectionCropping=true
```

Env:

```powershell
$env:UEVR_AFW_DERIVED_PROJECTIONS='1'
```

Result:

- Logs showed derived AFW projection FOVs were active.
- This addresses the AFW/projection disconnect, but it did not by itself remove the moving-object black artifact.

Conclusion:

- Projection wiring is necessary for the temporal-edge guard band problem.
- It is not sufficient for moving-object ghost/black artifacts without correct velocity.

### 4. Full source viewport feed

Config:

```powershell
$env:UEVR_AFW_FULL_SOURCE_VIEWPORT='1'
```

Result:

- Caused a severe two-position split.

Conclusion:

- Current default is restored to:

```powershell
$env:UEVR_AFW_FULL_SOURCE_VIEWPORT='0'
```

### 5. Prefill warp output

Config:

```powershell
$env:UEVR_AFW_PREFILL_WARP_OUTPUT='1'
```

Result:

- The destination warp output is prefilled with the native eye color before `EvaluateFrameWarp`.
- The black silhouette remained.

Conclusion:

- PDAFW appears to overwrite or clear disocclusion/hole regions internally.
- Prefill alone is not a complete fix.

### 6. Velocity bridge, first pass

Config:

```powershell
$env:UEVR_AFW_FRAME_RESOURCES_VELOCITY='1'
```

Observed logs:

```text
AFW bridge copied velocity ... 633x534 fmt=34 ... mvScale=3.296...
AFW bridge copied velocity ... 2087x1760 fmt=34 ... mvScale=1,1
```

Original conclusion, now corrected:

- The bridge was finding plausible velocity-shaped resources.
- The earlier assumption that `633x534` was "good" and `2087x1760` was "bad" was inverted.
- `2087x1760 fmt=34` is `R16G16_FLOAT`, a two-channel velocity-shaped target at the guard-band scene render resolution.
- `633x534` is likely a downsampled/auxiliary velocity buffer.
- The two-position oscillation came from applying different `mvScale` values, not from the full-resolution target being invalid.

Fix applied:

- `D3D12Component.cpp` now defaults `mvScale` to the provider-reported scale, which is `1.0` unless the provider derives a real one.
- It no longer computes `mvScale = color_resolution / velocity_resolution`.
- Leave `UEVR_AFW_VELOCITY_SCALE_X/Y` unset for the first retest.
- If motion pushes the wrong way after this, test sign, not magnitude.

### 7. Velocity candidate selection patch

Changed:

- `E:\Github\UEVRPureDark\plugins\afw_frame_resources\providers\D3D12BindProvider.cpp`

New behavior:

- Prefer canonical RG16 velocity-shaped targets.
- For equal intrinsic score, prefer larger area.
- This intentionally prefers the full-resolution guard-band scene velocity over smaller auxiliary/downsampled velocity targets.

Current status:

- Rebuilt successfully.
- Deployed successfully.
- Retested in the MetaXR simulator with AFW2.
- Logs still showed both `633x534` and `2087x1760` velocity resources, but both used `mvScale=1,1`.
- This is acceptable for the scale-fix proof; the old `3.296` scale did not reappear.

### 7b. Sticky velocity hardening

Review finding:

- Sticky velocity previously re-stamped a reused candidate with the current frame.
- That could hide tracker staleness.
- Holding an `ID3D12Resource` reference keeps the object alive, but it does not prove UE RDG has not aliased the underlying transient memory for another use.

Changed:

- Sticky fallback now preserves the original observation frame.
- Sticky/current comparisons keep the same canonical-score + largest-area policy as live candidates.

Current status:

- Rebuilt successfully.
- Self-test result: `7/7 PASS`.
- Deployed successfully.
- Retested in the MetaXR simulator with AFW2.
- API snapshots can report sticky velocity as stale because the sticky path now preserves the original observation frame instead of re-stamping it.
- That staleness is intentional; it prevents reused RDG memory from being falsely reported as a fresh velocity resource.
- A diagnostic env switch now exists: `UEVR_FRAME_RESOURCES_MAX_STALE_FRAMES`. Default remains `2`; test `6` or `8` only when isolating whether intermittent velocity staleness causes AFW to fall back to `FromOtherEye`.

### 7c. UEVR SceneDepthZ depth copy for AFW

Review finding:

- Yes, UEVR already has a depth path.
- UEVR hooks `FRenderTargetPool::FindFreeElement` and resolves `SceneDepthZ`.
- `D3D12Component.cpp` already used `SceneDepthZ` for OpenXR depth swapchain sizing/submission.
- But the AFW `depthDesc[nEye]` path only allocated a matching texture from `rawDepthTex`; it did not copy `SceneDepthZ` into that PDAFW depth input unless DLSS or the bridge copied depth.

Changed:

- When UEVR resolves `SceneDepthZ`, AFW now copies that resource into `vr->depthDesc[nEye]` on the PDAFW command list.
- Bridge depth can still copy if the plugin provider is `RenderTargetPool`.
- If UEVR already copied `SceneDepthZ`, lower-confidence plugin D3D12-bind depth is skipped instead of overwriting it.

Expected log:

```text
AFW copied UEVR SceneDepthZ depth ...
```

Interpretation:

- Depth no longer needs to come from DLSS.
- Depth no longer needs to be guessed by D3D12 bind tracking when UEVR's existing `SceneDepthZ` hook succeeds.
- The standalone plugin remains useful for velocity and diagnostics.

Retest result:

- AFW2 did not log `AFW copied UEVR SceneDepthZ depth ...`.
- Runtime status still reported depth from the D3D12 bind provider at `1268x1068`.
- This is expected for this UE5/RDG title: RDG transient scene depth does not necessarily go through the legacy `FRenderTargetPool::FindFreeElement` path that UEVR hooks for named pooled targets.
- Depth-via-D3D12-bind is therefore the correct fallback here, not a failure of the standalone module.
- A low-risk hardening patch now ranks depth-shaped DSV candidates instead of blindly accepting the last DSV bound that frame.

Source-backed cvar test:

- UE source at `E:\UnrealEngine\Source\Engine` confirms that `r.RDG.TransientAllocator=0` should make non-transient RDG textures route through `GRenderTargetPool.FindFreeElement(..., Texture->Name)`.
- Live AFW2 test: `r.RDG.TransientAllocator 0` was accepted by the console.
- Result: depth still resolved through the D3D12 bind provider, not `RenderTargetPool`.
- UEVR log showed the practical blocker:

```text
Attempting to hook RenderTargetPool::FindFreeElement
Scanning module Renderer...
Failed to find UITargetRT string
Scanning module RenderCore...
Failed to find UITargetRT string
Failed to find FRenderTargetPool::FindFreeElement, cannot hook
```

Interpretation:

- The cvar path is source-correct, but it only helps if UEVR can hook `FRenderTargetPool::FindFreeElement` in the shipping process.
- In this stripped AFW2 build, the current scanner does not find that function because it depends on a missing `UITargetRT` anchor string.
- So the cvar is not a complete fix yet; either the scanner needs a better signature, or we should use a direct UE/RDG hook/provider.

### 8. Newer PDAFW DLL

Changed:

- Copied `E:\Downloads\PDAFWPlugin.dll` over the active PDAFW binary.

Current active:

- `E:\Github\UEVRPureDark\build\bin\uevr\PDAFWPlugin.dll`
- Size: `512000`
- SHA-256: `D2E6612EAF742F3BB1479855664DD87F1D75287994DAD1587EA5FEC8C117E28D`

Backup:

- `E:\Github\UEVRPureDark\build\bin\uevr\PDAFWPlugin.dll.prev-20260618-132719`

Current status:

- Retested after copying the newer PDAFW DLL.
- The run was stable enough to capture screenshots and query the frame-resource API.
- Remaining artifacts were not eliminated by the PDAFW DLL swap alone.

## What We Still Need To Try

### Retest: velocity candidate stability

Run AFW2 through the bootstrap with velocity enabled and the newly rebuilt frame-resource plugin.

Expected good log pattern:

```text
AFW bridge copied velocity ... fmt=34 ... mvScale=1,1
```

Expected bad log pattern:

```text
AFW bridge copied velocity ... mvScale=3.296...
```

The velocity resolution may still show `2087x1760` or `633x534`. That is acceptable for this retest as long as `mvScale` stays constant at `1,1` and the two-position oscillation is gone.

Result:

- Passed the scale portion of the test.
- `mvScale` stayed at `1,1`.
- Numpad 6 confirmed that velocity visualization is present and has meaningful object/edge signal.
- The remaining moving-object black edge is now a PDAFW input-contract or disocclusion-fill problem, not a "no motion vectors exist" problem.

### Next visual capture

After injection, capture the MetaXR Simulator preview and compare:

1. Does the black artifact still alternate each eye?
2. Does the mannequin/object silhouette still get a black trailing edge?
3. Does the temporal side band still appear?
4. Does the image still jump between two positions?

### Framewarp mode isolation

If velocity selection is stable but black artifacts remain, test:

```ini
VR_FramewarpMode=2
```

Previous-frame warping should reduce cross-eye disocclusion because it reprojects from the same eye's previous frame rather than the other eye.

Then test:

```ini
VR_FramewarpMode=1
```

Alternate-eye-only warping isolates the pure cross-eye path.

Useful interpretation:

- If PreviousFrame looks clean but Alternate/Combined do not, the remaining problem is cross-eye disocclusion or PDAFW's cross-eye fill path.
- If all modes show the black silhouette, the input resource interpretation or PDAFW velocity/depth contract is still wrong.

### Motion-vector direction checks

Leave velocity scale at the default:

```powershell
Remove-Item Env:\UEVR_AFW_VELOCITY_SCALE_X -ErrorAction SilentlyContinue
Remove-Item Env:\UEVR_AFW_VELOCITY_SCALE_Y -ErrorAction SilentlyContinue
```

Do not restore the old resolution-ratio scale. UE motion vectors are already resolution-independent.

If motion magnitude looks sane but pushes the wrong direction, test sign flips only:

```powershell
$env:UEVR_AFW_VELOCITY_SCALE_X='-1'
$env:UEVR_AFW_VELOCITY_SCALE_Y='1'
```

or:

```powershell
$env:UEVR_AFW_VELOCITY_SCALE_X='1'
$env:UEVR_AFW_VELOCITY_SCALE_Y='-1'
```

Use Numpad 6 during each sign test so the visualized vector field can be compared against the visible artifact.

### PDAFW side behavior

If all bridge inputs are stable and correct but the black silhouette remains, the remaining issue is likely inside PDAFW's hole/disocclusion handling:

- PDAFW may clear uncovered pixels internally.
- Combined warping may not be using the previous-frame fill path as expected.
- Motion-vector units/sign/convention may differ from what PDAFW expects.
- Cross-eye disocclusion may require an explicit post-warp fill shader or history fallback.

At that point the bridge has done its job: depth/velocity are available without DLSS, and the next fix is PDAFW contract/fill behavior rather than resource discovery.

### Optional capture/debug escalation

If logs and screenshots are inconclusive:

1. Capture a RenderDoc/PIX frame after UEVR/PDAFW injection.
2. Identify the actual UE velocity pass/resource.
3. Compare resource pointer/format/size against what `afw_frame_resources` publishes.
4. Inspect PDAFW inputs immediately before `EvaluateFrameWarp`.

## Why We Are Still Guessing About Depth / Velocity

The current plugin has a heuristic provider because it is operating from the outside of the UE renderer:

- D3D12 bind tracking sees descriptors, formats, dimensions, and binding patterns.
- Render-target-pool tracking can use resource names when names survive and the pool is visible.
- Those are useful as a decoupled fallback, but they are not the final authority.

With local UE source, the better long-term answer is a UE-specific provider that reports scene resources from renderer/RDG structures directly. The authoritative tree for this investigation is now `E:\UnrealEngine`; relevant points verified in `E:\UnrealEngine\Source\Engine`:

- `Engine\Source\Runtime\Renderer\Internal\SceneTextures.h`
  - `FMinimalSceneTextures::Depth` is the scene depth texture.
  - `FSceneTextures::Velocity` is the dynamic motion-vector texture.
- `Engine\Source\Runtime\Renderer\Private\SceneTextures.cpp`
  - `FindStereoDepthTexture(...)` calls `StereoRenderTargetManager->AllocateDepthTexture(...)` at line `120`.
  - If direct stereo depth succeeds, UE registers it as `SceneDepthZ` at line `449`.
  - Otherwise UE creates RDG scene depth as `GraphBuilder.CreateTexture(Desc, TEXT("SceneDepthZ"))` at line `459`.
  - `SceneTextures.Velocity` is created as `SceneVelocity` at line `692`.
- `Engine\Source\Runtime\RenderCore\Private\RenderGraphPrivate.cpp`
  - `GRDGTransientAllocator` defaults to `1` at line `289`.
  - The cvar is `r.RDG.TransientAllocator` at line `291`.
- `Engine\Source\Runtime\RenderCore\Private\RenderGraphBuilder.cpp`
  - `FRDGBuilder` uses a transient allocator only when `GRDGTransientAllocator != 0` at line `577`.
  - Non-transient texture backing calls `GRenderTargetPool.FindFreeElement(InRHICmdList, Texture->Desc, Texture->Name)` at line `4404`.
- `Engine\Source\Runtime\RenderCore\Public\RendererInterface.h`
  - `FPostOpaqueRenderParameters` contains `ColorTexture`, `DepthTexture`, `VelocityTexture`, `SmallDepthTexture`, `ViewportRect`, `ViewMatrix`, `ProjMatrix`, `GraphBuilder`, and `View`.
  - `IRendererModule::RegisterPostOpaqueRenderDelegate(...)` is the clean public delegate route when available from a real UE module.
- `Engine\Source\Runtime\Renderer\Private\SceneRendering.cpp`
  - `FRendererModule::RenderPostOpaqueExtensions(...)` fills:
    - `RenderParameters.ColorTexture = SceneTextures.Color.Target`
    - `RenderParameters.DepthTexture = SceneTextures.Depth.Target`
    - `RenderParameters.VelocityTexture = SceneTextures.Velocity`
    - `RenderParameters.ViewportRect = View.ViewRect`
    - `RenderParameters.GraphBuilder = &GraphBuilder`
- `Engine\Source\Runtime\Renderer\Private\PostProcess\VisualizeMotionVectors.cpp`
  - The motion-vector debug pass samples `Inputs.SceneVelocity.Texture`, which matches what Numpad 6 is showing in the simulator.

Existing UEVR depth path:

- `E:\Github\UEVRPureDark\src\mods\vr\RenderTargetPoolHook.cpp` hooks `FRenderTargetPool::FindFreeElement`.
- `E:\Github\UEVRPureDark\src\mods\vr\RenderTargetPoolHook.hpp` exposes `get_texture<T>(L"SceneDepthZ")`.
- `E:\Github\UEVRPureDark\src\mods\vr\D3D12Component.cpp` already asks that hook for `SceneDepthZ`.
- In this AFW2 retest, the hook did not resolve `SceneDepthZ`.
- That is expected when UE5 RDG allocates scene depth through its transient allocator instead of the legacy named pooled target path.
- Turning off RDG transient allocation is not enough in this AFW2 build until UEVR's `FindFreeElement` scanner can locate the function.

Interpretation:

- We do not need to guess forever.
- The provider stack should prefer an authoritative UE/RDG provider when it can resolve the needed hook point in the shipping process.
- The current D3D12 bind provider should remain as fallback and diagnostics for unknown UE versions/titles.
- Having source tells us what to hook and how to interpret the data; it does not automatically give symbol names or stable addresses in a packaged shipping binary, so the practical work is still pattern/resolution plus validation.

Practical direct-hook options:

1. Clean UE-module route: register a `FPostOpaqueRenderDelegate` with `IRendererModule::RegisterPostOpaqueRenderDelegate`. This is the correct source-level API, but an injected UEVR backend is not currently built as the game project's UE module, so resolving and safely constructing the delegate in a shipping binary is non-trivial.
2. Direct injected route: pattern-resolve/hook `FRendererModule::RenderPostOpaqueExtensions` and read the same `FSceneTextures` values the engine passes into `FPostOpaqueRenderParameters`. This is source-backed and does not require DLSS, but it needs ABI/layout validation for UE 5.6 shipping builds.
3. Repair the existing UEVR `FRenderTargetPool::FindFreeElement` scanner and then use `r.RDG.TransientAllocator=0` as a deterministic named-depth path.
4. Implement `AllocateDepthTexture` in UEVR's stereo render-target manager so UE registers a UEVR-owned external `SceneDepthZ`. This is the cleanest depth ownership path, but it requires returning a real UE `FRHITexture`/`FTextureRHIRef`, not just a D3D12 resource pointer.
5. Keep current fallback route: D3D12 bind tracking plus render-target-pool names. This is already working enough to provide depth/velocity, but it is heuristic and should become fallback once the UE/RDG route works.

## Launch / Injection Reminder

Do not direct-launch the shipping exe. Launch the bootstrap:

```powershell
Start-Process -FilePath 'E:\AFW2\Windows\AFW2.exe' -WorkingDirectory 'E:\AFW2\Windows'
```

Then inject into:

```text
E:\AFW2\Windows\Engine\Binaries\Win64\UnrealGame-Win64-Shipping.exe
```

Use backend:

```text
E:\Github\UEVRPureDark\build\bin\uevr\UEVRBackend.dll
```

Use MCP plugin:

```text
E:\Github\uevr-mcp\plugin\build\Release\uevr_mcp.dll
```

## Current Best Read

The original edge band is mostly a cross-eye FOV/disocclusion problem. The projection-override wiring is the right fix direction for that.

The newer alternating black/flicker issue is most likely not the same thing. The best current evidence now points to the old oscillation being caused by wrong motion-vector scale, while the remaining black moving-object edge is either velocity convention or PDAFW disocclusion fill.

The immediate next proof is simple:

1. Keep `UEVR_AFW_VELOCITY_SCALE_X/Y` unset.
2. Use Numpad 6 to compare the visible velocity field against moving-object artifacts.
3. Retest once with `UEVR_FRAME_RESOURCES_MAX_STALE_FRAMES=6` to see whether avoiding intermittent `FromOtherEye` fallback changes the black flicker.
4. If the motion magnitude is sane but pushes the wrong direction, test sign overrides.
5. Build a UE/RDG provider or direct hook around `RenderPostOpaqueExtensions` so depth and velocity come from `FSceneTextures` instead of bind heuristics.
6. Continue treating the temporal-side band as a separate guard-band/cross-eye disocclusion problem.
