// AFWFrameResourcesBridge.cpp
//
// Self-contained, dead-until-wired bridge to the afw_frame_resources plugin. Uses only
// GetModuleHandle/GetProcAddress + getenv, so it never link-depends on the plugin and is a no-op
// when the plugin DLL is absent. See AFWFrameResourcesBridge.hpp for the wiring policy.
#define _CRT_SECURE_NO_WARNINGS

#include "AFWFrameResourcesBridge.hpp"

#include <Windows.h>

#include <cstdlib>

namespace uevr_afw_bridge {
namespace {

bool env_flag(const char* name, bool def) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return def;
    }
    return std::atoi(v) != 0;
}

UEVR_FrameResourcesApi g_api{};
bool g_resolved = false;
bool g_ok = false;

void resolve() {
    if (g_resolved) {
        return;
    }
    g_resolved = true;

    HMODULE mod = GetModuleHandleW(L"afw_frame_resources.dll");
    if (mod == nullptr) {
        return;
    }
    auto fn = reinterpret_cast<uevr_frame_resources_get_api_fn>(
        GetProcAddress(mod, "uevr_frame_resources_get_api"));
    if (fn == nullptr) {
        return;
    }
    g_ok = fn(UEVR_FRAME_RESOURCES_API_VERSION, &g_api) && g_api.get_latest != nullptr;
}

} // namespace

bool enabled() {
    static const bool e = env_flag("UEVR_AFW_FRAME_RESOURCES", false);
    return e;
}

bool legacy_fallback() {
    static const bool e = env_flag("UEVR_AFW_FRAME_RESOURCES_LEGACY_FALLBACK", true);
    return e;
}

bool use_velocity() {
    static const bool e = env_flag("UEVR_AFW_FRAME_RESOURCES_VELOCITY", false);
    return e;
}

bool available() {
    resolve();
    return g_ok;
}

bool get_latest_depth(UEVR_FrameResourceView* out) {
    resolve();
    if (!g_ok || out == nullptr) {
        return false;
    }
    return g_api.get_latest(UEVR_FRAME_RESOURCE_DEPTH, UEVR_FRAME_RESOURCE_EYE_UNKNOWN, out);
}

bool get_latest_velocity(UEVR_FrameResourceView* out) {
    resolve();
    if (!g_ok || out == nullptr) {
        return false;
    }
    return g_api.get_latest(UEVR_FRAME_RESOURCE_VELOCITY, UEVR_FRAME_RESOURCE_EYE_UNKNOWN, out);
}

const char* describe_state() {
    resolve();
    if (!g_ok) {
        return "afw_frame_resources.dll not present or not API-compatible";
    }
    return g_api.describe_state != nullptr ? g_api.describe_state() : "";
}

} // namespace uevr_afw_bridge
