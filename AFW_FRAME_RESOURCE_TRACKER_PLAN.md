# AFW Frame Resource Tracker Plan

Date: 2026-06-18

Primary checkout: `E:\Github\UEVRPureDark`

Scope rule: this plan assumes implementation source and link dependencies come from `E:\Github\UEVRPureDark`. It does not assume `DIBRDepthTracker` exists. It does not assume another checkout can be copied in. Any missing tracker functionality is listed as new work to build in this repo. `E:\Epic Games\UnrealEngine-5.6.1` may be used as reference documentation for Unreal renderer resource names, pass timing, formats, cvars, and call semantics, but the plugin should not become an Unreal Engine source build dependency.

Architecture rule: build this as a loadable UEVR plugin / independent module, not as a fork of UEVR itself. The tracker should live outside the `uevr` target. Any changes under `src/mods`, `src/hooks`, or `include/uevr/API.*` must be optional bridge points, guarded, and small enough that UEVR still behaves exactly as before when the plugin is absent or disabled.

DLSS rule: the module must not require DLSS to be installed, enabled, loaded, or linked. It must build without NVIDIA NGX import libs, run when `nvngx.dll` / `_nvngx.dll` are absent, and complete the DLSS-off test path using non-DLSS providers. DLSS/NGX can only be an optional observer provider that is dynamically detected and skipped when unavailable.

Local API rule: anything already present under `E:\Github\UEVRPureDark` is fair use for building the plugin's internal API and provider adapters. That includes public SDK headers, internal UEVR headers, local UESDK structs, PureDark AFW headers, and existing helper implementations. The constraint is architectural, not source-access: the tracker should still ship as a plugin/module, and its exported API should not leak unstable internal types unless that is deliberately versioned.

## Verified Against Source (2026-06-18)

This plan was reconciled against the actual repo before implementation. The findings below
override any earlier statement in this document that contradicts them.

```text
1. NATIVE RESOURCE EXTRACTION IS A SOLVED, LINK-SAFE PATH (was the plan's biggest open risk).
   - API::RenderTargetPoolHook::get_render_target(name) returns an opaque
     UEVR_IPooledRenderTargetHandle. src/mods/pluginloader/FRenderTargetPoolHook.cpp:15
     shows that handle IS the internal sdk::IPooledRenderTarget* (a direct cast).
   - sdk::IPooledRenderTarget layout (dependencies/submodules/UESDK/src/sdk/StereoStuff.hpp):
       IPooledRenderTarget : IRefCountedObject { vtable; FSceneRenderTargetItem item; }   (item @ +0x08)
       FSceneRenderTargetItem { FTexture2DRHIRef texture; ... }                            (StereoStuff.hpp:212)
       FTexture2DRHIRef { FRHITexture2D* texture{nullptr}; }                               (StereoStuff.hpp:63-89)
     So item.texture.texture is an FRHITexture2D*, reachable with a tiny copied layout.
   - To turn FRHITexture2D* into ID3D12Resource*, DO NOT link StereoStuff.cpp's
     FRHITexture::get_native_resource(). Instead use the plugin-facing SDK function table:
       UEVR_FRHITexture2DFunctions::get_native_resource(handle)   (include/uevr/API.h:432)
     which is implemented in the main module (src/mods/pluginloader/FRHITexture2DFunctions.cpp:14)
     and reached from a plugin via the C++ wrapper API::FRHITexture2D::get_native_resource()
     (include/uevr/API.hpp:1187). This removes all cross-DLL linkage fragility.
   => The Phase 2 / Patch 6 "Option A/B/C" choice is resolved: use the layout walk + the
      frhitexture2d SDK function. No new SDK bridge is required.

2. LOGGING is API::get()->log_info / log_warn / log_error (printf-style), NOT spdlog.
   See examples/example_plugin/Plugin.cpp:60-62. spdlog is the main module's logger, not the
   plugin's. The plugin's debug output channel is the UEVR log via these calls.

3. CONSOLE access is API::get()->get_console_manager() (include/uevr/API.hpp:213) ->
   find_variable(L"r.Velocity.ForceOutput") -> set(1). IConsoleVariable::set(int/float) exists
   (API.hpp). The diagnostic-cvar snippets in this plan are correct in intent; the accessor is
   get_console_manager() directly on API, not via a free function.

4. RENDERER access is API::get()->param()->renderer, a const UEVR_RendererData* with fields
   { int renderer_type; void* device; void* swapchain; void* command_queue; } (API.h:253).
   Guard on renderer_type == UEVR_RENDERER_D3D12 (==1) before casting device/queue.

5. PLUGIN DEPLOYMENT/LOAD: UEVR LoadLibrary's plugin DLLs from <persistent_dir>/plugins and
   <persistent_dir>/../UEVR/plugins (src/mods/PluginLoader.cpp:1724-1761). Build output
   afw_frame_resources.dll must be copied into that plugins folder to be loaded. This is the
   deploy step every in-game test below depends on.

6. BUILD: cmake -B build && cmake --build build --config Release (cmake.toml header). Plugins
   use [template.plugin] (type="plugin"): C++23, includes include/ + examples/renderlib, links
   plugin_renderlib (which links imgui). Add [target.afw_frame_resources] type="plugin".

7. AFW ALREADY HAS A NON-DLSS VELOCITY STORY. In src/mods/vr/D3D12Component.cpp the AFW call
   sets MotionVectorsType = vr->is_fix_dlss() ? Normal : FromOtherEye, and motionVectorsDesc is
   DXGI_FORMAT_R16G16_FLOAT populated only from DLSS MV. So with DLSS off, AFW currently runs in
   FromOtherEye mode (reproject from the other eye's previous frame) and does NOT consume a
   velocity texture. The tracker's velocity discovery is therefore about UNLOCKING true
   per-object Normal-mode velocity without DLSS, not about making DLSS-off AFW work at all.
   This reframes the velocity-missing result: "velocity missing" is the expected baseline and
   only blocks Normal mode, not AFW itself.

8. DIBRDepthTracker.hpp/.cpp confirmed ABSENT. The "Important Correction" section stands.
```

## Implementation Status (2026-06-18)

```text
BUILT + VERIFIED (compiles, links, self-test passes):
  plugins/afw_frame_resources/
    Plugin.cpp                                   plugin entry, env config, exported C ABI, per-frame drive
    FrameResourceTypes.hpp                        enums + view/observation structs (C-ABI-aligned)
    FrameResourceLog.{hpp,cpp}                    sink-based, level-gated logger (Info/Debug/Trace)
    FrameResourceTracker.{hpp,cpp}                provider-neutral core: observe/resolve/staleness/describe + self-test
    include/UEVRFrameResourcesAPI.h               stable C ABI (version 1)
    providers/PooledRenderTargetLayout.hpp        minimal copied UESDK layout (the ONLY copied layout)
    providers/RenderTargetPoolProvider.{hpp,cpp}  Phase 2: SceneDepthZ + velocity-name probing
    providers/D3D12BindProvider.{hpp,cpp}         Phase 3: inline-hook RTV/DSV/OMSetRenderTargets, log-only velocity
    providers/NgxDlssProvider.{hpp,cpp}           Phase 6: detection-only optional observer (never required)
    tools/selftest_main.cpp                       offline tier-1 runner
  src/mods/vr/AFWFrameResourcesBridge.{hpp,cpp}   Patch 5 bridge helper (dead code until wired; see Patch 5)
  cmake.toml                                      targets afw_frame_resources (plugin) + afw_frame_resources_selftest (exe)

VERIFICATION DONE THIS SESSION:
  - afw_frame_resources_selftest.exe runs -> "core selftest: 7/7 PASS" (state machine, priority, staleness,
    eye routing, missing/invalid handling, describe_state).
  - cmake configure + build (VS2022 toolset) produced build/Release/afw_frame_resources.dll, linked with
    plugin_renderlib + imgui.
  - dumpbin confirms the DLL exports undecorated `uevr_frame_resources_get_api`.
  - All plugin TUs + the AFW bridge compile clean against the real uevr/API.hpp & uevr/Plugin.hpp.

NOT YET DONE (needs the game / live validation; cannot be done offline):
  - In-game DLSS-off run (Patch 4 / Tier 3): deploy afw_frame_resources.dll into <game>/plugins, set the
    UEVR_FRAME_RESOURCES_* env, confirm the depth/velocity log lines and self-test 7/7 at launch.
  - D3D12 bind hooks are written but UNVALIDATED in a live process: confirm the vtable indices
    (CreateRenderTargetView=20, CreateDepthStencilView=21, OMSetRenderTargets=46) bind correctly and that
    install/uninstall survives device reset. Keep UEVR_FRAME_RESOURCES_ENABLE_D3D12BIND as the kill switch.
  - Patch 5 wiring of the bridge into D3D12Component.cpp (depth-only first) — exact edit documented below.
  - Phases 6 (NGX observation hooks), 7 (velocity snapshot), 8 (corrected velocity).
```

