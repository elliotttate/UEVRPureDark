// RenderTargetPoolProvider.hpp
//
// Phase 2 provider. Activates UEVR's RenderTargetPool hook and, each frame, resolves known pooled
// render-target names (SceneDepthZ + velocity candidates) down to ID3D12Resource* and feeds them
// to the tracker. Engine-truth depth path; no DLSS, no new D3D12 hooks.
#pragma once

#include "../FrameResourceTracker.hpp"

namespace afw_fr {

class RenderTargetPoolProvider {
public:
    void activate();
    bool active() const { return m_active; }

    // Query pooled targets and emit observations for the current tracker frame.
    void update(FrameResourceTracker& tracker);

private:
    bool probe(FrameResourceTracker& tracker, const wchar_t* wname, const char* name, FrameResourceKind kind);
    bool m_active{false};
};

} // namespace afw_fr
