#pragma once

// Clean, game-agnostic C FFI surface for UEVR's embedded RenderDoc capture.
//
// This is a slimmed-down extraction of UEVRJ's RenderDiagnosticsCAPI RenderDoc
// section, decoupled from the SN2 forensics subsystem (no sidecar emission,
// no fog-chain capture comments, no shader-override cvar toggling). It owns:
//   - the proactive bootstrap that loads/initialises renderdoc.dll,
//   - the file-trigger capture watcher (%TEMP%/uevr_renderdoc_capture.req),
//   - exact-pair-preferring trigger/wildcard capture,
//   - status / template helpers.
//
// All functions returning const char* return UTF-8 JSON owned by UEVRBackend;
// the buffer is valid until the next call to the same function on the same
// thread, so callers must copy immediately.

#include <cstdint>

#if defined(_WIN32)
#define UEVR_RENDER_CAPI __declspec(dllexport)
#else
#define UEVR_RENDER_CAPI
#endif

extern "C" {

// Result of the proactive renderdoc.dll bootstrap. Mirrors the in-process
// capture service's BootstrapResult so external consumers can reason about
// capture safety without touching internal C++ types.
struct UevrRenderDocBootstrapResult {
    void* module;            // HMODULE for renderdoc.dll
    bool  was_preloaded;     // true if renderdoc.dll was in the process before UEVR ran
    bool  late_loaded;       // true if UEVR loaded renderdoc.dll itself
    bool  api_loaded;        // true if RENDERDOC_GetAPI succeeded
    bool  capture_safe;      // true when RenderDoc was present before D3D12/DXGI modules
    bool  d3d12_was_loaded;  // true if d3d12.dll was already loaded before bootstrap
    bool  dxgi_was_loaded;   // true if dxgi.dll was already loaded before bootstrap
    int   api_version_major;
    int   api_version_minor;
    int   api_version_patch;
};

// Proactively load renderdoc.dll if not already loaded (late-load is opt-in via
// UEVR_LOAD_RENDERDOC_DLL=1 / an explicit DLL path), then initialise the in-app
// API and apply default capture options/template. Skips entirely if
// UEVR_DISABLE_RENDERDOC_BOOTSTRAP=1. Logs status via spdlog. Safe to call
// multiple times.
UEVR_RENDER_CAPI UevrRenderDocBootstrapResult uevr_renderdoc_bootstrap();

// Returns true iff the RenderDoc in-app API is currently loaded.
UEVR_RENDER_CAPI bool uevr_renderdoc_is_api_loaded();

// Trigger a single capture, preferring the exact {device,window} pair tracked
// by the D3D12 Present hook and falling back to a wildcard capture. Returns
// true if EndFrameCapture reported a capture was written. No JSON wrapping -
// for use from Framework / hotkey paths.
UEVR_RENDER_CAPI bool uevr_renderdoc_capture_wildcard();

// Start the sentinel-file watcher for triggering captures from outside the
// process. Spawns a detached thread that polls
// %TEMP%/uevr_renderdoc_capture.req every 250ms. File format:
//   line 1: capture file template path (optional - empty = use default)
//   line 2: "frames=N" (optional, default 1)
// On detection: SetCaptureFilePathTemplate(template) then capture N frames.
// Stops automatically on process exit. Idempotent.
UEVR_RENDER_CAPI void uevr_renderdoc_start_capture_watcher();

// Returns JSON describing RenderDoc API status (loaded, version, num_captures,
// is_frame_capturing, loaded_path, capture_safe, capture_path_template, active
// device/window pointers, captures list). Returns { loaded: false } when the
// API is not in-process.
UEVR_RENDER_CAPI const char* uevr_render_diag_renderdoc_status_json();

// Queue/perform a RenderDoc trigger-capture for the next `num_frames` frames
// (1 if <=1). num_frames==1 takes a synchronous exact-pair-preferring capture;
// >1 uses RenderDoc's TriggerMultiFrameCapture. Returns JSON {ok, ...}.
UEVR_RENDER_CAPI const char* uevr_render_diag_renderdoc_trigger_capture(int num_frames);

// Launch the RenderDoc replay UI connected to this process. Returns JSON
// {ok, pid, error}.
UEVR_RENDER_CAPI const char* uevr_render_diag_renderdoc_launch_ui();

// Override the RenderDoc capture file template (.../prefix). NULL/empty leaves
// it unchanged. Returns JSON {ok, template, error}.
UEVR_RENDER_CAPI const char* uevr_render_diag_renderdoc_set_capture_template(const char* path_template);

} // extern "C"