## Goal

Build a decoupled frame resource tracker as a UEVR plugin / independent module in UEVRPureDark that lets AFW and future plugins ask for depth and velocity buffers through an API instead of reaching into `VR` fields or depending on DLSS.

The immediate test goal is:

```text
Run AFW without DLSS and prove what depth/velocity resources the module can discover and expose.
```

The long-term goal is:

```text
Make depth and velocity discovery reusable.
Expose a small API for initialization, provider updates, and buffer lookup.
Keep changes to the original UEVR/VR code narrow and explicit.
```

Desired end state:

```text
AFW or another caller asks the plugin/module:
  uevr_frame_resources_get_latest_depth(...)
  uevr_frame_resources_get_latest_velocity(...)

The tracker answers:
  this frame has SceneDepthZ from the render target pool
  or this frame has depth/velocity from D3D12 bind tracking
  or this frame has optional DLSS-provided depth/MV
  or velocity is missing
```

The tracker should be provider-neutral. DLSS should be one provider, not the owner of depth and velocity truth.

Non-DLSS operation is not a fallback mode. It is the primary design target.

## Hard Plugin Boundary

The implementation home is a plugin target, for example:

```text
plugins/afw_frame_resources/Plugin.cpp
plugins/afw_frame_resources/FrameResourceTracker.hpp
plugins/afw_frame_resources/FrameResourceTracker.cpp
plugins/afw_frame_resources/FrameResourceTypes.hpp
plugins/afw_frame_resources/providers/RenderTargetPoolProvider.hpp
plugins/afw_frame_resources/providers/RenderTargetPoolProvider.cpp
plugins/afw_frame_resources/providers/D3D12BindProvider.hpp
plugins/afw_frame_resources/providers/D3D12BindProvider.cpp
plugins/afw_frame_resources/providers/NgxDlssProvider.hpp        optional
plugins/afw_frame_resources/providers/NgxDlssProvider.cpp        optional
plugins/afw_frame_resources/include/UEVRFrameResourcesAPI.h
```

The build target should follow the repo's existing plugin pattern:

```toml
[target.afw_frame_resources]
type = "plugin"
sources = ["plugins/afw_frame_resources/**.cpp", "plugins/afw_frame_resources/**.c"]
headers = ["plugins/afw_frame_resources/**.hpp", "plugins/afw_frame_resources/**.h"]
```

The plugin should start from the existing UEVR plugin SDK:

```text
include/uevr/Plugin.hpp
include/uevr/API.h
include/uevr/API.hpp
examples/example_plugin/Plugin.cpp
```

It can also use local UEVRPureDark implementation details where that makes the API practical:

```text
dependencies/submodules/UESDK/src/sdk/StereoStuff.hpp
dependencies/pd-afwmod/include/PDAFWPlugin.h
src/mods/vr/RenderTargetPoolHook.hpp
src/mods/pluginloader/FRenderTargetPoolHook.cpp
src/mods/vr/UpscaleHelper.hpp                      optional NGX declarations only
src/hooks/D3D12Hook.hpp                             reference only unless a bridge is needed
```

Rule for these internals:

```text
Use them inside isolated provider/adapter files.
Do not expose their raw types in UEVRFrameResourcesAPI.h unless versioned.
Prefer copying the minimum layout/declaration needed into the plugin adapter when separate-DLL linking would be fragile.
If a direct internal symbol is not linkable from the plugin DLL, add a tiny bridge or use a layout adapter rather than moving the tracker into UEVR core.
```

The plugin can already get these useful surfaces without modifying UEVR core:

```text
on_initialize
on_present
on_post_render_vr_framework_dx12
on_device_reset
API::get()->param()->renderer.device
API::get()->param()->renderer.command_queue
API::RenderTargetPoolHook::activate()
API::RenderTargetPoolHook::get_render_target(...)
API::get()->functions->register_inline_hook(...)
API::get()->functions->unregister_inline_hook(...)
```

Core UEVR files are not the module location:

```text
Do not put the permanent tracker implementation in src/mods/vr.
Do not put the permanent tracker implementation in src/hooks.
Do not wire the DLSS hook in src/mods/VR.cpp as the primary data path.
Do not extend src/hooks/D3D12Hook.cpp for the first velocity probe.
Do not link the plugin against NGX/DLSS libraries.
Do not require nvngx.dll to be present for plugin startup.
```

Referencing, including, or adapting local UEVRPureDark code in the plugin is allowed. The restriction is against making the tracker a core UEVR fork feature instead of a plugin-owned module.

Allowed UEVR-core contact points, only if the plugin cannot otherwise feed AFW:

```text
1. A tiny optional AFW bridge in D3D12Component.cpp that asks the plugin for resources.
2. A tiny optional API table in include/uevr/API.h if a stable SDK callback is needed.
3. A generic callback/event bridge that does nothing when the plugin is absent.
```

Those bridge points are not the product. The product is the plugin/module DLL.

## Engine Source Reference Policy

Use this engine checkout as reference when the plugin needs to know how Unreal names, creates, binds, or encodes the resources:

```text
E:\Epic Games\UnrealEngine-5.6.1
```

Allowed use:

```text
Look up RDG texture names.
Look up render target formats and flags.
Look up when velocity/depth are bound.
Look up cvar names and intended behavior.
Look up velocity encode/decode math.
Look up whether a resource can be produced, black dummy, or absent.
```

Not allowed as a default implementation dependency:

```text
Do not include Engine\Source headers directly in the plugin build unless there is no local UEVRPureDark equivalent.
Do not link against Unreal Engine modules.
Do not require an Unreal Engine source checkout to build or run the plugin.
Do not call Unreal renderer C++ functions directly from the plugin unless UEVRPureDark already exposes them or a tiny bridge is deliberately added.
```

The correct pattern is:

```text
Use engine source to learn what to look for.
Call through UEVRPureDark plugin APIs, local UEVRPureDark layouts, D3D12 hooks, or a tiny optional UEVRPureDark bridge.
```

### UE 5.6.1 Reference Checklist

Depth resource:

```text
Engine\Source\Runtime\Renderer\Private\SceneTextures.cpp:457
Engine\Source\Runtime\Renderer\Private\SceneTextures.cpp:464
Engine\Source\Runtime\Renderer\Private\SceneTextures.cpp:474
Engine\Source\Runtime\Renderer\Private\SceneTextures.cpp:478
```

These show `SceneDepthZ` being registered or created. This validates `SceneDepthZ` as the first RenderTargetPool lookup name.

Velocity resource:

```text
Engine\Source\Runtime\Renderer\Internal\SceneTextures.h:142
Engine\Source\Runtime\Renderer\Private\SceneTextures.cpp:687
Engine\Source\Runtime\Renderer\Private\SceneTextures.cpp:991
Engine\Source\Runtime\Renderer\Private\SceneTextures.cpp:1078
```

These show `FSceneTextures::Velocity`, creation with debug name `SceneVelocity`, `ESceneTexture::Velocity`, and conditional setup when velocity has been produced.

Velocity helper:

```text
Engine\Source\Runtime\Renderer\Public\FXRenderingUtils.h:36
Engine\Source\Runtime\Renderer\Private\FXRenderingUtils.cpp:59
```

These show the engine-side helper for `GetSceneVelocityTexture(const FSceneView& View)`. The plugin cannot directly call this helper, but it documents the authoritative source: `ViewFamily->GetSceneTexturesChecked()->Velocity`.

Velocity pass binding:

```text
Engine\Source\Runtime\Renderer\Private\VelocityRendering.cpp:244
Engine\Source\Runtime\Renderer\Private\VelocityRendering.cpp:314
Engine\Source\Runtime\Renderer\Private\VelocityRendering.cpp:350
```

These show `RenderVelocities` and binding `SceneTextures.Velocity` as render target slot 0. This is the main evidence for what the plugin-owned D3D12 bind tracker should identify.

Velocity format and flags:

```text
Engine\Source\Runtime\Renderer\Private\VelocityRendering.cpp:382
Engine\Source\Runtime\Renderer\Private\VelocityRendering.cpp:400
Engine\Source\Runtime\Renderer\Private\VelocityRendering.cpp:406
Engine\Source\Runtime\Renderer\Private\VelocityRendering.cpp:865
```

