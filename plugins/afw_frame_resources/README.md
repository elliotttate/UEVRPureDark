# afw_frame_resources

A decoupled, provider-neutral **frame resource tracker** for UEVRPureDark, shipped as a loadable
UEVR plugin DLL. It discovers depth/velocity render resources **without requiring DLSS/NGX** and
exposes them through a small, versioned C ABI so AFW (or any other plugin) can ask for buffers
instead of reaching into `VR` fields or depending on DLSS.

See `../../AFW_FRAME_RESOURCE_TRACKER_PLAN.md` for the full design, rationale, phases, and the
debug/test plan. This README is the quick operational reference.

## What it does today

- **Phase 1** — plugin skeleton + core state machine + exported C ABI (`include/UEVRFrameResourcesAPI.h`).
- **Phase 2** — `RenderTargetPoolProvider`: resolves `SceneDepthZ` and velocity-name candidates
  (`SceneVelocity` / `GBufferVelocity` / `Velocity`) to `ID3D12Resource*`.
- **Phase 3** — `D3D12BindProvider`: plugin-owned inline hooks on `CreateRenderTargetView` /
  `CreateDepthStencilView` / `OMSetRenderTargets`; identifies depth- and velocity-shaped binds and
  reports them to the tracker (the provider takes no resource copy — the consumer does).
- **Phase 6 (optional)** — `NgxDlssProvider`: detection-only DLSS observer, never required.
- Debug logging (level-gated) + an offline self-test.

It never links any NVIDIA/NGX import lib and runs with `nvngx.dll`/`_nvngx.dll` absent.

## Build

The plugin is a normal target in the repo's cmake:

```sh
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target afw_frame_resources afw_frame_resources_selftest
```

Outputs:
- `build/Release/afw_frame_resources.dll` — the plugin.
- `build/Release/afw_frame_resources_selftest.exe` — offline state-machine self-test (exit 0 = PASS).

## Deploy

Copy `afw_frame_resources.dll` into the game's UEVR plugins folder
(`<persistent_dir>/plugins` or `<persistent_dir>/../UEVR/plugins`; see `src/mods/PluginLoader.cpp`).

## Environment switches

| Variable | Default | Meaning |
|---|---|---|
| `UEVR_FRAME_RESOURCES` | 1 | Master enable (0 = load but stay inert) |
| `UEVR_FRAME_RESOURCES_LOG` | 1 | 0 silent, 1 info, 2 debug, 3 trace |
| `UEVR_FRAME_RESOURCES_SELFTEST` | 0 | Run the offline self-test at init and log PASS/FAIL |
| `UEVR_FRAME_RESOURCES_ENABLE_RTPOOL` | 1 | Enable RenderTargetPool provider |
| `UEVR_FRAME_RESOURCES_ENABLE_D3D12BIND` | 1 | Enable D3D12 bind provider (kill switch if hooks misbehave) |
| `UEVR_FRAME_RESOURCES_ENABLE_DLSS_OBSERVER` | 0 | Enable optional NGX observer (never required) |
| `UEVR_FRAME_RESOURCES_FORCE_VELOCITY` | 0 | Best-effort: request `r.VelocityOutputPass=1` via console for diagnostics. That cvar is read-only, so to force velocity reliably set it at game startup (Engine.ini / command line) instead |
| `UEVR_FRAME_RESOURCES_FORCE_RDG_POOL` | 0 | Request `r.RDG.TransientAllocator=0` via console so UE5 `SceneDepthZ` / `SceneVelocity` route through `FRenderTargetPool::FindFreeElement` by name |
| `UEVR_FRAME_RESOURCES_DUMP_EVERY` | 0 | If N>0, log `describe_state()` every N frames |
| `UEVR_FRAME_RESOURCES_MAX_STALE_FRAMES` | 2 | Frames before an observed resource reports stale; use 6-8 only as a velocity flicker diagnostic |
| `UEVR_FRAME_RESOURCES_ALLOW_LOW_RES_VELOCITY` | 0 | Allow a lower-resolution auxiliary velocity target to win after a larger scene-sized velocity has been seen; keep 0 for AFW |
| `UEVR_FRAME_RESOURCES_HOLD_SCENE_VELOCITY` | 0 | Diagnostic AFW mode: keep the selected high-water RG16 scene velocity fresh even if the bind hook only observes it once |
| `UEVR_FRAME_RESOURCES_ENABLE_RESOURCE_CREATE` | 0 | Opt-in diagnostic hook for D3D12 resource creation; leave 0 for normal AFW tests |

AFW bridge gates (consumed by `src/mods/vr/AFWFrameResourcesBridge.*` and the AFW path in `src/mods/vr/D3D12Component.cpp`):

| Variable | Default | Meaning |
|---|---|---|
| `UEVR_AFW_FRAME_RESOURCES` | 0 | Let AFW query the plugin |
| `UEVR_AFW_FRAME_RESOURCES_LEGACY_FALLBACK` | 1 | Keep the existing AFW path as fallback |
| `UEVR_AFW_FRAME_RESOURCES_VELOCITY` | 0 | Feed bridge velocity into AFW. Leave off unless isolating motion-vector crashes |

## Consuming the API from another module

```c
#include "UEVRFrameResourcesAPI.h"

auto* mod = GetModuleHandleW(L"afw_frame_resources.dll");
auto get_api = mod ? (uevr_frame_resources_get_api_fn)
    GetProcAddress(mod, "uevr_frame_resources_get_api") : nullptr;

UEVR_FrameResourcesApi api{};
if (get_api && get_api(UEVR_FRAME_RESOURCES_API_VERSION, &api)) {
    UEVR_FrameResourceView depth{};
    api.get_latest(UEVR_FRAME_RESOURCE_DEPTH, UEVR_FRAME_RESOURCE_EYE_UNKNOWN, &depth);
    // depth.validity == UEVR_FRAME_RESOURCE_VALIDITY_VALID -> depth.d3d12_resource usable
}
```

## Tests

- **Offline (no game):** run `afw_frame_resources_selftest.exe` → `core selftest: 7/7 PASS`.
- **In-game (DLSS off):** deploy the DLL, launch with `UEVR_FRAME_RESOURCES_LOG=2
  UEVR_FRAME_RESOURCES_SELFTEST=1 UEVR_FRAME_RESOURCES_ENABLE_DLSS_OBSERVER=0`, then grep the UEVR
  log for the per-phase lines listed in the plan's "Debug And Verification" section.
