// D3D12BindProvider.hpp
//
// Phase 3 provider (log-only velocity discovery). Installs plugin-owned inline hooks on a small set
// of D3D12 vtable methods via the UEVR plugin hook API, maps descriptor handles to resource descs,
// and identifies velocity-shaped render-target binds. It does NOT snapshot/copy resources and does
// NOT modify src/hooks/D3D12Hook.cpp.
//
// SAFETY: this installs inline hooks on shared D3D12 runtime vtable entries. Enable via
// UEVR_FRAME_RESOURCES_ENABLE_D3D12BIND (default on per plan) but validate in-game before relying
// on it; uninstall() is called on device reset / teardown.
#pragma once

#include "../FrameResourceTracker.hpp"

namespace afw_fr {

class D3D12BindProvider {
public:
    // device: the live ID3D12Device from param()->renderer->device. Used to read vtables.
    void install(ID3D12Device* device);
    void uninstall();
    bool installed() const;

    // Score the latest velocity-shaped bind seen this frame and emit an observation (or a miss).
    void flush(FrameResourceTracker& tracker);
};

} // namespace afw_fr