These show `FVelocityRendering::GetFormat`, `GetCreateFlags`, and `GetRenderTargetDesc`. The plugin should use these as reference when scoring velocity candidates by DXGI format, dimensions, render-target capability, UAV capability, and shader-resource capability.

Force velocity cvar:

```text
Engine\Source\Runtime\Engine\Private\PrimitiveSceneProxy.cpp:80
```

This shows `r.Velocity.ForceOutput`. The plugin can set this through the UEVR public console API for diagnostics, but should not require it for normal operation.

Velocity encode/decode:

```text
Engine\Shaders\Private\Common.ush:2038
Engine\Shaders\Private\Common.ush:2043
Engine\Shaders\Private\Common.ush:2076
Engine\Shaders\Private\Common.ush:2102
```

These show `VELOCITY_ENCODE_HAS_PIXEL_ANIMATION`, `EncodeVelocityToTexture`, `DecodeVelocityFromTexture`, and pixel-animation metadata. Use this only when adding corrected velocity. Do not add corrected velocity before the plugin can first discover and expose raw non-DLSS velocity.

### How To Call The Pieces

Plugin startup:

```cpp
void on_initialize() override {
    auto* api = uevr::API::get();
    const auto* renderer = api->param()->renderer;

    tracker.initialize(
        static_cast<ID3D12Device*>(renderer->device),
        static_cast<ID3D12CommandQueue*>(renderer->command_queue));

    uevr::API::RenderTargetPoolHook::activate();
}
```

RenderTargetPool depth/velocity lookup:

```cpp
auto* pooled_depth = uevr::API::RenderTargetPoolHook::get_render_target(L"SceneDepthZ");
auto* pooled_velocity = uevr::API::RenderTargetPoolHook::get_render_target(L"SceneVelocity");
```

Then use the verified, link-safe extraction path (see "Verified Against Source", finding 1):

```cpp
// 1. The opaque handle IS the internal sdk::IPooledRenderTarget*. Reinterpret it through a
//    minimal copied layout (providers/PooledRenderTargetLayout.hpp) to reach item.texture.texture.
ID3D12Resource* native = nullptr;
if (pooled_depth != nullptr) {
    auto* rt = reinterpret_cast<MinPooledRenderTarget*>(pooled_depth); // vtable @0, item @ +0x08
    if (auto* rhi_tex = rt->item.texture.texture) {                    // FRHITexture2D*
        // 2. Convert FRHITexture2D* -> ID3D12Resource* via the plugin-facing SDK function table.
        //    This is implemented in the MAIN module, so the plugin links nothing extra.
        auto* api_tex = reinterpret_cast<uevr::API::FRHITexture2D*>(rhi_tex);
        native = static_cast<ID3D12Resource*>(api_tex->get_native_resource());
    }
}
```

The minimal layout struct lives in an isolated provider header and is the only piece of UESDK
knowledge the plugin copies. It must not appear in the exported `UEVRFrameResourcesAPI.h`.

D3D12 bind tracking:

```text
Use API::get()->functions->register_inline_hook.
Hook CreateRenderTargetView to map descriptor handle -> ID3D12Resource desc.
Hook OMSetRenderTargets / BeginRenderPass to observe when a velocity-shaped resource is bound.
Use the engine source references above to score candidates against SceneVelocity behavior.
```

Diagnostic cvar call:

```cpp
auto* console = uevr::API::get()->get_console_manager();
if (console != nullptr) {
    if (auto* force_velocity = console->find_variable(L"r.Velocity.ForceOutput")) {
        force_velocity->set(1);
    }
}
```

Exported module API call from AFW bridge or another plugin:

```cpp
auto* module = GetModuleHandleW(L"afw_frame_resources.dll");
auto* get_api = module
    ? reinterpret_cast<uevr_frame_resources_get_api_fn>(
        GetProcAddress(module, "uevr_frame_resources_get_api"))
    : nullptr;

UEVR_FrameResourcesApi frame_resources{};
if (get_api != nullptr && get_api(UEVR_FRAME_RESOURCES_API_VERSION, &frame_resources)) {
    UEVR_FrameResourceView depth{};
    UEVR_FrameResourceView velocity{};
    frame_resources.get_latest(UEVR_FRAME_RESOURCE_DEPTH, UEVR_FRAME_RESOURCE_EYE_UNKNOWN, &depth);
    frame_resources.get_latest(UEVR_FRAME_RESOURCE_VELOCITY, UEVR_FRAME_RESOURCE_EYE_UNKNOWN, &velocity);
}
```

AFW bridge rule:

```text
The optional AFW bridge may convert UEVR_FrameResourceView -> pd::TextureDesc.
That conversion belongs in a small adapter.
The plugin API remains raw D3D12 resource plus metadata.
If the plugin is absent, disabled, or reports invalid resources, AFW keeps the existing UEVRPureDark path.
```

## Important Correction

UEVRPureDark does not currently have `DIBRDepthTracker`.

Do not write code or plans as if this exists:

```text
src/hooks/DIBRDepthTracker.hpp
src/hooks/DIBRDepthTracker.cpp
```

The PureDark branch currently has:

```text
src/hooks/D3D12Hook.hpp
src/hooks/D3D12Hook.cpp
src/mods/VR.hpp
src/mods/VR.cpp
src/mods/vr/D3D12Component.hpp
src/mods/vr/D3D12Component.cpp
src/mods/vr/RenderTargetPoolHook.hpp
src/mods/vr/RenderTargetPoolHook.cpp
src/mods/vr/UpscaleHelper.hpp
dependencies/pd-afwmod/include/PDAFWPlugin.h
```

Therefore the correct plan is:

```text
1. Wrap and decouple the depth/MV resources that PureDark already captures.
2. Use existing RenderTargetPoolHook for depth fallback.
3. Add new D3D12 descriptor/bind tracking only if needed for DLSS-off velocity.
4. Do not assume preexisting velocity snapshot infrastructure.
```

## Current UEVRPureDark State

### DLSS Resource Capture

Current DLSS hook path:

```text
E:\Github\UEVRPureDark\src\mods\VR.cpp
  hk_NVSDK_NGX_D3D12_CreateFeature(...)
  hk_NVSDK_NGX_D3D12_ReleaseFeature(...)
  hk_NVSDK_NGX_D3D12_EvaluateFeature(...)
```

The EvaluateFeature hook reads:

```text
NVSDK_NGX_Parameter_Color
NVSDK_NGX_Parameter_Depth
NVSDK_NGX_Parameter_MotionVectors
NVSDK_NGX_Parameter_Output
NVSDK_NGX_Parameter_MV_Scale_X
NVSDK_NGX_Parameter_MV_Scale_Y
```

Then it stores:

```cpp
vr->rawDepthTex = depth;
vr->rawMotionVectorsTex = motionVectors;
vr->mvScale[0] = ...
vr->mvScale[1] = ...
```

It also copies DLSS-provided depth and motion vectors into AFW-owned descriptors when those descriptors already exist:

```cpp
vr->d3d12Renderer->Copy(InCmdList, vr->depthDesc[nEye], src);
vr->d3d12Renderer->Copy(InCmdList, vr->motionVectorsDesc[nEye], src);
```

### Current Global Fields

Current fields in `VR`:

```text
E:\Github\UEVRPureDark\src\mods\VR.hpp

CameraData cameraData[2]
D3D12RendererAPI* d3d12Renderer
ID3D12Resource* rawDepthTex
ID3D12Resource* rawMotionVectorsTex
TextureDesc uiBufferDesc
TextureDesc depthDesc[2]
TextureDesc motionVectorsDesc[2]
float mvScale[2]
std::map<NVSDK_NGX_Handle*, NVSDK_NGX_Feature> vrDLSSHandleMap
```

These are useful but too coupled:

```text
They live on VR.
They assume DLSS as the primary source for motion vectors.
They mix resource observation, allocation, copying, and AFW consumption.
They are not a clean API for plugins or separate modules.
```

### Current AFW Consumption

Current AFW code in `D3D12Component.cpp`:

```text
E:\Github\UEVRPureDark\src\mods\vr\D3D12Component.cpp
```

Important behavior:

```text
1. If rawDepthTex is null, try RenderTargetPoolHook SceneDepthZ.
2. If rawDepthTex exists, allocate depthDesc[2].
3. If rawMotionVectorsTex exists, allocate motionVectorsDesc[2].
4. Build a PureDark FrameBufferDesc.
5. Pass depthDesc[nEye] and motionVectorsDesc[nEye] to EvaluateFrameWarp.
```

