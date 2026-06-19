// PooledRenderTargetLayout.hpp
//
// The ONLY piece of UESDK memory layout the plugin copies. It mirrors just enough of
// dependencies/submodules/UESDK/src/sdk/StereoStuff.hpp to walk from the opaque pooled-render-target
// handle returned by API::RenderTargetPoolHook::get_render_target(...) down to the FRHITexture2D*.
//
// Verified source (UEVRPureDark, 2026-06-18):
//   src/mods/pluginloader/FRenderTargetPoolHook.cpp:15
//     -> the UEVR_IPooledRenderTargetHandle IS a direct cast of the internal sdk::IPooledRenderTarget*.
//   StereoStuff.hpp:219-224  struct IPooledRenderTarget : IRefCountedObject { ... FSceneRenderTargetItem item; }
//                            (IRefCountedObject contributes a single vtable pointer at offset 0,
//                             so `item` begins at +0x08 on x64.)
//   StereoStuff.hpp:212-217  struct FSceneRenderTargetItem { FTexture2DRHIRef texture; FTexture2DRHIRef srt; void** uav; char pad[0x20]; }
//   StereoStuff.hpp:63-89    struct FTexture2DRHIRef { FRHITexture2D* texture{nullptr}; }
//
// If a future engine/UESDK revision changes this layout, THIS FILE is the only place that needs
// to change. The static_asserts below catch the common x64 drift at compile time.
//
// The FRHITexture2D* obtained here is converted to ID3D12Resource* by the provider via the
// plugin-facing SDK function table (uevr::API::FRHITexture2D::get_native_resource()), NOT by
// linking StereoStuff.cpp. This header therefore has no UEVR/UESDK link dependency.
#pragma once

#include <cstddef>
#include <cstdint>

namespace afw_fr {

// Opaque; we only ever hold a pointer and hand it to the SDK get_native_resource() function.
struct MinFRHITexture2D;

struct MinFTexture2DRHIRef {
    MinFRHITexture2D* texture; // +0x00
};

struct MinSceneRenderTargetItem {
    MinFTexture2DRHIRef texture; // +0x00  target texture (this is the one we want)
    MinFTexture2DRHIRef srt;     // +0x08  shader-resource texture
    void** uav;                  // +0x10
    char pad[0x20];              // +0x18
};

struct MinPooledRenderTarget {
    void* vtable;                  // +0x00  IPooledRenderTarget/IRefCountedObject vptr
    MinSceneRenderTargetItem item; // +0x08
};

static_assert(sizeof(void*) == 8, "afw_frame_resources pooled-RT layout assumes x64 (8-byte pointers)");
static_assert(offsetof(MinSceneRenderTargetItem, texture) == 0, "FSceneRenderTargetItem.texture must be first");
static_assert(offsetof(MinPooledRenderTarget, item) == 8, "IPooledRenderTarget.item must follow the vtable pointer");

// Walk handle -> FRHITexture2D*. Returns nullptr (and sets *failed_hop) if any hop is null.
inline MinFRHITexture2D* pooled_rt_to_rhi_texture(void* handle, const char** failed_hop) {
    if (handle == nullptr) {
        if (failed_hop) *failed_hop = "handle";
        return nullptr;
    }
    auto* rt = reinterpret_cast<MinPooledRenderTarget*>(handle);
    MinFRHITexture2D* rhi = rt->item.texture.texture;
    if (rhi == nullptr) {
        if (failed_hop) *failed_hop = "item.texture.texture";
        return nullptr;
    }
    if (failed_hop) *failed_hop = nullptr;
    return rhi;
}

} // namespace afw_fr
