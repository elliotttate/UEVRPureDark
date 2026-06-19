// AFWFrameResourcesBridge.hpp
//
// Optional, runtime-discovered bridge that lets the existing AFW path (D3D12Component.cpp) consume
// the afw_frame_resources plugin's depth/velocity views WITHOUT a compile-time dependency on the
// plugin and WITHOUT changing behavior when the plugin is absent or the bridge is disabled.
//
// Gates:
//   UEVR_AFW_FRAME_RESOURCES=1                  -> let AFW query the plugin (default OFF)
//   UEVR_AFW_FRAME_RESOURCES_LEGACY_FALLBACK=1  -> keep the existing path as fallback (default ON)
//   UEVR_AFW_FRAME_RESOURCES_VELOCITY=1         -> feed bridge velocity to AFW (default OFF)
#pragma once

#include <cstdint>

// Single source of truth for the exported view/option structs (no plugin link dependency).
#include "../../../plugins/afw_frame_resources/include/UEVRFrameResourcesAPI.h"

namespace uevr_afw_bridge {

// Env gates (read once on first call).
bool enabled();
bool legacy_fallback();
bool use_velocity();

// True if afw_frame_resources.dll is loaded and exports a compatible API.
bool available();

// Latest depth/velocity. Returns true only when validity == UEVR_FRAME_RESOURCE_VALIDITY_VALID;
// `out` is always populated (even on false) so the caller can log provider/validity/reason.
bool get_latest_depth(UEVR_FrameResourceView* out);
bool get_latest_velocity(UEVR_FrameResourceView* out);

// Human-readable tracker state for logging; never null.
const char* describe_state();

} // namespace uevr_afw_bridge