Relevant flow:

```cpp
if (!vr->rawDepthTex) {
    auto& rt_pool = vr->get_render_target_pool_hook();
    scene_depth_tex = rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ");
    if (scene_depth_tex) {
        vr->rawDepthTex = scene_depth_tex.Get();
    }
}

if (vr->rawDepthTex) {
    allocate vr->depthDesc[0/1]
}

if (vr->rawMotionVectorsTex) {
    allocate vr->motionVectorsDesc[0/1]
}

s_CurrentEyeFrameBuffer.depth = vr->depthDesc[nEye];
s_CurrentEyeFrameBuffer.motionVectors = vr->motionVectorsDesc[nEye];
params.InMotionScale[0] = vr->mvScale[0];
params.InMotionScale[1] = vr->mvScale[1];
EvaluateFrameWarp(params);
```

### Current RenderTargetPoolHook

UEVRPureDark already has an engine-level render target pool hook:

```text
src/mods/vr/RenderTargetPoolHook.hpp
src/mods/vr/RenderTargetPoolHook.cpp
src/mods/pluginloader/FRenderTargetPoolHook.cpp
include/uevr/API.h
include/uevr/API.hpp
```

Useful existing API:

```cpp
template<typename T>
Microsoft::WRL::ComPtr<T> get_texture(const std::wstring& name);
```

Plugin-facing existing API:

```c
UEVR_IPooledRenderTargetHandle (*get_render_target)(const wchar_t* name);
```

C++ wrapper:

```cpp
API::RenderTargetPoolHook::activate();
API::RenderTargetPoolHook::get_render_target(const wchar_t* name);
```

This is enough to query known render target pool names such as:

```text
SceneDepthZ
```

It may or may not be enough to query velocity names, depending on engine version and render path:

```text
SceneVelocity
GBufferVelocity
Velocity
```

The first implementation should try these names and log the result, but should not pretend this is a complete velocity tracker.

### Current D3D12Hook

UEVRPureDark's `D3D12Hook` currently hooks:

```text
Present
Present1
ResizeBuffers
ResizeTarget
```

It tracks:

```text
ID3D12Device4* device
IDXGISwapChain3* swapchain
ID3D12CommandQueue* command_queue
display/render sizes
frame generation swapchain flag
```

It does not currently hook:

```text
ID3D12Device::CreateRenderTargetView
ID3D12Device::CreateDepthStencilView
ID3D12Device::CreateShaderResourceView
ID3D12GraphicsCommandList::OMSetRenderTargets
ID3D12GraphicsCommandList4::BeginRenderPass
ID3D12CommandQueue::ExecuteCommandLists
```

That means UEVRPureDark currently cannot reliably discover a velocity render target by D3D12 bind signatures unless we add new hook infrastructure.

## Existing PureDark AFW API

PureDark's AFW interface is in:

```text
E:\Github\UEVRPureDark\dependencies\pd-afwmod\include\PDAFWPlugin.h
```

Important types:

```cpp
struct TextureDesc {
    ImageType type;
    ID3D12Resource* pTexture;
    int srvPos;
    int uavPos;
    D3D12_GPU_DESCRIPTOR_HANDLE shaderResourceViewHandle;
    D3D12_GPU_DESCRIPTOR_HANDLE unorderedAccessViewHandle;
    D3D12_CPU_DESCRIPTOR_HANDLE renderTargetViewHandle;
    D3D12_RESOURCE_STATES initialState;
};

struct FrameBufferDesc {
    TextureDesc color;
    TextureDesc depth;
    TextureDesc motionVectors;
};

enum MVType {
    Normal,
    FromOtherEye,
    ObjectOnly
};

struct FrameWarpEvaluateParams {
    void* InCmdList;
    FrameBufferDesc* InEyeFrameBuffer;
    FrameBufferDesc* OutEyeFrameBuffer;
    TextureDesc* InUIColorAlpha;
    float InMotionScale[2];
    FrameWarpMode Mode;
    EyeIndex EyeIndex;
    CameraData* CameraData;
    MVType MotionVectorsType;
};
```

Important functions:

```cpp
D3D12RendererAPI* InitDevice(DeviceParams params);
EyeFrameBuffers InitFrameWarp(FrameWarpInitParams params);
void EvaluateFrameWarp(FrameWarpEvaluateParams& params);
```

Important renderer helper methods:

```cpp
SetupTextureDesc(TextureDesc& srcDesc)
CreateTexture(...)
Copy(...)
CorrectMotionVectors(...)
```

The tracker should not be defined in terms of `pd::TextureDesc`. It should expose raw D3D12 resources plus metadata, then have a PureDark adapter that builds `TextureDesc`.

## Proposed Module

### Name

Use:

```text
FrameResourceTracker
```

Avoid:

```text
DIBRDepthTracker
DLSSMotionTracker
AFWDepthTracker
```

Reason:

```text
This module is not only for DIBR.
This module is not only for DLSS.
This module should be useful for AFW, future corrected velocity, and plugins.
```

### Location

Add:

```text
plugins/afw_frame_resources/Plugin.cpp
plugins/afw_frame_resources/FrameResourceTracker.hpp
plugins/afw_frame_resources/FrameResourceTracker.cpp
plugins/afw_frame_resources/FrameResourceTypes.hpp
plugins/afw_frame_resources/ExportedFrameResourceApi.hpp
plugins/afw_frame_resources/providers/RenderTargetPoolProvider.*
plugins/afw_frame_resources/providers/D3D12BindProvider.*
plugins/afw_frame_resources/providers/NgxDlssProvider.*          optional
```

Do not start in `src/mods/vr` and split later. That would turn the design into a fork-first change. Start as a plugin target so the boundaries are forced from the first patch.

### Responsibilities

The tracker should own:

```text
Provider selection
Resource metadata
Latest depth resource
Latest velocity resource
Per-eye copies where needed
DLSS observed resource state
RenderTargetPool fallback state
D3D12 bind/descriptor candidate state
Diagnostics and skip reasons
```

The tracker should not own:

```text
AFW rendering policy
PureDark UI
OpenXR submission
UEVR rendering method selection
Hard dependency on DLSS/NGX
```

### Resource Providers

Initial provider enum:

```cpp
enum class FrameResourceProvider : uint32_t {
    None,
    RenderTargetPool,
    D3D12Bind,
    InternalCopy,
    DlssNgx,           // optional observer only; never required
};
```

Resource kind enum:

```cpp
enum class FrameResourceKind : uint32_t {
    Depth,
    Velocity,
    CorrectedVelocity,
    Color,
    Output,
};
```

Eye enum:

```cpp
enum class FrameResourceEye : uint32_t {
    Unknown,
    Left,
    Right,
    Both,
};
```

Validity enum:

```cpp
enum class FrameResourceValidity : uint32_t {
    Invalid,
    Valid,
    MissingProvider,
    MissingResource,
    WrongRenderer,
    WrongFormat,
    WrongSize,
    NotReady,
    Stale,
};
```

Resource view:

```cpp
struct FrameResourceView {
    FrameResourceKind kind{};
    FrameResourceProvider provider{};
    FrameResourceValidity validity{};
    FrameResourceEye eye{FrameResourceEye::Unknown};

    ID3D12Resource* resource{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    uint32_t width{};
    uint32_t height{};

    D3D12_RESOURCE_STATES expected_state{D3D12_RESOURCE_STATE_COMMON};

    float motion_scale_x{1.0f};
    float motion_scale_y{1.0f};

    uint32_t render_frame{};
    const char* debug_name{};
    const char* reason{};
};
```

### Plugin-Internal API

```cpp
class FrameResourceTracker {
public:
    static FrameResourceTracker& get();

    void initialize(ID3D12Device* device, ID3D12CommandQueue* queue);
    void reset();

    void begin_frame(uint32_t render_frame);
    void set_current_eye(FrameResourceEye eye);

    void observe_resource(const ObservedFrameResource& resource);
    void observe_missing(FrameResourceKind kind, FrameResourceProvider provider, const char* reason);

    FrameResourceView get_latest(FrameResourceKind kind, FrameResourceEye eye = FrameResourceEye::Unknown) const;

    std::string describe_state() const;
};
```

Generic observation payload:

