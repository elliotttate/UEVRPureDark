/*
 * UEVRFrameResourcesAPI.h
 *
 * Stable C ABI exported by the afw_frame_resources plugin DLL.
 *
 * This is the ONLY header a consumer (AFW bridge, another plugin) needs to talk to the tracker.
 * It exposes raw D3D12 resources plus metadata. It deliberately does NOT expose any internal
 * UEVR/UESDK/PureDark type. Discover the entry point at runtime:
 *
 *     auto* mod = GetModuleHandleW(L"afw_frame_resources.dll");
 *     auto get_api = mod ? (uevr_frame_resources_get_api_fn)
 *         GetProcAddress(mod, "uevr_frame_resources_get_api") : nullptr;
 *     UEVR_FrameResourcesApi api{};
 *     if (get_api && get_api(UEVR_FRAME_RESOURCES_API_VERSION, &api)) { ... }
 *
 * Versioning: bump UEVR_FRAME_RESOURCES_API_VERSION on any incompatible struct/enum change.
 * get_api() returns false if the requested version does not match what the DLL exports.
 */
#ifndef UEVR_FRAME_RESOURCES_API_H
#define UEVR_FRAME_RESOURCES_API_H

#include <stdint.h>

#if !defined(__cplusplus)
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Only the plugin DLL that DEFINES uevr_frame_resources_get_api should export it. Consumers that
 * include this header (e.g. the AFW bridge compiled into UEVRBackend.dll) must NOT re-export the
 * symbol, so the dllexport attribute is gated behind UEVR_FRAME_RESOURCES_EXPORTS, which is defined
 * only by the plugin's Plugin.cpp before including this header. */
#if defined(_WIN32) && defined(UEVR_FRAME_RESOURCES_EXPORTS)
#define UEVR_FRAME_RESOURCES_API __declspec(dllexport)
#else
#define UEVR_FRAME_RESOURCES_API
#endif

/* v1 -> v2: added UEVR_FrameResourceView.expected_state so consumers can transition the resource. */
#define UEVR_FRAME_RESOURCES_API_VERSION 2u

/* ---- kind ----------------------------------------------------------------- */
#define UEVR_FRAME_RESOURCE_DEPTH              0u
#define UEVR_FRAME_RESOURCE_VELOCITY           1u
#define UEVR_FRAME_RESOURCE_CORRECTED_VELOCITY 2u
#define UEVR_FRAME_RESOURCE_COLOR              3u
#define UEVR_FRAME_RESOURCE_OUTPUT             4u

/* ---- provider ------------------------------------------------------------- */
#define UEVR_FRAME_RESOURCE_PROVIDER_NONE               0u
#define UEVR_FRAME_RESOURCE_PROVIDER_RENDER_TARGET_POOL 1u
#define UEVR_FRAME_RESOURCE_PROVIDER_D3D12_BIND         2u
#define UEVR_FRAME_RESOURCE_PROVIDER_INTERNAL_COPY      3u
#define UEVR_FRAME_RESOURCE_PROVIDER_DLSS_NGX           4u /* optional observer; never required */

/* ---- validity ------------------------------------------------------------- */
#define UEVR_FRAME_RESOURCE_VALIDITY_INVALID          0u
#define UEVR_FRAME_RESOURCE_VALIDITY_VALID            1u
#define UEVR_FRAME_RESOURCE_VALIDITY_MISSING_PROVIDER 2u
#define UEVR_FRAME_RESOURCE_VALIDITY_MISSING_RESOURCE 3u
#define UEVR_FRAME_RESOURCE_VALIDITY_WRONG_RENDERER   4u
#define UEVR_FRAME_RESOURCE_VALIDITY_WRONG_FORMAT     5u
#define UEVR_FRAME_RESOURCE_VALIDITY_WRONG_SIZE       6u
#define UEVR_FRAME_RESOURCE_VALIDITY_NOT_READY        7u
#define UEVR_FRAME_RESOURCE_VALIDITY_STALE            8u

/* ---- eye ------------------------------------------------------------------ */
#define UEVR_FRAME_RESOURCE_EYE_UNKNOWN 0u
#define UEVR_FRAME_RESOURCE_EYE_LEFT    1u
#define UEVR_FRAME_RESOURCE_EYE_RIGHT   2u
#define UEVR_FRAME_RESOURCE_EYE_BOTH    3u

typedef struct UEVR_FrameResourceView {
    uint32_t kind;          /* UEVR_FRAME_RESOURCE_*           */
    uint32_t provider;      /* UEVR_FRAME_RESOURCE_PROVIDER_*  */
    uint32_t validity;      /* UEVR_FRAME_RESOURCE_VALIDITY_*  */
    uint32_t eye;           /* UEVR_FRAME_RESOURCE_EYE_*       */
    uint32_t format;        /* DXGI_FORMAT                     */
    uint32_t width;
    uint32_t height;
    uint32_t render_frame;  /* tracker frame index the resource was last observed */
    uint32_t expected_state;/* D3D12_RESOURCE_STATES expected for a source transition */
    void*    d3d12_resource;/* ID3D12Resource*. Valid until tracker replacement/reset;
                               AddRef it if retained by the caller. */
    float    motion_scale_x;
    float    motion_scale_y;
    const char* reason;     /* static diagnostic string; may be null */
} UEVR_FrameResourceView;

typedef struct UEVR_FrameResourceOptions {
    uint32_t enable_render_target_pool;
    uint32_t enable_d3d12_bind_tracking;
    uint32_t enable_dlss_observer; /* optional; absence/disable must not break anything */
    uint32_t log_level;            /* 0 silent, 1 info, 2 debug, 3 trace */
} UEVR_FrameResourceOptions;

typedef struct UEVR_FrameResourcesApi {
    uint32_t version;
    bool (*is_available)(void);
    bool (*get_latest)(uint32_t kind, uint32_t eye, UEVR_FrameResourceView* out);
    void (*set_options)(const UEVR_FrameResourceOptions* options);
    const char* (*describe_state)(void);
} UEVR_FrameResourcesApi;

typedef bool (*uevr_frame_resources_get_api_fn)(uint32_t version, UEVR_FrameResourcesApi* out);

UEVR_FRAME_RESOURCES_API bool uevr_frame_resources_get_api(uint32_t version, UEVR_FrameResourcesApi* out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* UEVR_FRAME_RESOURCES_API_H */
