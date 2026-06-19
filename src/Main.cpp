// dllmain
#include <windows.h>
#include <cstdint>
#include <iterator>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <utility/Thread.hpp>

#include "Framework.hpp"
#include "render/RenderDocCaptureService.hpp"
#include "render/RenderDocDxgiProofHooks.hpp"

namespace {

void signal_renderdoc_launcher_ready() {
    wchar_t event_name[256]{};
    const DWORD len = GetEnvironmentVariableW(
        L"UEVR_RENDERDOC_READY_EVENT",
        event_name,
        static_cast<DWORD>(std::size(event_name)));
    if (len == 0 || len >= std::size(event_name)) {
        return;
    }

    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name);
    if (event == nullptr) {
        spdlog::warn("[RenderDoc] launcher ready event could not be opened");
        return;
    }

    SetEvent(event);
    CloseHandle(event);
    spdlog::info("[RenderDoc] launcher ready event signaled");
}

} // namespace

void startup_thread(HMODULE poc_module) {
    if (uevr::renderdoc_capture::env_truthy_w(L"UEVR_RENDERDOC_BOOTSTRAP")) {
        const auto renderdoc_bootstrap = uevr::renderdoc_capture::bootstrap(
            uevr::renderdoc_capture::env_truthy_w(L"UEVR_LOAD_RENDERDOC_DLL"));
        if (renderdoc_bootstrap.api_loaded) {
            uevr::renderdoc_capture::refresh_hooks();
        }
        uevr::renderdoc_dxgi_proof::install();
    }

    g_framework = std::make_unique<Framework>(poc_module);
    signal_renderdoc_launcher_ready();
}

BOOL APIENTRY DllMain(HANDLE handle, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)startup_thread, handle, 0, nullptr);
    }

    return TRUE;
}