```cpp
struct ObservedFrameResource {
    FrameResourceKind kind{};
    FrameResourceProvider provider{};
    FrameResourceEye eye{FrameResourceEye::Unknown};

    ID3D12GraphicsCommandList* cmd{};
    ID3D12Resource* resource{};

    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    uint32_t width{};
    uint32_t height{};
    D3D12_RESOURCE_STATES expected_state{D3D12_RESOURCE_STATE_COMMON};

    float mv_scale_x{1.0f};
    float mv_scale_y{1.0f};

    uint32_t render_frame{};
    const char* debug_name{};
};
```

DLSS/NGX, if enabled, converts its observed depth/MV into this generic payload. The tracker core does not know whether NGX is installed.

### PureDark Adapter

Adapter role:

```text
Convert FrameResourceView into pd::TextureDesc.
Call d3d12Renderer->SetupTextureDesc where needed.
Allocate owned pd::TextureDesc copies where needed.
Preserve existing AFW behavior while moving ownership out of VR.
```

This adapter is not the tracker core. The core API returns raw D3D12 resources plus metadata and can be used without PureDark AFW. AFW-specific `pd::TextureDesc` conversion should live in an optional adapter/bridge layer.

Example:

```cpp
struct PdFrameResourceTexture {
    pd::TextureDesc desc{};
    bool valid{};
};

PdFrameResourceTexture make_pd_texture_desc(
    pd::D3D12RendererAPI* renderer,
    const FrameResourceView& view,
    D3D12_RESOURCE_STATES state);
```

The first pass can still use the existing `pd::D3D12RendererAPI::Copy` path. The important change is that AFW asks the tracker where the resource came from.

## Build Plan

### Phase 1: Plugin Skeleton, No DLSS

Add a new plugin target and a minimal `uevr::Plugin` implementation.

Files:

```text
plugins/afw_frame_resources/Plugin.cpp
plugins/afw_frame_resources/FrameResourceTracker.*
plugins/afw_frame_resources/FrameResourceTypes.hpp
plugins/afw_frame_resources/include/UEVRFrameResourcesAPI.h
cmake.toml
```

The plugin must:

```text
Load when DLSS is disabled.
Load when nvngx.dll and _nvngx.dll are absent.
Use only UEVR's plugin SDK, D3D12 headers, and local repo dependencies.
Expose a C ABI getter such as uevr_frame_resources_get_api().
Return "missing" for depth/velocity until providers are enabled.
```

It must not:

```text
Link any NGX/NVIDIA import library.
Call LoadLibrary for nvngx.dll as a startup requirement.
Fail initialization when NGX is missing.
```

It may include local UEVRPureDark headers when useful. If `src/mods/vr/UpscaleHelper.hpp` is used, keep that usage inside the optional NGX observer provider, not the core tracker or required startup path.

Verification:

```text
Build plugin target.
Load plugin with DLSS disabled.
Log: [FrameResources] initialized renderer=D3D12 dlss_required=false ngx_present=false.
```

### Phase 2: RenderTargetPool Provider, No DLSS

Add `RenderTargetPoolProvider`.

Use existing plugin SDK entrypoints:

```cpp
API::RenderTargetPoolHook::activate();
API::RenderTargetPoolHook::get_render_target(L"SceneDepthZ");
```

Probe:

```text
SceneDepthZ
SceneVelocity
GBufferVelocity
Velocity
```

The repo's public SDK exposes pooled render target handles. It does NOT expose a
`get_render_target`-style native-resource accessor for pooled targets directly, BUT it does
expose `frhitexture2d->get_native_resource(handle)` for any `FRHITexture2D*` (API.h:432). Combined
with the fact that the pooled-target handle is the internal `sdk::IPooledRenderTarget*`
(FRenderTargetPoolHook.cpp:15), the extraction is fully resolved — no new SDK bridge needed:

```text
RESOLVED PATH (use this; supersedes the old Option A/B/C):
  1. handle = API::RenderTargetPoolHook::get_render_target(L"SceneDepthZ")
  2. rt     = reinterpret_cast<MinPooledRenderTarget*>(handle)     // minimal copied layout
  3. rhi    = rt->item.texture.texture                            // FRHITexture2D* (may be null)
  4. native = ((uevr::API::FRHITexture2D*)rhi)->get_native_resource()  // ID3D12Resource*, via SDK table
```

Rules for the provider:

```text
Keep MinPooledRenderTarget in providers/PooledRenderTargetLayout.hpp; it is the ONLY copied UESDK layout.
Validate every pointer hop (handle, item.texture.texture, native) and log the exact hop that failed.
Do not leak MinPooledRenderTarget / FRHITexture2D through the exported module API.
If a future engine version shifts the layout, only PooledRenderTargetLayout.hpp changes.
Patch 6 (a get_native_resource(name) SDK bridge) is now OPTIONAL and only justified if this
  layout proves brittle across titles. The resolved path is the default.
```

Validity rules:

```text
Depth is valid only when a real ID3D12Resource* is obtained and desc looks like a depth/scene-sized texture.
Velocity is valid only when a real ID3D12Resource* is obtained and desc looks like a velocity/scene-sized texture.
Missing velocity is a valid diagnostic result.
```

Verification:

```text
DLSS off:
  SceneDepthZ either becomes a valid depth resource or logs an exact extraction failure.
  Velocity names either become valid resources or log exact misses.
```

### Phase 3: Plugin-Owned D3D12 Bind Provider, No DLSS

Add `D3D12BindProvider` inside the plugin. It should use UEVR's plugin hook API:

```cpp
API::get()->functions->register_inline_hook(...);
API::get()->functions->unregister_inline_hook(...);
```

Do not modify `src/hooks/D3D12Hook.cpp` for the first implementation.

Hook candidates from the plugin:

```text
ID3D12Device::CreateRenderTargetView
ID3D12Device::CreateDepthStencilView
ID3D12GraphicsCommandList::OMSetRenderTargets
ID3D12GraphicsCommandList4::BeginRenderPass
ID3D12CommandQueue::ExecuteCommandLists
```

Minimal state:

```cpp
struct D3D12ViewInfo {
    ID3D12Resource* resource{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    uint32_t width{};
    uint32_t height{};
    uint32_t sample_count{};
    D3D12_RESOURCE_FLAGS flags{};
    D3D12_RESOURCE_STATES observed_state{D3D12_RESOURCE_STATE_COMMON};
};

std::unordered_map<SIZE_T, D3D12ViewInfo> rtv_views;
std::unordered_map<SIZE_T, D3D12ViewInfo> dsv_views;
```

At `CreateRenderTargetView`:

```text
record descriptor.ptr -> resource desc
```

At `CreateDepthStencilView`:

```text
record descriptor.ptr -> resource desc
```

At `OMSetRenderTargets` and `BeginRenderPass`:

```text
resolve bound RTV/DSV descriptor handles
track likely scene depth
track likely velocity-shaped RTVs
emit observations into FrameResourceTracker
```

Velocity-shaped candidates:

```text
DXGI_FORMAT_R16G16B16A16_UNORM
DXGI_FORMAT_R16G16_UNORM
DXGI_FORMAT_R16G16_FLOAT
scene-sized or SceneDepthZ-sized
sample count == 1
render-target capable
not a swapchain backbuffer
```

This phase is log-only for velocity:

```text
[FrameResources] d3d12 velocity candidate ptr=... WxH fmt=... source=OMSetRenderTargets
```

It should expose the latest candidate as `valid_candidate` only if confidence rules pass. It should not copy or mutate resources yet.

### Phase 4: Exported Module API

Expose a stable C API from the plugin DLL:

```c
extern "C" __declspec(dllexport)
bool uevr_frame_resources_get_api(uint32_t version, UEVR_FrameResourcesApi* out);
```

Core calls:

```c
bool get_latest(uint32_t kind, uint32_t eye, UEVR_FrameResourceView* out);
const char* describe_state();
void set_options(const UEVR_FrameResourceOptions* options);
```

Other plugins can use:

```text
GetModuleHandleW(L"afw_frame_resources.dll")
GetProcAddress(..., "uevr_frame_resources_get_api")
```

This API must work even when the only active providers are RenderTargetPool and D3D12 bind tracking.

### Phase 5: Optional AFW Bridge

Only if AFW must consume the module from existing `D3D12Component.cpp`, add a small bridge.

Bridge behavior:

```text
Look for the plugin DLL export at runtime.
If absent, keep current UEVR/PureDark behavior.
If present and enabled, ask plugin for depth/velocity.
Convert raw resource metadata to pd::TextureDesc in a tiny adapter.
Log provider and validity.
```

The bridge should be behind an env/config gate:

```text
UEVR_AFW_FRAME_RESOURCES=1
UEVR_AFW_FRAME_RESOURCES_LEGACY_FALLBACK=1
```

