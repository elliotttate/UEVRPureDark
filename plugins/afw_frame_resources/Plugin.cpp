// Plugin.cpp
//
// afw_frame_resources: a decoupled, provider-neutral frame resource tracker exposed as a UEVR
// plugin DLL. Discovers depth/velocity resources without requiring DLSS/NGX and exposes them
// through a stable C ABI (include/UEVRFrameResourcesAPI.h).
//
// This is the ONLY translation unit that includes uevr/Plugin.hpp (which defines the exported
// plugin entry points / DllMain). All other TUs include only uevr/API.hpp.
#define _CRT_SECURE_NO_WARNINGS

#include <Windows.h>
#include <d3d12.h>

#include <cstdlib>
#include <memory>
#include <string>

#include "uevr/Plugin.hpp"

#include "FrameResourceLog.hpp"
#include "FrameResourceTracker.hpp"
// This TU defines + exports uevr_frame_resources_get_api, so it opts into the dllexport attribute.
#define UEVR_FRAME_RESOURCES_EXPORTS 1
#include "include/UEVRFrameResourcesAPI.h"
#include "providers/D3D12BindProvider.hpp"
#include "providers/NgxDlssProvider.hpp"
#include "providers/RenderTargetPoolProvider.hpp"

using namespace uevr;

namespace {

// ---- log sink: route the core logger to the UEVR plugin log -------------------------------
void plugin_log_sink(afw_fr::LogSeverity severity, const char* message) {
    auto& api = uevr::API::get();
    if (api == nullptr) {
        return;
    }
    switch (severity) {
    case afw_fr::LogSeverity::Error: api->log_error("%s", message); break;
    case afw_fr::LogSeverity::Warn: api->log_warn("%s", message); break;
    case afw_fr::LogSeverity::Info:
    default: api->log_info("%s", message); break;
    }
}

// ---- env config helpers --------------------------------------------------------------------
int env_int(const char* name, int def) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return def;
    return std::atoi(v);
}
bool env_bool(const char* name, bool def) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return def;
    return std::atoi(v) != 0;
}

struct Config {
    bool master{true};
    int log_level{1};
    bool selftest{false};
    bool enable_rtpool{true};
    bool enable_d3d12bind{true};
    bool enable_dlss_observer{false};
    bool force_velocity{false};
    bool force_rdg_pool{false};
    int dump_every{0};
    uint32_t max_stale_frames{2};
};

Config read_config() {
    Config c;
    c.master = env_bool("UEVR_FRAME_RESOURCES", true);
    c.log_level = env_int("UEVR_FRAME_RESOURCES_LOG", 1);
    c.selftest = env_bool("UEVR_FRAME_RESOURCES_SELFTEST", false);
    c.enable_rtpool = env_bool("UEVR_FRAME_RESOURCES_ENABLE_RTPOOL", true);
    c.enable_d3d12bind = env_bool("UEVR_FRAME_RESOURCES_ENABLE_D3D12BIND", true);
    c.enable_dlss_observer = env_bool("UEVR_FRAME_RESOURCES_ENABLE_DLSS_OBSERVER", false);
    c.force_velocity = env_bool("UEVR_FRAME_RESOURCES_FORCE_VELOCITY", false);
    c.force_rdg_pool = env_bool("UEVR_FRAME_RESOURCES_FORCE_RDG_POOL", false);
    c.dump_every = env_int("UEVR_FRAME_RESOURCES_DUMP_EVERY", 0);
    const int max_stale_frames = env_int("UEVR_FRAME_RESOURCES_MAX_STALE_FRAMES", 2);
    c.max_stale_frames = static_cast<uint32_t>(max_stale_frames < 0 ? 0 : max_stale_frames);
    return c;
}

} // namespace

// ---- exported C ABI -------------------------------------------------------------------------
namespace {

bool api_is_available() {
    return afw_fr::FrameResourceTracker::get().is_available();
}

bool api_get_latest(uint32_t kind, uint32_t eye, UEVR_FrameResourceView* out) {
    if (out == nullptr) {
        return false;
    }
    const afw_fr::FrameResourceView v = afw_fr::FrameResourceTracker::get().get_latest(
        static_cast<afw_fr::FrameResourceKind>(kind), static_cast<afw_fr::FrameResourceEye>(eye));
    out->kind = static_cast<uint32_t>(v.kind);
    out->provider = static_cast<uint32_t>(v.provider);
    out->validity = static_cast<uint32_t>(v.validity);
    out->eye = static_cast<uint32_t>(v.eye);
    out->format = static_cast<uint32_t>(v.format);
    out->width = v.width;
    out->height = v.height;
    out->render_frame = v.render_frame;
    out->expected_state = static_cast<uint32_t>(v.expected_state);
    out->d3d12_resource = v.resource;
    out->motion_scale_x = v.motion_scale_x;
    out->motion_scale_y = v.motion_scale_y;
    out->reason = v.reason;
    return v.validity == afw_fr::FrameResourceValidity::Valid;
}

void api_set_options(const UEVR_FrameResourceOptions* options) {
    if (options == nullptr) {
        return;
    }
    auto& tracker = afw_fr::FrameResourceTracker::get();
    afw_fr::TrackerOptions opts = tracker.options();
    opts.enable_render_target_pool = options->enable_render_target_pool != 0;
    opts.enable_d3d12_bind = options->enable_d3d12_bind_tracking != 0;
    opts.enable_dlss_observer = options->enable_dlss_observer != 0;
    opts.log_level = static_cast<afw_fr::LogLevel>(options->log_level);
    tracker.set_options(opts);
    afw_fr::set_log_level(opts.log_level);
    // NOTE: this changes resolution/logging only; provider hook (un)install is bound to
    // on_initialize / on_device_reset, not to runtime option flips.
}

const char* api_describe_state() {
    return afw_fr::FrameResourceTracker::get().describe_state_cstr();
}

} // namespace

