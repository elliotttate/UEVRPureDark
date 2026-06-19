#include "render/RenderDocCaptureControl.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "render/RenderDocCaptureService.hpp"

namespace rdc = uevr::renderdoc_capture;
using json = nlohmann::json;

namespace {

thread_local std::string g_capi_return_buffer{};

const char* publish(json value) {
    g_capi_return_buffer = value.dump();
    return g_capi_return_buffer.c_str();
}

std::string format_pointer(uintptr_t pointer) {
    std::ostringstream ss{};
    ss << "0x" << std::hex << std::uppercase << pointer;
    return ss.str();
}

struct RenderDocCaptureAttempt {
    bool ended{};
    bool had_exact_pair{};
    bool wildcard_fallback{};
    const char* mode{"wildcard_no_active_pair"};
    rdc::CapturePair pair{};
};

// Prefer the exact {device,window} pair published by the D3D12 Present hook,
// then fall back to a wildcard capture (device=null, window=null) which lets
// RenderDoc capture whatever swapchain is active.
RenderDocCaptureAttempt renderdoc_capture_prefer_active_pair(std::chrono::milliseconds duration) {
    RenderDocCaptureAttempt attempt{};
    attempt.pair = rdc::active_window();
    attempt.had_exact_pair = attempt.pair.device != nullptr && attempt.pair.window != nullptr;

    if (attempt.had_exact_pair) {
        attempt.mode = "exact_pair";
        attempt.ended = rdc::capture_blocking(attempt.pair, duration);
        if (attempt.ended) {
            return attempt;
        }

        attempt.wildcard_fallback = true;
        attempt.mode = "exact_pair_failed_wildcard";
    }

    attempt.ended = rdc::capture_blocking({}, duration);
    if (!attempt.had_exact_pair) {
        attempt.mode = "wildcard_no_active_pair";
    }
    return attempt;
}

// File-trigger capture watcher.

std::thread g_renderdoc_watcher_thread{};
std::atomic<bool> g_renderdoc_watcher_started{false};

void renderdoc_capture_watcher_loop() {
    namespace fs = std::filesystem;
    wchar_t tempw[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tempw) == 0) {
        spdlog::warn("[RenderDoc] watcher: GetTempPathW failed");
        return;
    }
    fs::path sentinel = fs::path{tempw} / L"uevr_renderdoc_capture.req";
    spdlog::info("[RenderDoc] watcher: polling {} every 250ms", sentinel.string());

    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        std::error_code ec;
        if (!fs::exists(sentinel, ec)) {
            continue;
        }

        // Read + consume the sentinel.
        std::string content;
        try {
            std::ifstream f(sentinel);
            std::stringstream ss;
            ss << f.rdbuf();
            content = ss.str();
        } catch (...) {
            // ignore
        }
        fs::remove(sentinel, ec);

        // Parse: line 1 = capture template, later "frames=N".
        std::string capture_template;
        int frames = 1;
        {
            std::istringstream iss(content);
            std::string line;
            int line_no = 0;
            while (std::getline(iss, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
                    line.pop_back();
                }
                ++line_no;
                if (line_no == 1) {
                    capture_template = std::move(line);
                } else if (line.rfind("frames=", 0) == 0) {
                    try {
                        // Parenthesised to dodge the Windows.h max() macro.
                        frames = (std::max)(1, std::stoi(line.substr(7)));
                    } catch (...) {
                    }
                }
            }
        }

        auto* api = rdc::api();
        if (api == nullptr) {
            spdlog::warn("[RenderDoc] watcher: trigger received but API not loaded");
            continue;
        }
        if (!capture_template.empty()) {
            rdc::set_capture_template(capture_template);
            spdlog::info("[RenderDoc] watcher: capture template -> {}", capture_template);
        }

        if (frames > 1) {
            api->TriggerMultiFrameCapture(static_cast<uint32_t>(frames));
            spdlog::info("[RenderDoc] watcher: triggered {} frames", frames);
        } else {
            const auto attempt = renderdoc_capture_prefer_active_pair(std::chrono::milliseconds(250));
            spdlog::info("[RenderDoc] watcher: capture mode={} ended={} newest='{}'",
                         attempt.mode, attempt.ended, rdc::newest_capture_path());
        }
    }
}

} // namespace

extern "C" UEVR_RENDER_CAPI UevrRenderDocBootstrapResult uevr_renderdoc_bootstrap() {
    UevrRenderDocBootstrapResult r{};

    const auto result = rdc::bootstrap(rdc::env_truthy_w(L"UEVR_LOAD_RENDERDOC_DLL"));
    r.module = result.module;
    r.was_preloaded = result.was_preloaded;
    r.late_loaded = result.late_loaded;
    r.api_loaded = result.api_loaded;
    r.capture_safe = result.capture_safe;
    r.d3d12_was_loaded = result.d3d12_was_loaded;
    r.dxgi_was_loaded = result.dxgi_was_loaded;
    r.api_version_major = result.api_version_major;
    r.api_version_minor = result.api_version_minor;
    r.api_version_patch = result.api_version_patch;

    if (!r.api_loaded) {
        return r;
    }

    spdlog::info("[RenderDoc] API ready: v{}.{}.{}  (preloaded={})",
                 r.api_version_major, r.api_version_minor, r.api_version_patch,
                 r.was_preloaded);
    return r;
}

extern "C" UEVR_RENDER_CAPI bool uevr_renderdoc_is_api_loaded() {
    return rdc::is_api_loaded();
}

extern "C" UEVR_RENDER_CAPI bool uevr_renderdoc_capture_wildcard() {
    return renderdoc_capture_prefer_active_pair(std::chrono::milliseconds(250)).ended;
}

