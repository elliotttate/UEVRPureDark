// RenderTargetPoolProvider.cpp
#include "RenderTargetPoolProvider.hpp"

#include "../FrameResourceLog.hpp"
#include "PooledRenderTargetLayout.hpp"

#include "uevr/API.hpp"

namespace afw_fr {

void RenderTargetPoolProvider::activate() {
    // Tells the engine-level FRenderTargetPool hook to start capturing pooled targets.
    uevr::API::RenderTargetPoolHook::activate();
    m_active = true;
    log_info("RenderTargetPool provider activated");
}

bool RenderTargetPoolProvider::probe(FrameResourceTracker& tracker, const wchar_t* wname, const char* name,
                                     FrameResourceKind kind) {
    void* handle = reinterpret_cast<void*>(uevr::API::RenderTargetPoolHook::get_render_target(wname));
    if (handle == nullptr) {
        log_trace("rtpool walk failed at handle for name=%s", name);
        tracker.bump("rtpool_handle_miss");
        return false;
    }

    const char* failed_hop = nullptr;
    MinFRHITexture2D* rhi = pooled_rt_to_rhi_texture(handle, &failed_hop);
    if (rhi == nullptr) {
        log_debug("rtpool walk failed at %s for name=%s", failed_hop ? failed_hop : "?", name);
        tracker.bump("rtpool_walk_fail");
        return false;
    }

    // Convert FRHITexture2D* -> ID3D12Resource* via the plugin-facing SDK function table.
    // This is implemented in the main module (FRHITexture2DFunctions.cpp), so no extra linkage.
    auto* api_tex = reinterpret_cast<uevr::API::FRHITexture2D*>(rhi);
    auto* native = reinterpret_cast<ID3D12Resource*>(api_tex->get_native_resource());
    if (native == nullptr) {
        log_debug("rtpool walk failed at native for name=%s", name);
        tracker.bump("rtpool_native_null");
        return false;
    }

    const D3D12_RESOURCE_DESC desc = native->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width == 0 || desc.Height == 0) {
        log_debug("rtpool %s rejected: not a 2D texture (dim=%d %llux%u)", name, (int)desc.Dimension,
                  (unsigned long long)desc.Width, desc.Height);
        tracker.bump("rtpool_bad_desc");
        return false;
    }

    ObservedFrameResource o;
    o.kind = kind;
    o.provider = FrameResourceProvider::RenderTargetPool;
    // TODO(per-eye/motion): emitted as Both with default unit motion scale. The pooled targets are
    // the shared engine resources (not per-eye), and motion_scale is not yet derived. Per-eye
    // routing + motion_scale population are future work (see plan Phase 4+); the tracker's eye
    // machinery already supports it once a provider sets eye/mv_scale.
    o.eye = FrameResourceEye::Both;
    o.resource = native;
    o.format = desc.Format;
    o.width = static_cast<uint32_t>(desc.Width);
    o.height = desc.Height;
    o.expected_state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    o.render_frame = tracker.current_frame();
    o.debug_name = name; // static literal, safe to keep
    tracker.observe_resource(o);
    tracker.bump("rtpool_hit");
    return true;
}

void RenderTargetPoolProvider::update(FrameResourceTracker& tracker) {
    if (!m_active) {
        return;
    }

    // Depth: SceneDepthZ is the proven pooled name (validated against UE 5.6 SceneTextures.cpp).
    if (!probe(tracker, L"SceneDepthZ", "SceneDepthZ", FrameResourceKind::Depth)) {
        tracker.observe_missing(FrameResourceKind::Depth, FrameResourceProvider::RenderTargetPool,
                                "SceneDepthZ not resolved");
    }

    // Velocity: try candidate names in order; first hit wins.
    const bool v1 = probe(tracker, L"SceneVelocity", "SceneVelocity", FrameResourceKind::Velocity);
    const bool v2 = !v1 && probe(tracker, L"GBufferVelocity", "GBufferVelocity", FrameResourceKind::Velocity);
    const bool v3 = !v1 && !v2 && probe(tracker, L"Velocity", "Velocity", FrameResourceKind::Velocity);

    if (log_once_key("rtpool_velocity_probe_summary")) {
        log_info("velocity probe(RenderTargetPool): SceneVelocity=%s GBufferVelocity=%s Velocity=%s",
                 v1 ? "hit" : "miss", v2 ? "hit" : "miss", v3 ? "hit" : "miss");
    }
    if (!v1 && !v2 && !v3) {
        tracker.observe_missing(FrameResourceKind::Velocity, FrameResourceProvider::RenderTargetPool,
                                "no pool velocity name resolved (SceneVelocity/GBufferVelocity/Velocity)");
    }
}

} // namespace afw_fr