extern "C" __declspec(dllexport) bool uevr_frame_resources_get_api(uint32_t version, UEVR_FrameResourcesApi* out) {
    if (out == nullptr) {
        return false;
    }
    if (version != UEVR_FRAME_RESOURCES_API_VERSION) {
        return false;
    }
    out->version = UEVR_FRAME_RESOURCES_API_VERSION;
    out->is_available = &api_is_available;
    out->get_latest = &api_get_latest;
    out->set_options = &api_set_options;
    out->describe_state = &api_describe_state;
    return true;
}

// ---- plugin ---------------------------------------------------------------------------------
class FrameResourcesPlugin : public uevr::Plugin {
public:
    FrameResourcesPlugin() = default;

    void on_initialize() override {
        afw_fr::set_log_sink(&plugin_log_sink);

        m_config = read_config();
        afw_fr::set_log_level(static_cast<afw_fr::LogLevel>(m_config.log_level));

        if (!m_config.master) {
            afw_fr::log_info("disabled via UEVR_FRAME_RESOURCES=0; staying inert");
            return;
        }

        auto& api = uevr::API::get();
        const auto* renderer = api->param()->renderer;
        const bool ngx_present = afw_fr::NgxDlssProvider::ngx_present();

        auto& tracker = afw_fr::FrameResourceTracker::get();

        if (renderer != nullptr && renderer->renderer_type == UEVR_RENDERER_D3D12 && renderer->device != nullptr) {
            auto* device = static_cast<ID3D12Device*>(renderer->device);
            auto* queue = static_cast<ID3D12CommandQueue*>(renderer->command_queue);
            tracker.initialize(device, queue);

            afw_fr::TrackerOptions opts = tracker.options();
            opts.enable_render_target_pool = m_config.enable_rtpool;
            opts.enable_d3d12_bind = m_config.enable_d3d12bind;
            opts.enable_dlss_observer = m_config.enable_dlss_observer;
            opts.log_level = static_cast<afw_fr::LogLevel>(m_config.log_level);
            opts.max_stale_frames = m_config.max_stale_frames;
            tracker.set_options(opts);

            if (m_config.enable_rtpool) {
                m_rtpool.activate();
            }
            if (m_config.enable_d3d12bind) {
                m_d3d12bind.install(device);
            }
            if (m_config.enable_dlss_observer) {
                m_ngx.try_enable();
            }
            if (m_config.force_velocity) {
                m_force_velocity_pending = true;
                afw_fr::log_info("force_velocity: deferred until engine tick");
            }
            if (m_config.force_rdg_pool) {
                m_force_rdg_pool_pending = true;
                afw_fr::log_info("force_rdg_pool: deferred until engine tick");
            }
        } else {
            afw_fr::log_warn("init skipped: renderer is not D3D12 (type=%d device=%p)",
                             renderer ? renderer->renderer_type : -1,
                             renderer ? renderer->device : nullptr);
        }

        if (m_config.selftest) {
            run_selftest();
        }

        afw_fr::log_info("init renderer=%s dlss_required=false ngx_present=%d providers=[rtpool=%d d3d12bind=%d dlss=%d]",
                         (renderer && renderer->renderer_type == UEVR_RENDERER_D3D12) ? "D3D12" : "non-D3D12",
                         ngx_present ? 1 : 0, m_config.enable_rtpool ? 1 : 0,
                         m_config.enable_d3d12bind ? 1 : 0, m_config.enable_dlss_observer ? 1 : 0);
        afw_fr::log_info("tracker options: max_stale_frames=%u", m_config.max_stale_frames);
        afw_fr::log_info("api ready version=%u", UEVR_FRAME_RESOURCES_API_VERSION);
    }