Current AFW path remains the fallback:

```text
VR::rawDepthTex
VR::rawMotionVectorsTex
VR::depthDesc[nEye]
VR::motionVectorsDesc[nEye]
```

DLSS-off behavior:

```text
If plugin depth valid:
  use plugin depth.

If plugin velocity invalid:
  pass no fake velocity and keep the existing AFW fallback mode explicit.

If plugin velocity valid:
  use plugin velocity and log provider.
```

### Phase 6: Optional NGX/DLSS Observer

Add `NgxDlssProvider` only after the no-DLSS module path works.

It must be dynamic:

```text
Check GetModuleHandleW(L"_nvngx.dll") and GetModuleHandleW(L"nvngx.dll").
If neither exists, log once and skip.
Resolve NGX functions with GetProcAddress only when the module is already loaded.
Install hooks with register_inline_hook only when enabled.
Never require DLSS to initialize the plugin.
```

It must not be the first provider in default selection.

Provider priority should be configurable:

```text
Default DLSS-off test:
  RenderTargetPool
  D3D12Bind
  InternalCopy
  DlssNgx disabled

Optional comparison:
  RenderTargetPool
  D3D12Bind
  DlssNgx
```

The purpose of this provider is comparison and diagnostics:

```text
Compare DLSS-provided MV against non-DLSS SceneVelocity/D3D12 candidates.
Prove whether moving-object smearing follows DLSS MV, non-DLSS velocity, or AFW fallback.
```

### Phase 7: Same-Frame Velocity Snapshot, No DLSS

If log-only D3D12 velocity discovery works, add snapshots in the plugin.

Snapshot goal:

```text
Copy the completed velocity target after velocity draws, not at the bind that starts the velocity pass.
```

Because UEVRPureDark does not currently expose a post-velocity marker, the first implementation must be conservative.

Possible copy points:

```text
Option A:
  copy when the next non-velocity SceneColor/depth bind appears

Option B:
  copy before AFW uses the resource if command ordering is known safe

Option C:
  no snapshot yet, only expose raw source and log risk
```

Do not use DLSS EvaluateFeature as the required copy point. That would reintroduce a DLSS dependency.

Recommended first implementation:

```text
Do not snapshot in Phase 3.
Only identify and log candidate velocity resources.
Add snapshots as a separate Phase 7 after RenderDoc/log evidence shows a safe copy point.
```

This avoids silently copying stale content.

### Phase 8: Corrected Velocity Later

Do not build corrected velocity until:

```text
Tracker exists.
AFW consumes tracker resources.
DLSS-off depth behavior is proven.
DLSS-off velocity availability is known.
```

Future corrected-velocity concept:

```text
Decode UE velocity.
Predict camera/static velocity for same-eye history.
Subtract to isolate object residual.
Scale residual for AFR/AFW history interval.
Re-encode velocity into an owned corrected texture.
Feed corrected texture to AFW.
```

Use PureDark API if available:

```cpp
D3D12RendererAPI::CorrectMotionVectors(...)
```

But first verify what it expects and whether it corrects the same problem.

## Debug Mode And Verification

Every phase ships with a way to prove it works before the next phase is built. There are three
verification tiers: (1) a build-time/offline self-test that needs no game, (2) structured debug
logging gated by level, and (3) a per-phase in-game checklist. Nothing is "done" until its tier-1
and tier-2 checks pass and the relevant tier-3 line appears in the UEVR log.

### Debug Mode

The plugin reads its configuration once at `on_initialize` from environment variables (so it can be
flipped without a rebuild) and exposes the same switches through `set_options()` on the exported API.

```text
UEVR_FRAME_RESOURCES=1                     master enable (default on; 0 = load but stay inert)
UEVR_FRAME_RESOURCES_LOG=<0..3>            log level: 0 silent, 1 info (default), 2 debug, 3 trace
UEVR_FRAME_RESOURCES_SELFTEST=1            run the offline self-test at init and log PASS/FAIL, then continue
UEVR_FRAME_RESOURCES_ENABLE_RTPOOL=1       enable RenderTargetPool provider (default 1)
UEVR_FRAME_RESOURCES_ENABLE_D3D12BIND=1    enable D3D12 bind provider (default 1)
UEVR_FRAME_RESOURCES_ENABLE_DLSS_OBSERVER=0  enable optional NGX observer (default 0; never required)
UEVR_FRAME_RESOURCES_FORCE_VELOCITY=0      set r.Velocity.ForceOutput=1 via console for diagnostics
UEVR_FRAME_RESOURCES_DUMP_EVERY=0          if N>0, log describe_state() every N frames
```

Log-level semantics (all lines prefixed `[FrameResources]`, emitted via `API::get()->log_*`):

```text
level 1 (info):  one-time provider init/teardown, first valid depth/velocity per provider,
                 first explicit "missing" per kind, self-test result, option changes.
level 2 (debug): per-provider per-frame resolution result (provider, validity, reason),
                 descriptor/bind candidate acceptance/rejection with the failing score rule.
level 3 (trace): every CreateRTV/DSV record and every OMSetRenderTargets/BeginRenderPass bind,
                 every pointer hop in the pooled-target walk. High volume; short test windows only.
```

Use `SPDLOG_INFO_ONCE`-style "log once per distinct key" dedup so trace mode does not flood: a
candidate (ptr,WxH,fmt) tuple logs once until it changes. Counters (`accepts`, `rejects`,
`hops_failed`) accumulate and print in the periodic `describe_state()` dump.

`describe_state()` is the single human-readable snapshot. It must always be safe to call (even
before init) and must report, per kind: current provider, validity + reason, resource ptr,
WxH/format, render_frame age (current_frame - resource.render_frame), and per-provider counters.

### Tier 1 — Offline Self-Test (no game required)

`UEVR_FRAME_RESOURCES_SELFTEST=1` runs `FrameResourceTracker::run_self_test()` at init. It exercises
the pure state machine with fabricated observations (dummy non-null `ID3D12Resource*` sentinels —
never dereferenced) and asserts the contracts:

```text
T1.1  Fresh tracker: get_latest(Depth/Velocity) == validity MissingProvider, resource == null.
T1.2  observe_missing(Velocity, RenderTargetPool, "no pool name"): get_latest(Velocity) == MissingResource,
      reason string round-trips, resource == null.
T1.3  observe_resource(Depth, RenderTargetPool, frame=10, 1920x1080, D32_FLOAT): get_latest(Depth) == Valid,
      ptr/format/size match, provider == RenderTargetPool.
T1.4  Staleness: begin_frame(10) then advance to begin_frame(13) without re-observing ->
      get_latest(Depth) validity == Stale (age beyond max_stale_frames), ptr still returned for inspection.
T1.5  Provider priority: with both RenderTargetPool depth and D3D12Bind depth observed same frame,
      get_latest returns the higher-priority provider per the configured order; flipping set_options order flips the winner.
T1.6  Eye routing: observe Left + Right separately; get_latest(kind, Left/Right/Unknown) returns the right one;
      Unknown returns most-recent.
T1.7  Exported ABI: uevr_frame_resources_get_api(WRONG_VERSION,&api)==false; (CURRENT,&api)==true and
      populates all fn pointers; get_latest through the C struct mirrors the C++ result for T1.3.
T1.8  describe_state() is non-null and contains each kind's provider+validity tokens.
```

Result line: `[FrameResources] selftest: 8/8 PASS` (or `FAIL t1.4 expected Stale got Valid`). A FAIL
must not crash the host — it logs and the plugin continues inert for the failed subsystem.

This is the regression net: it runs on every launch when enabled and on the build machine via a
tiny `--selftest` harness exe (optional `[target.afw_frame_resources_selftest] type="executable"`
linking the same core .cpp, calling `run_self_test()` and returning non-zero on failure) so CI/local
builds catch state-machine breakage without launching a game.

### Tier 2 — Structured Logging Assertions

For each phase, the success/failure is a specific log line at level >= the noted level. These are
the exact strings the in-game checklist greps for:

```text
Phase 1 (info): [FrameResources] init renderer=D3D12 dlss_required=false ngx_present=<0|1> providers=<list>
Phase 1 (info): [FrameResources] api ready version=<n>
Phase 2 (info): [FrameResources] depth provider=RenderTargetPool name=SceneDepthZ ptr=0x.. 1920x1080 fmt=<n>
Phase 2 (info): [FrameResources] velocity probe: SceneVelocity=<hit|miss> GBufferVelocity=<..> Velocity=<..>
Phase 2 (debug): [FrameResources] rtpool walk failed at <handle|item.texture.texture|native> for name=<..>
Phase 3 (debug): [FrameResources] d3d12 rtv record ptr=0x.. 1920x1080 fmt=<n> flags=<..>
Phase 3 (info): [FrameResources] d3d12 velocity candidate ptr=0x.. WxH fmt=<n> source=OMSetRenderTargets conf=<score>
Phase 3 (info): [FrameResources] d3d12 no velocity candidate this scene (scored=<n> rejected=<n>)
```

