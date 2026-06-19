// NgxDlssProvider.cpp
#include "NgxDlssProvider.hpp"

#include "../FrameResourceLog.hpp"

#include <Windows.h>

namespace afw_fr {

bool NgxDlssProvider::ngx_present() {
    // Only check already-loaded modules; never LoadLibrary NGX.
    return GetModuleHandleW(L"_nvngx.dll") != nullptr || GetModuleHandleW(L"nvngx.dll") != nullptr;
}

void NgxDlssProvider::try_enable() {
    if (!ngx_present()) {
        if (log_once_key("ngx_absent")) {
            log_info("dlss observer disabled or NGX not loaded; continuing (DLSS is never required)");
        }
        m_enabled = false;
        return;
    }

    // NGX is loaded. Full EvaluateFeature observation hooks are intentionally deferred so the
    // DLSS-off path stays the primary, validated design. When implemented, this provider will
    // resolve NVSDK_NGX_D3D12_EvaluateFeature via GetProcAddress on the already-loaded module,
    // install an inline hook through param()->functions->register_inline_hook, and convert the
    // observed depth/MV into ObservedFrameResource for FrameResourceProvider::DlssNgx.
    m_enabled = true;
    if (log_once_key("ngx_present_observer")) {
        log_info("NGX present; dlss observer enabled (detection only in this build; observation hooks pending)");
    }
}

void NgxDlssProvider::disable() {
    m_enabled = false;
}

} // namespace afw_fr