    void on_post_engine_tick(API::UGameEngine* engine, float delta) override {
        (void)engine;
        (void)delta;
        if (!m_config.master) {
            return;
        }

        if (m_force_rdg_pool_pending) {
            m_force_rdg_pool_pending = false;
            set_force_rdg_pool_cvar();
        }
        if (m_force_velocity_pending) {
            m_force_velocity_pending = false;
            set_force_velocity_cvar();
        }
    }

    void on_present() override {
        if (!m_config.master) {
            return;
        }
        auto& tracker = afw_fr::FrameResourceTracker::get();
        if (!tracker.is_available()) {
            return;
        }

        ++m_frame;
        tracker.begin_frame(m_frame);

        if (m_config.enable_rtpool) {
            m_rtpool.update(tracker);
        }
        if (m_config.enable_d3d12bind) {
            m_d3d12bind.flush(tracker);
        }

        if (m_config.dump_every > 0 && (m_frame % static_cast<uint32_t>(m_config.dump_every)) == 0) {
            afw_fr::log_info("%s", tracker.describe_state().c_str());
        }
    }

    void on_device_reset() override {
        m_d3d12bind.uninstall();
        afw_fr::FrameResourceTracker::get().reset();
        // Re-arm the once-only discovery logs so the post-reset device re-reports what it finds.
        afw_fr::reset_log_once();
        m_frame = 0;
        afw_fr::log_info("device reset: providers torn down, tracker reset");
    }

private:
    void set_force_velocity_cvar() {
        auto& api = uevr::API::get();
        auto* console = api->get_console_manager();
        if (console == nullptr) {
            afw_fr::log_info("force_velocity: console manager unavailable");
            return;
        }
        // r.VelocityOutputPass is the real UE5 velocity-generation cvar (0=depth pass, 1=base pass,
        // 2=after base pass). It is ECVF_ReadOnly and changing it triggers a shader recompile, so a
        // runtime set is best-effort and may be ignored by the engine — to force velocity reliably,
        // set it at game startup (Engine.ini / command line). (The old r.Velocity.ForceOutput name
        // does not exist in UE5.)
        if (auto* cvar = console->find_variable(L"r.VelocityOutputPass")) {
            cvar->set(1);
            afw_fr::log_info("force_velocity: requested r.VelocityOutputPass=1 (best-effort; this cvar is "
                             "read-only and may only take effect when set at game startup)");
        } else {
            afw_fr::log_info("force_velocity: r.VelocityOutputPass not found in this title");
        }
    }

    void set_force_rdg_pool_cvar() {
        auto& api = uevr::API::get();
        auto* console = api->get_console_manager();
        if (console == nullptr) {
            afw_fr::log_info("force_rdg_pool: console manager unavailable");
            return;
        }

        // UE5's RDG transient allocator aliases scene textures and bypasses FRenderTargetPool.
        // Turning it off routes SceneDepthZ / SceneVelocity through FindFreeElement by name,
        // which makes the RenderTargetPool provider deterministic and injection-timing independent.
        if (auto* cvar = console->find_variable(L"r.RDG.TransientAllocator")) {
            cvar->set(0);
            afw_fr::log_info("force_rdg_pool: requested r.RDG.TransientAllocator=0, current=%d",
                             cvar->get_int());
        } else {
            afw_fr::log_info("force_rdg_pool: r.RDG.TransientAllocator not found in this title");
        }
    }

    void run_selftest() {
        std::string report;
        afw_fr::FrameResourceTracker::run_self_test(&report); // PASS/FAIL is encoded in `report`
        afw_fr::log_info("%s", report.c_str());

        // ABI check (T1.7): wrong version must fail, current version must populate.
        bool abi_ok = true;
        UEVR_FrameResourcesApi probe{};
        if (uevr_frame_resources_get_api(0xFFFFFFFFu, &probe)) abi_ok = false;
        if (!uevr_frame_resources_get_api(UEVR_FRAME_RESOURCES_API_VERSION, &probe)) abi_ok = false;
        if (abi_ok && (probe.get_latest == nullptr || probe.describe_state == nullptr ||
                       probe.is_available == nullptr || probe.set_options == nullptr)) {
            abi_ok = false;
        }
        if (abi_ok) {
            UEVR_FrameResourceView v{};
            probe.get_latest(UEVR_FRAME_RESOURCE_DEPTH, UEVR_FRAME_RESOURCE_EYE_UNKNOWN, &v);
        }
        afw_fr::log_info("abi selftest: %s", abi_ok ? "PASS" : "FAIL");
    }

    Config m_config{};
    uint32_t m_frame{0};
    bool m_force_velocity_pending{false};
    bool m_force_rdg_pool_pending{false};
    afw_fr::RenderTargetPoolProvider m_rtpool;
    afw_fr::D3D12BindProvider m_d3d12bind;
    afw_fr::NgxDlssProvider m_ngx;
};

std::unique_ptr<FrameResourcesPlugin> g_plugin{new FrameResourcesPlugin()};
