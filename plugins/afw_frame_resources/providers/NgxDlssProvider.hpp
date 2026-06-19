// NgxDlssProvider.hpp
//
// Phase 6 OPTIONAL observer. DLSS/NGX is never required by this plugin. This provider only does
// anything when _nvngx.dll/nvngx.dll is ALREADY loaded in the process AND the observer is explicitly
// enabled. It links no NVIDIA import library and never LoadLibrary's NGX. When absent/disabled it
// logs once and stays inert.
//
// The current implementation detects NGX presence and reports it; installing the EvaluateFeature
// observation hooks is a documented future step (kept out of the DLSS-off primary path on purpose).
#pragma once

#include "../FrameResourceTracker.hpp"

namespace afw_fr {

class NgxDlssProvider {
public:
    // Returns true if NGX modules are currently loaded in the process.
    static bool ngx_present();

    // Enable the observer if NGX is present. No-op (logged once) otherwise. Never fails startup.
    void try_enable();
    void disable();
    bool enabled() const { return m_enabled; }

private:
    bool m_enabled{false};
};

} // namespace afw_fr