### Tier 3 — Per-Phase In-Game Test (the canonical loop)

Deploy = copy `build/.../afw_frame_resources.dll` into the game's `plugins/` folder
(PluginLoader.cpp:1724). Reuse the existing repo skill `sn2-fog-experiment` shape: launch the game
via the canonical launcher with the env above, let the scene render, then grep the UEVR log for the
phase's tier-2 line. Each phase has a single pass condition:

```text
Phase 1 PASS: with DLSS disabled AND nvngx.dll/_nvngx.dll absent or ignored, the plugin loads,
              logs "init ... dlss_required=false", "api ready", and self-test 8/8 PASS. No host
              behavior change vs. baseline (AFW still runs exactly as before).
Phase 2 PASS: log shows depth provider=RenderTargetPool name=SceneDepthZ with a non-null ptr and a
              plausible scene-sized desc, OR an exact "rtpool walk failed at <hop>" — never silence.
              Velocity probe logs an explicit hit/miss per name.
Phase 3 PASS: log shows either a velocity candidate with a confidence score, or an explicit "no
              velocity candidate". CreateRTV/bind hooks install and uninstall cleanly (no crash on
              device reset / alt-tab). Verify hook removal in on_device_reset.
Bridge PASS: with UEVR_AFW_FRAME_RESOURCES=0 the in-game image and log are byte-for-byte the baseline
             (proves zero-impact-when-off). With =1, AFW logs "AFW depth=valid provider=..." and the
             chosen velocity provider; toggling LEGACY_FALLBACK reproduces the old path.
```

Cross-check tooling already wired into this repo (optional, for deeper proof than logs):

```text
RenderDoc / PIX: confirm the ptr logged as SceneDepthZ is the same resource the engine binds as DSV;
                 confirm a logged velocity candidate is actually written by a velocity pass (not stale).
mcp uevr_render_* tools: uevr_render_resources / uevr_render_eye_dump to compare the tracker's chosen
                 resource against what UEVR sees per eye.
gemma-screen-observer / take_screenshot: capture both eyes to judge smear before/after, per the
                 existing DLSS-off interpretation table below.
```

Regression guard for the "do no harm" rule: a baseline capture (plugin absent) and a plugin-present-
but-disabled capture must match. This is the concrete test behind every "no behavior change when
disabled" acceptance criterion in this document.

## DLSS-Off Test Plan

### Purpose

Answer:

```text
Can AFW run without using DLSS-provided motion vectors?
Does AFW get a valid depth resource without DLSS?
Does this game expose a velocity texture through the current UEVRPureDark surfaces?
```

### Test Settings

```text
DLSS: disabled in game
DLSS Frame Generation: disabled
AFW: enabled
FrameResourceTracker: enabled
Legacy fallback: enabled at first
Velocity correction: disabled
```

Suggested env:

```text
UEVR_FRAME_RESOURCES=1
UEVR_FRAME_RESOURCES_LOG=1
UEVR_FRAME_RESOURCES_ENABLE_DLSS_OBSERVER=0
UEVR_FRAME_RESOURCES_LEGACY_FALLBACK=1
```

If adding a force-velocity cvar using existing UEVR SDK cvar helpers:

```text
UEVR_FRAME_RESOURCES_FORCE_VELOCITY=1
```

The plugin can use the public SDK console path:

```cpp
auto* console = API::get()->get_console_manager();
if (console != nullptr) {
    if (auto* force_velocity = console->find_variable(L"r.Velocity.ForceOutput")) {
        force_velocity->set(1);
    }
}
```

If that cvar is not found in a title, log it and continue.

### Expected Logs

Depth success:

```text
[FrameResources] depth provider=RenderTargetPool name=SceneDepthZ ptr=... WxH fmt=...
```

DLSS observer absent or disabled:

```text
[FrameResources] dlss observer disabled or NGX not loaded; continuing
```

Velocity by pool name:

```text
[FrameResources] velocity provider=RenderTargetPool name=SceneVelocity ptr=... WxH fmt=...
```

Velocity missing:

```text
[FrameResources] velocity missing: no non-DLSS velocity provider resolved
```

AFW consumption:

```text
[FrameResources] AFW depth=valid provider=...
[FrameResources] AFW velocity=invalid reason=...
```

### Interpretation

If DLSS off and depth valid:

```text
The tracker achieved the first decoupling milestone.
AFW no longer depends on DLSS for depth.
```

If DLSS off and velocity missing:

```text
This is expected with the current repo.
It means RenderTargetPool name lookup was not enough and D3D12 bind discovery/snapshot work is needed.
It does not mean AFW is fixed.
```

If DLSS off and velocity valid by pool name:

```text
Great. AFW can test non-DLSS velocity immediately.
Still verify freshness before relying on it for motion-sensitive work.
```

If moving-object smear persists with DLSS off:

```text
Then the smear is not only DLSS-provided MV quality.
Next compare AFW fallback modes and velocity availability.
```

If smear improves with DLSS off:

```text
Then DLSS path or DLSS-fed motion vectors are likely contributing.
The tracker can isolate DLSS-provided MV vs UE-provided SceneVelocity.
```

## Minimal Patch Sequence

### Patch 1: Add Plugin Target And Empty API

Files:

```text
plugins/afw_frame_resources/Plugin.cpp
plugins/afw_frame_resources/include/UEVRFrameResourcesAPI.h
plugins/afw_frame_resources/FrameResourceTracker.*
cmake.toml
```

Changes:

```text
Add plugin target using the existing type = "plugin" pattern.
Export uevr_frame_resources_get_api().
Return explicit missing-resource states.
No DLSS code.
No core UEVR code changes.
```

### Patch 2: Add RenderTargetPool Provider

Files:

```text
plugins/afw_frame_resources/providers/RenderTargetPoolProvider.*
plugins/afw_frame_resources/FrameResourceTracker.*
```

Changes:

```text
Activate RenderTargetPoolHook from plugin.
Probe SceneDepthZ and velocity names.
Extract native ID3D12Resource through an isolated adapter or log why extraction is unavailable.
No DLSS code.
```

### Patch 3: Add Plugin-Owned D3D12 Bind Provider

Files:

```text
plugins/afw_frame_resources/providers/D3D12BindProvider.*
plugins/afw_frame_resources/FrameResourceTracker.*
```

Changes:

```text
Use register_inline_hook from the plugin SDK.
Hook descriptor/view and bind functions from the plugin.
Log likely depth and velocity candidates.
No edits to src/hooks/D3D12Hook.cpp.
No DLSS code.
```

### Patch 4: DLSS-Off Diagnostic Run

Files:

```text
plugins/afw_frame_resources/*
```

Changes:

```text
Run plugin with DLSS disabled.
Confirm depth valid or explicitly missing.
Confirm velocity valid/candidate/missing without NGX.
Capture logs before any AFW bridge work.
```

### Patch 5: Optional AFW Bridge

Status: the bridge helper is BUILT and compile-verified, but deliberately NOT yet wired into the AFW
render path (it is dead code so the core is byte-for-byte unchanged). Wiring is the validated next
step and must be confirmed in-game.

Files already added:

```text
src/mods/vr/AFWFrameResourcesBridge.hpp   (runtime loader + env gates; includes the plugin C ABI)
src/mods/vr/AFWFrameResourcesBridge.cpp   (GetModuleHandle/GetProcAddress only; no plugin link)
```

The bridge exposes:

```cpp
uevr_afw_bridge::enabled();           // UEVR_AFW_FRAME_RESOURCES (default OFF)
uevr_afw_bridge::legacy_fallback();   // UEVR_AFW_FRAME_RESOURCES_LEGACY_FALLBACK (default ON)
uevr_afw_bridge::available();         // plugin DLL loaded + API-compatible
uevr_afw_bridge::get_latest_depth(UEVR_FrameResourceView*);
uevr_afw_bridge::get_latest_velocity(UEVR_FrameResourceView*);
uevr_afw_bridge::describe_state();
```

The remaining wiring edit (apply ONLY after an in-game DLSS-off run validates the plugin views).
Insert near the depth-fallback block in `src/mods/vr/D3D12Component.cpp` (currently around line 447,
right after the existing `rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ")` fallback):