extern "C" UEVR_RENDER_CAPI void uevr_renderdoc_start_capture_watcher() {
    bool expected = false;
    if (!g_renderdoc_watcher_started.compare_exchange_strong(expected, true)) {
        return; // already started
    }
    g_renderdoc_watcher_thread = std::thread{renderdoc_capture_watcher_loop};
    g_renderdoc_watcher_thread.detach();
}

extern "C" UEVR_RENDER_CAPI const char* uevr_render_diag_renderdoc_status_json() {
    try {
        auto* api = rdc::api();
        if (api == nullptr) {
            return publish(json{
                {"loaded", false},
                {"hint", "Launch the game via the RenderDoc launcher (or set "
                         "UEVR_RENDERDOC_BOOTSTRAP=1 + UEVR_LOAD_RENDERDOC_DLL=1) so renderdoc.dll "
                         "is in-process. No-op otherwise."},
            });
        }
        int major{}, minor{}, patch{};
        api->GetAPIVersion(&major, &minor, &patch);
        const uint32_t num_captures = api->GetNumCaptures();
        const auto bootstrap = rdc::status();
        const auto active_pair = rdc::active_window();

        json captures = json::array();
        for (const auto& capture : rdc::captures()) {
            captures.push_back({
                {"index", capture.index},
                {"path", capture.path},
                {"timestamp", capture.timestamp},
            });
        }

        return publish(json{
            {"loaded", true},
            {"version", std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch)},
            {"num_captures", num_captures},
            {"is_target_control_connected", api->IsTargetControlConnected() != 0},
            {"is_frame_capturing", api->IsFrameCapturing() != 0},
            {"loaded_path", bootstrap.loaded_path},
            {"was_preloaded", bootstrap.was_preloaded},
            {"loaded_by_uevr", bootstrap.late_loaded},
            {"capture_safe", bootstrap.capture_safe},
            {"d3d12_loaded_before_bootstrap", bootstrap.d3d12_was_loaded},
            {"dxgi_loaded_before_bootstrap", bootstrap.dxgi_was_loaded},
            {"loaded_before_graphics_modules", bootstrap.loaded_before_graphics_modules},
            {"capture_path_template", rdc::capture_template()},
            {"active_device", format_pointer(reinterpret_cast<uintptr_t>(active_pair.device))},
            {"active_device_vtable_module", rdc::com_object_vtable_module(active_pair.device)},
            {"active_device_renderdoc_wrapped", rdc::com_object_looks_renderdoc_wrapped(active_pair.device)},
            {"active_window", format_pointer(reinterpret_cast<uintptr_t>(active_pair.window))},
            {"captures", std::move(captures)},
        });
    } catch (const std::exception& e) {
        return publish(json{{"loaded", false}, {"error", e.what()}});
    }
}

extern "C" UEVR_RENDER_CAPI const char* uevr_render_diag_renderdoc_trigger_capture(int num_frames) {
    try {
        auto* api = rdc::api();
        if (api == nullptr) {
            return publish(json{
                {"ok", false},
                {"error", "RenderDoc API not loaded - launch the game via the RenderDoc launcher first."},
            });
        }
        const uint32_t frames = num_frames <= 1 ? 1u : static_cast<uint32_t>(num_frames);
        if (frames == 1) {
            const auto attempt = renderdoc_capture_prefer_active_pair(std::chrono::milliseconds(250));
            return publish(json{
                {"ok", attempt.ended},
                {"queued_frames", frames},
                {"mode", attempt.mode},
                {"ended", attempt.ended},
                {"had_exact_pair", attempt.had_exact_pair},
                {"wildcard_fallback", attempt.wildcard_fallback},
                {"active_device", format_pointer(reinterpret_cast<uintptr_t>(attempt.pair.device))},
                {"active_window", format_pointer(reinterpret_cast<uintptr_t>(attempt.pair.window))},
                {"newest_capture", rdc::newest_capture_path()},
                {"num_captures", api->GetNumCaptures()},
            });
        }
        api->TriggerMultiFrameCapture(frames);
        return publish(json{{"ok", true}, {"queued_frames", frames}});
    } catch (const std::exception& e) {
        return publish(json{{"ok", false}, {"error", e.what()}});
    }
}

extern "C" UEVR_RENDER_CAPI const char* uevr_render_diag_renderdoc_launch_ui() {
    try {
        auto* api = rdc::api();
        if (api == nullptr) {
            return publish(json{{"ok", false}, {"error", "RenderDoc API not loaded"}});
        }
        const uint32_t pid = api->LaunchReplayUI(1, nullptr);
        if (pid == 0) {
            return publish(json{{"ok", false}, {"error", "LaunchReplayUI returned pid=0 (failed)"}});
        }
        return publish(json{{"ok", true}, {"pid", pid}});
    } catch (const std::exception& e) {
        return publish(json{{"ok", false}, {"error", e.what()}});
    }
}

extern "C" UEVR_RENDER_CAPI const char* uevr_render_diag_renderdoc_set_capture_template(const char* path_template) {
    try {
        auto* api = rdc::api();
        if (api == nullptr) {
            return publish(json{{"ok", false}, {"error", "RenderDoc API not loaded"}});
        }
        if (path_template != nullptr && *path_template != '\0') {
            rdc::set_capture_template(path_template);
        }
        return publish(json{{"ok", true}, {"template", rdc::capture_template()}});
    } catch (const std::exception& e) {
        return publish(json{{"ok", false}, {"error", e.what()}});
    }
}