```cpp
// OPTIONAL plugin-sourced depth (gated; falls back to the existing path on miss/disable).
if (uevr_afw_bridge::enabled() && uevr_afw_bridge::available()) {
    UEVR_FrameResourceView d{};
    const bool ok = uevr_afw_bridge::get_latest_depth(&d);
    spdlog::info("[FrameResources] AFW depth={} provider={}",
                 ok ? "valid" : "invalid", (int)d.provider);
    if (ok && d.d3d12_resource != nullptr) {
        vr->rawDepthTex = static_cast<ID3D12Resource*>(d.d3d12_resource);
    }
    // Velocity stays diagnostic until Phase 7 proves a fresh snapshot; do NOT feed a raw
    // bind-tracked velocity into Normal mode without freshness proof. With it invalid, AFW
    // continues in FromOtherEye mode exactly as today.
    UEVR_FrameResourceView v{};
    const bool vok = uevr_afw_bridge::get_latest_velocity(&v);
    spdlog::info("[FrameResources] AFW velocity={} provider={} reason={}",
                 vok ? "valid" : "invalid", (int)v.provider, v.reason ? v.reason : "");
}
// If !enabled / !available, or on any miss, the existing rawDepthTex / FromOtherEye path runs
// unchanged. legacy_fallback() lets a tester force the old path even when the plugin is present.
```

Why depth-only first: depth from SceneDepthZ is the proven, safe decoupling milestone. Velocity via
D3D12 bind tracking is a discovered SOURCE, not yet a safe-to-sample snapshot (see Phase 7), so the
bridge logs it but does not swap it into Normal-mode AFW until freshness is proven in-repo.

### Patch 6: Optional SDK Bridge For Native Pool Resource

Files:

```text
include/uevr/API.h
include/uevr/API.hpp
src/mods/pluginloader/FRenderTargetPoolHook.cpp
plugins/afw_frame_resources/providers/RenderTargetPoolProvider.*
```

Changes:

```text
Only if the plugin-side pooled-target adapter is too brittle.
Add a narrow get_native_resource(name) helper.
Keep it generic and disabled by absence.
Do not move tracker logic into UEVR core.
```

### Patch 7: Optional NGX Observer

Files:

```text
plugins/afw_frame_resources/providers/NgxDlssProvider.*
```

Changes:

```text
Use GetModuleHandle/GetProcAddress.
Install hooks only when NGX is already loaded and provider is enabled.
Report DLSS depth/MV as optional observations.
Plugin must still pass startup and DLSS-off diagnostics when this provider is disabled or absent.
```

### Patch 8: D3D12 Velocity Snapshot

Only after Patch 3 proves candidate selection.

Changes:

```text
Find a safe post-velocity copy point.
Add owned snapshot texture.
Add state transitions.
Keep retired resources alive for in-flight command lists.
Expose snapshot through tracker.
No DLSS dependency.
```

## Exported Plugin API Shape

The module should expose its own C ABI so it can be used by AFW or another plugin without becoming part of UEVR core.

```c
typedef struct UEVR_FrameResourceView {
    uint32_t kind;
    uint32_t provider;
    uint32_t validity;
    uint32_t eye;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t render_frame;
    void* d3d12_resource;
    float motion_scale_x;
    float motion_scale_y;
} UEVR_FrameResourceView;

typedef struct UEVR_FrameResourceOptions {
    uint32_t enable_render_target_pool;
    uint32_t enable_d3d12_bind_tracking;
    uint32_t enable_dlss_observer;
    uint32_t log_level;
} UEVR_FrameResourceOptions;

typedef struct UEVR_FrameResourcesApi {
    uint32_t version;
    bool (*is_available)();
    bool (*get_latest)(uint32_t kind, uint32_t eye, UEVR_FrameResourceView* out);
    void (*set_options)(const UEVR_FrameResourceOptions* options);
    const char* (*describe_state)();
} UEVR_FrameResourcesApi;

__declspec(dllexport)
bool uevr_frame_resources_get_api(uint32_t version, UEVR_FrameResourcesApi* out);
```

Callers discover it through:

```text
GetModuleHandleW(L"afw_frame_resources.dll")
GetProcAddress(module, "uevr_frame_resources_get_api")
```

Only expose raw resources and metadata at first. Descriptor heaps/SRVs can come later.

Do not require this API to be added to `include/uevr/API.h` unless the plugin-exported ABI proves insufficient.

## Risk List

### Accidental DLSS Dependency

The largest architectural failure would be a module that only works when NGX/DLSS is loaded.

Mitigation:

```text
Build and test the plugin with no NGX provider compiled or enabled.
Use GetModuleHandle/GetProcAddress only for the optional NGX observer.
Do not link NGX libraries.
Make "NGX not loaded" an info log, not an error.
```

### Velocity May Not Be Available Without New Hooks

Current public plugin surfaces can try RenderTargetPool names. Robust velocity discovery probably needs plugin-owned D3D12 bind tracking.

Mitigation:

```text
Log missing velocity explicitly.
Implement D3D12 descriptor/bind tracking inside the plugin after the skeleton works.
Do not move that logic into src/hooks/D3D12Hook.cpp unless a generic SDK bridge is later justified.
```

### RenderTargetPool Name Lookup May Fail

`SceneDepthZ` is already used and likely useful. Velocity names are less certain.

Mitigation:

```text
Try names.
Validate desc.
Do not rely on them as the only plan.
```

### Snapshot Timing Is Easy To Get Wrong

Copying a velocity target at bind time can capture stale content.

Mitigation:

```text
Do not add velocity snapshots until a safe copy point is proven in this repo.
Start log-only.
Use RenderDoc or clear frame logs to validate.
```

### Resource State Assumptions

Without knowing the exact RDG state transition point, copying can break or no-op.

Mitigation:

```text
Keep snapshots gated.
Use conservative resource barriers only after validation.
Add opt-out env.
```

### Too Much Original UEVR Churn

The goal is not to rewrite `VR`, `D3D12Component`, or `D3D12Hook`.

Mitigation:

```text
Build plugin first.
Keep all UEVR-core contact points optional.
Only add a tiny AFW bridge if the existing AFW path needs to consume plugin resources.
Do not remove old globals as part of the first module work.
```

## Acceptance Criteria

### Plugin Skeleton

```text
Builds as a plugin target.
Can be disabled.
Loads when DLSS is disabled.
Loads when nvngx.dll is absent.
No behavior change when disabled.
Exports uevr_frame_resources_get_api().
Reports missing resources explicitly before providers are active.
```

### DLSS-Off Depth

```text
DLSS disabled.
NGX absent or ignored.
Plugin reports SceneDepthZ through RenderTargetPool or reports exact extraction failure.
If AFW bridge is enabled, AFW logs depth provider.
No stale DLSS depth pointer is used.
```

### DLSS-Off Velocity Probe

```text
DLSS disabled.
NGX absent or ignored.
Plugin either reports a real/candidate velocity resource or explicitly says velocity is missing.
No fake valid velocity.
No stale DLSS MV pointer.
```

### AFW Decoupling

```text
AFW bridge runtime-loads the plugin API.
AFW builds its FrameBufferDesc from plugin views only when the plugin is present and enabled.
Legacy path can be toggled for comparison.
Plugin absent behavior matches current baseline.
DLSS-off behavior is diagnosable.
```

### Plugin API

```text
A plugin or optional AFW bridge can ask for latest depth/velocity without knowing whether it came from RenderTargetPool, D3D12 bind tracking, or optional DLSS observation.
API is versioned.
Missing resources are reported explicitly.
No API consumer needs to link NGX.
```

## Short Summary

Use only UEVRPureDark.

Build the module as:

```text
plugins/afw_frame_resources
type = "plugin"
exported C ABI
no DLSS dependency
```

Use existing plugin surfaces first:

```text
RenderTargetPoolHook
renderer.device / renderer.command_queue
on_post_render_vr_framework_dx12
register_inline_hook
console cvar access
```

Build missing providers inside the plugin:

```text
FrameResourceTracker core
RenderTargetPoolProvider
D3D12BindProvider
optional AFW bridge
optional NGX observer
optional velocity snapshot path
```

Do not assume `DIBRDepthTracker`.

Do not make DLSS required.

Do not put the tracker core in UEVR itself.

First useful test:

```text
DLSS off.
NGX absent or ignored.
AFW on.
Plugin on.
Depth from SceneDepthZ.
Velocity either real/candidate or explicitly missing.
```
