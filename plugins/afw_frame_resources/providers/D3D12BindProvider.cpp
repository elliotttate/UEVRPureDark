// D3D12BindProvider.cpp
#include "D3D12BindProvider.hpp"

#include "../FrameResourceLog.hpp"

#include "uevr/API.hpp"

#include <atomic>
#include <cwchar>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <d3dcommon.h>
#include <iterator>
#include <mutex>
#include <unordered_map>

// RenderDoc API — used only to query IsFrameCapturing() so we can suspend our injected CopyResource for the
// exact frame RenderDoc is serializing (see renderdoc_is_capturing / snapshot_velocity_resource_locked).
#include "../../../dependencies/vendor/renderdoc/renderdoc_app.h"

namespace afw_fr {
namespace {

// ---- D3D12 vtable indices (verified against d3d12.h interface ordering, x64) ----------------
// ID3D12Device : ID3D12Object : IUnknown
//   IUnknown(0..2) Object(3..6) then Device methods from 7:
//   ... CreateRenderTargetView = 20, CreateDepthStencilView = 21
constexpr unsigned kIdx_CreateRenderTargetView = 20;
constexpr unsigned kIdx_CreateDepthStencilView = 21;
constexpr unsigned kIdx_CreateCommittedResource = 27;
constexpr unsigned kIdx_CreatePlacedResource = 29;
// ID3D12GraphicsCommandList : ID3D12CommandList : ID3D12DeviceChild : ID3D12Object : IUnknown
//   ... ResourceBarrier = 26
//   ... OMSetRenderTargets = 46
constexpr unsigned kIdx_ResourceBarrier = 26;
constexpr unsigned kIdx_OMSetRenderTargets = 46;

// Safety valve: descriptor handles are normally reused (so the maps stay small), but if a title
// ever churns through distinct candidate-shaped descriptors faster than they are recycled we
// release everything and rebuild rather than pin an unbounded number of resources alive.
constexpr size_t kMaxTrackedViews = 4096;
constexpr uint32_t kMaxTrackedViewAgeFrames = 30;
constexpr uint32_t kStickyVelocityMaxAgeFrames = 6;
constexpr uint32_t kSceneVelocityHighWaterDecayFrames = 3600;
constexpr GUID kD3DDebugObjectNameW = {
    0x4cca5fd8, 0x921f, 0x42c8, {0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb7, 0x41}
};

struct D3D12ViewInfo {
    ID3D12Resource* resource{nullptr};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t sample_count{1};
    D3D12_RESOURCE_FLAGS flags{D3D12_RESOURCE_FLAG_NONE};
    uint32_t last_seen_frame{0};
    bool uevr_owned{false};
};

struct ResourceCandidate {
    bool present{false};
    D3D12ViewInfo info{};
    uint32_t frame{0};
    int score{0};
    const char* source{nullptr};
};

// ---- file-static hook state (detours are free functions and need global reach) ---------------
std::mutex g_state_mutex;
std::atomic<bool> g_installed{false};

// Both maps OWN a COM reference on every stored resource (AddRef on store, Release on overwrite /
// clear). That keeps the ID3D12Resource object alive between the CreateView detour that records it
// and the OMSetRenderTargets detour that promotes it. It does not prove UE5 RDG transient memory
// still contains velocity/depth contents; consumers should still treat stale resources cautiously.
std::unordered_map<SIZE_T, D3D12ViewInfo> g_rtv_views;
std::unordered_map<SIZE_T, D3D12ViewInfo> g_dsv_views;
ResourceCandidate g_velocity_candidate;
ResourceCandidate g_depth_candidate;
ResourceCandidate g_sticky_velocity_candidate;
ResourceCandidate g_scene_velocity_candidate;
// Depth has no per-frame guarantee of a bind (AFW alternates eyes; many frames bind only shadow /
// spectator DSVs), so — like velocity — keep a sticky copy of the best recent depth and serve it on
// frames where the eye depth DSV isn't (re)bound. Without this the depth entry goes Stale within
// max_stale_frames and the combine starves (no depth -> no reconstruction -> no motion vectors).
ResourceCandidate g_sticky_depth_candidate;
uint64_t g_scene_velocity_area = 0;
uint32_t g_scene_velocity_width = 0;
uint32_t g_scene_velocity_height = 0;
uint32_t g_scene_velocity_frame = 0;
UINT g_rtv_descriptor_increment = 0;
ID3D12Device* g_device = nullptr;

struct VelocitySnapshot {
    ID3D12Resource* resource{nullptr};
    ID3D12Resource* source_resource{nullptr};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t frame{0};
};

VelocitySnapshot g_velocity_snapshot{};

int g_hook_create_rtv = -1;
int g_hook_create_dsv = -1;
int g_hook_create_committed = -1;
int g_hook_create_placed = -1;
int g_hook_resource_barrier = -1;
int g_hook_omset = -1;

using PFN_CreateRenderTargetView = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*,
                                                            const D3D12_RENDER_TARGET_VIEW_DESC*,
                                                            D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CreateDepthStencilView = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*,
                                                            const D3D12_DEPTH_STENCIL_VIEW_DESC*,
                                                            D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CreateCommittedResource = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
                                                                D3D12_HEAP_FLAGS,
                                                                const D3D12_RESOURCE_DESC*,
                                                                D3D12_RESOURCE_STATES,
                                                                const D3D12_CLEAR_VALUE*, REFIID,
                                                                void**);
using PFN_CreatePlacedResource = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64,
                                                             const D3D12_RESOURCE_DESC*,
                                                             D3D12_RESOURCE_STATES,
                                                             const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_ResourceBarrier = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
                                                     const D3D12_RESOURCE_BARRIER*);
using PFN_OMSetRenderTargets = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
                                                        const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                                        const D3D12_CPU_DESCRIPTOR_HANDLE*);

// Atomic so a detour reading the trampoline can never observe a torn pointer while uninstall()
// nulls it on another thread.
std::atomic<PFN_CreateRenderTargetView> g_orig_create_rtv{nullptr};
std::atomic<PFN_CreateDepthStencilView> g_orig_create_dsv{nullptr};
std::atomic<PFN_CreateCommittedResource> g_orig_create_committed{nullptr};
std::atomic<PFN_CreatePlacedResource> g_orig_create_placed{nullptr};
std::atomic<PFN_ResourceBarrier> g_orig_resource_barrier{nullptr};
std::atomic<PFN_OMSetRenderTargets> g_orig_omset{nullptr};

// caller must hold g_state_mutex. AddRefs the incoming resource and releases any prior occupant of
// the slot, so the map's reference count stays balanced across overwrites.
void store_view_locked(std::unordered_map<SIZE_T, D3D12ViewInfo>& map, SIZE_T ptr, const D3D12ViewInfo& info) {
    if (map.size() >= kMaxTrackedViews && map.find(ptr) == map.end()) {
        for (auto& kv : map) {
            if (kv.second.resource != nullptr) kv.second.resource->Release();
        }
        map.clear();
        log_warn("D3D12Bind: tracked-view map hit %zu entries; cleared to bound memory", kMaxTrackedViews);
    }
    if (info.resource != nullptr) {
        info.resource->AddRef();
    }
    D3D12ViewInfo stored = info;
    stored.last_seen_frame = FrameResourceTracker::get().current_frame();

    auto it = map.find(ptr);
    if (it != map.end()) {
        if (it->second.resource != nullptr) it->second.resource->Release();
        it->second = stored;
    } else {
        map.emplace(ptr, stored);
    }
}

// caller must hold g_state_mutex.
void clear_views_locked(std::unordered_map<SIZE_T, D3D12ViewInfo>& map) {
    for (auto& kv : map) {
        if (kv.second.resource != nullptr) kv.second.resource->Release();
    }
    map.clear();
}

void prune_views_locked(std::unordered_map<SIZE_T, D3D12ViewInfo>& map, uint32_t current_frame) {
    for (auto it = map.begin(); it != map.end();) {
        const uint32_t seen_frame = it->second.last_seen_frame;
        const bool stale = current_frame >= seen_frame &&
                           (current_frame - seen_frame) > kMaxTrackedViewAgeFrames;
        if (stale) {
            if (it->second.resource != nullptr) {
                it->second.resource->Release();
            }
            it = map.erase(it);
        } else {
            ++it;
        }
    }
}

void release_candidate_locked(ResourceCandidate& candidate) {
    if (candidate.info.resource != nullptr) {
        candidate.info.resource->Release();
    }
    candidate = {};
}

ResourceCandidate copy_candidate_locked(const ResourceCandidate& candidate) {
    ResourceCandidate copy = candidate;
    if (copy.info.resource != nullptr) {
        copy.info.resource->AddRef();
    }
    return copy;
}

void replace_candidate_locked(ResourceCandidate& candidate, const D3D12ViewInfo& info, int score,
                              const char* source) {
    if (info.resource != nullptr) {
        info.resource->AddRef();
    }
    release_candidate_locked(candidate);
    candidate.present = true;
    candidate.info = info;
    candidate.frame = FrameResourceTracker::get().current_frame();
    candidate.score = score;
    candidate.source = source;
}

bool resource_debug_name_contains(ID3D12Resource* resource, const wchar_t* needle) {
    if (resource == nullptr || needle == nullptr || *needle == L'\0') {
        return false;
    }

    wchar_t name[256]{};
    UINT bytes = sizeof(name);
    const HRESULT hr = resource->GetPrivateData(kD3DDebugObjectNameW, &bytes, name);
    if (FAILED(hr) || bytes == 0) {
        return false;
    }

    const UINT chars = bytes / sizeof(wchar_t);
    const UINT last = chars < static_cast<UINT>(std::size(name)) ? chars : static_cast<UINT>(std::size(name)) - 1;
    name[last] = L'\0';
    return std::wcsstr(name, needle) != nullptr;
}

D3D12ViewInfo describe(ID3D12Resource* resource) {
    D3D12ViewInfo v;
    if (resource == nullptr) {
        return v;
    }
    const D3D12_RESOURCE_DESC d = resource->GetDesc();
    v.resource = resource;
    v.format = d.Format;
    v.width = static_cast<uint32_t>(d.Width);
    v.height = d.Height;
    v.sample_count = d.SampleDesc.Count;
    v.flags = d.Flags;
    return v;
}

// Two-channel 16-bit targets are the canonical UE velocity buffer (PF_G16R16 / float variant) and
// are a strong velocity signal. UE also emits velocity as PF_A16B16G16R16 (== R16G16B16A16_UNORM)
// when velocity-depth encoding is required (e.g. UE5.5/5.6 with DLSS) — that exact format is rare
// for anything else, so it is also canonical. Crucially, four-channel 16-bit *FLOAT* is HDR scene
// colour/bloom, NOT velocity, so it stays out of canonical and is only a weak shape signal.
bool is_canonical_velocity_format(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R16G16_UNORM || f == DXGI_FORMAT_R16G16_SNORM || f == DXGI_FORMAT_R16G16_FLOAT ||
           f == DXGI_FORMAT_R16G16B16A16_UNORM;
}

bool is_velocity_shaped_without_owner_check(const D3D12ViewInfo& v) {
    if (v.resource == nullptr) return false;
    if (v.sample_count != 1) return false;
    if (v.width < 32 || v.height < 32) return false;
    switch (v.format) {
    // UE 5.6 FVelocityRendering::GetFormat() uses PF_G16R16 on D3D, or PF_A16B16G16R16 when
    // velocity-depth encoding is required.
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_FLOAT:
        return true; // canonical
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return true; // possible-but-weak; tracked for discovery, reported only with corroboration
    default:
        return false;
    }
}

bool is_velocity_shaped(const D3D12ViewInfo& v) {
    return !v.uevr_owned && is_velocity_shaped_without_owner_check(v);
}

bool is_depth_shaped_without_owner_check(const D3D12ViewInfo& v) {
    if (v.resource == nullptr) return false;
    if (v.width < 32 || v.height < 32) return false;
    if ((v.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) return true;
    switch (v.format) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D16_UNORM:
        return true;
    default:
        return false;
    }
}

void populate_uevr_owned_if_candidate(D3D12ViewInfo& v) {
    if (v.resource == nullptr || v.uevr_owned) {
        return;
    }

    if (is_velocity_shaped_without_owner_check(v) || is_depth_shaped_without_owner_check(v)) {
        v.uevr_owned = resource_debug_name_contains(v.resource, L"UEVR");
    }
}

bool is_velocity_resource_create_candidate(const D3D12ViewInfo& v) {
    if (!is_velocity_shaped(v)) {
        return false;
    }
    if (!is_canonical_velocity_format(v.format)) {
        return false;
    }
    if ((v.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) {
        return false;
    }
    if ((v.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) {
        return false;
    }
    return true;
}

int depth_intrinsic_score(const D3D12ViewInfo& depth, const D3D12ViewInfo* best_rtv) {
    int s = 0;
    if ((depth.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) s += 3;
    if (depth.sample_count == 1) s += 1;
    if (depth.width >= 256 && depth.height >= 256) s += 1;

    // Scene depth is normally bound alongside scene/GBuffer RTVs. Shadow and custom-depth passes
    // often bind a DSV alone, so matching the current RTV set is a strong scene-depth signal.
    if (best_rtv != nullptr && best_rtv->resource != nullptr) {
        if (depth.width == best_rtv->width && depth.height == best_rtv->height) {
            s += 4;
        } else if (depth.width >= best_rtv->width && depth.height >= best_rtv->height) {
            s += 1; // tolerate guard-band / family-size depth larger than a cropped RTV
        }
    }

    return s;
}

bool is_better_depth_candidate(int score, const ResourceCandidate& current) {
    if (!current.present || current.info.resource == nullptr) {
        return true;
    }

    if (score != current.score) {
        return score > current.score;
    }

    // Same score: preserve the old behavior and let the latest equivalent DSV win.
    return true;
}

// Cheap intrinsic score used at bind time so the BEST velocity-shaped RTV in a frame wins, not just
// the last one bound (canonical RG16 beats an incidental RGBA16 colour target).
int velocity_intrinsic_score(const D3D12ViewInfo& v) {
    int s = 0;
    if (is_canonical_velocity_format(v.format)) s += 2;
    if (v.width >= 256 && v.height >= 256) s += 1;
    // Opt-in (UEVR_AFW_PREFER_ENCODED_VELOCITY): prefer the ENCODED velocity-depth buffer
    // (PF_A16B16G16R16 == R16G16B16A16_UNORM) — the exact buffer NVIDIA's VelocityCombine.usf reads
    // (DecodeVelocityFromTexture + the `EncodedVelocity.x > 0` written flag). On this title the default
    // RG16_FLOAT pick is an already-decoded / banded variant whose "written" pixels are scattered, so
    // porting DLSS's exact selection onto it reintroduces noise; the encoded RGBA16_UNORM buffer carries
    // the coherent written flag DLSS relies on. Gated so it cannot mislabel other titles' RGBA16 colour.
    static const bool s_prefer_encoded = []{
        const char* e = std::getenv("UEVR_AFW_PREFER_ENCODED_VELOCITY");
        return e != nullptr && *e != '\0' && std::atoi(e) != 0;
    }();
    if (s_prefer_encoded && v.format == DXGI_FORMAT_R16G16B16A16_UNORM) s += 3;
    return s;
}

uint64_t resource_area(const D3D12ViewInfo& v) {
    return static_cast<uint64_t>(v.width) * static_cast<uint64_t>(v.height);
}

bool env_bool(const char* name, bool default_value) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return default_value;
    }

    if (_stricmp(value, "0") == 0 || _stricmp(value, "false") == 0 || _stricmp(value, "off") == 0 ||
        _stricmp(value, "no") == 0) {
        return false;
    }

    if (_stricmp(value, "1") == 0 || _stricmp(value, "true") == 0 || _stricmp(value, "on") == 0 ||
        _stricmp(value, "yes") == 0) {
        return true;
    }

    return default_value;
}

bool velocity_snapshot_enabled() {
    // DEFAULT ON: the live-window snapshot of the first-per-frame velocity is how we deliver the real encoded
    // SceneVelocity to the combine (the live RDG resource is transient/recycled and the heuristic picks the wrong
    // dense RGBA16). Set UEVR_FRAME_RESOURCES_SNAPSHOT_VELOCITY=0 to fall back to serving the raw live candidate.
    return env_bool("UEVR_FRAME_RESOURCES_SNAPSHOT_VELOCITY", true);
}

void release_velocity_snapshot_locked() {
    if (g_velocity_snapshot.resource != nullptr) {
        g_velocity_snapshot.resource->Release();
    }
    g_velocity_snapshot = {};
}

bool velocity_snapshot_matches_locked(const D3D12ViewInfo& info) {
    return g_velocity_snapshot.resource != nullptr &&
           g_velocity_snapshot.width == info.width &&
           g_velocity_snapshot.height == info.height &&
           g_velocity_snapshot.format == info.format;
}

bool ensure_velocity_snapshot_locked(const D3D12ViewInfo& info) {
    if (g_device == nullptr || info.resource == nullptr || info.width == 0 || info.height == 0 ||
        info.format == DXGI_FORMAT_UNKNOWN) {
        return false;
    }

    if (velocity_snapshot_matches_locked(info)) {
        return true;
    }

    release_velocity_snapshot_locked();

    auto desc = info.resource->GetDesc();
    desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    ID3D12Resource* snapshot = nullptr;
    const HRESULT hr = g_device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&snapshot));

    if (FAILED(hr) || snapshot == nullptr) {
        log_warn("D3D12Bind: failed to create velocity snapshot %ux%u fmt=%u hr=0x%08x",
                 info.width, info.height, static_cast<unsigned>(info.format), static_cast<unsigned>(hr));
        return false;
    }

    g_velocity_snapshot.resource = snapshot;
    g_velocity_snapshot.format = info.format;
    g_velocity_snapshot.width = info.width;
    g_velocity_snapshot.height = info.height;
    log_info("D3D12Bind: created velocity snapshot %ux%u fmt=%u",
             info.width, info.height, static_cast<unsigned>(info.format));
    return true;
}

void command_list_transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (list == nullptr || resource == nullptr || before == after) {
        return;
    }

    auto orig = g_orig_resource_barrier.load(std::memory_order_acquire);
    if (orig == nullptr) {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    orig(list, 1, &barrier);
}

// True only while RenderDoc is actively capturing this frame (lazy-loaded; false when renderdoc.dll absent).
// During a capture RenderDoc serializes the engine command list; injecting our snapshot CopyResource onto it
// then stalls RenderDoc's capture thread (the original reason the bind provider was disabled wholesale under
// RenderDoc). We instead suspend just that injected copy for the captured frame.
bool renderdoc_is_capturing() {
    static RENDERDOC_API_1_7_0* s_api = nullptr;
    static bool s_resolved = false;
    if (!s_resolved) {
        s_resolved = true;
        if (HMODULE mod = GetModuleHandleW(L"renderdoc.dll")) {
            if (auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"))) {
                void* api_ptr = nullptr;
                if (get_api(eRENDERDOC_API_Version_1_7_0, &api_ptr) == 1) {
                    s_api = static_cast<RENDERDOC_API_1_7_0*>(api_ptr);
                }
            }
        }
    }
    return s_api != nullptr && s_api->IsFrameCapturing() != 0;
}

void snapshot_velocity_resource_locked(ID3D12GraphicsCommandList* list, const D3D12ViewInfo& info,
                                       D3D12_RESOURCE_STATES current_state, uint32_t current_frame,
                                       const char* source) {
    if (!velocity_snapshot_enabled() || list == nullptr || info.resource == nullptr) {
        return;
    }

    // Suspend the injected copy while RenderDoc is capturing this frame: our CopyResource on the engine list
    // would stall RenderDoc's in-flight capture. The captured frame keeps the previous stable snapshot, so the
    // AFW combine still runs; non-capture frames are unaffected. This lets AFW stay live under RenderDoc AND
    // lets frame captures complete (instead of the old all-or-nothing: provider-off = capture-only, no AFW).
    if (renderdoc_is_capturing()) {
        return;
    }

    if (!ensure_velocity_snapshot_locked(info) || g_velocity_snapshot.resource == nullptr) {
        return;
    }

    command_list_transition(list, info.resource, current_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(g_velocity_snapshot.resource, info.resource);
    command_list_transition(list, info.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, current_state);

    g_velocity_snapshot.source_resource = info.resource;
    g_velocity_snapshot.frame = current_frame;
    log_debug("D3D12Bind: snapped velocity ptr=0x%p into stable copy %ux%u fmt=%u source=%s frame=%u",
              static_cast<void*>(info.resource), info.width, info.height,
              static_cast<unsigned>(info.format), source ? source : "?", current_frame);
}

bool velocity_matches_scene_size(const D3D12ViewInfo& velocity, const FrameResourceView& depth) {
    if (depth.validity != FrameResourceValidity::Valid || depth.width == 0 || depth.height == 0) {
        return velocity.width >= 256 && velocity.height >= 256;
    }

    if (velocity.width == depth.width && velocity.height == depth.height) {
        return true;
    }

    // Projection growth can make scene velocity larger than the cropped depth buffer.
    return velocity.width >= depth.width && velocity.height >= depth.height;
}

bool is_better_velocity_candidate(const D3D12ViewInfo& info, int score, const ResourceCandidate& current) {
    if (!current.present || current.info.resource == nullptr) {
        return true;
    }

    if (score != current.score) {
        return score > current.score;
    }

    const uint64_t area = resource_area(info);
    const uint64_t current_area = resource_area(current.info);
    if (area != current_area) {
        return area > current_area;
    }

    // Same score and same dimensions: keep existing latest-bind behavior for genuinely equivalent
    // targets.
    return true;
}

// Sticky-depth comparison: like velocity, prefer the higher intrinsic score, then the LARGER buffer.
// is_better_depth_candidate is score-then-latest-wins, which lets an incidental equal-score low-res DSV
// (e.g. the engine's 1004x628 spectator depth) displace the full-res eye depth that the combine needs.
// The area tiebreak keeps the eye-resolution depth pinned as the sticky.
bool is_better_sticky_depth(const D3D12ViewInfo& info, int score, const ResourceCandidate& current) {
    if (!current.present || current.info.resource == nullptr) {
        return true;
    }
    if (score != current.score) {
        return score > current.score;
    }
    const uint64_t area = resource_area(info);
    const uint64_t current_area = resource_area(current.info);
    if (area != current_area) {
        return area > current_area;
    }
    return true; // equal score and size: latest equivalent DSV wins
}

void reset_scene_velocity_high_water_locked() {
    release_candidate_locked(g_scene_velocity_candidate);
    g_scene_velocity_area = 0;
    g_scene_velocity_width = 0;
    g_scene_velocity_height = 0;
    g_scene_velocity_frame = 0;
}

void decay_scene_velocity_high_water_locked(uint32_t current_frame) {
    if (g_scene_velocity_area == 0) {
        return;
    }
    if (env_bool("UEVR_FRAME_RESOURCES_HOLD_SCENE_VELOCITY", false)) {
        return;
    }
    if (current_frame < g_scene_velocity_frame) {
        reset_scene_velocity_high_water_locked();
        return;
    }
    if ((current_frame - g_scene_velocity_frame) <= kSceneVelocityHighWaterDecayFrames) {
        return;
    }

    log_info("D3D12Bind: scene velocity high-water %ux%u expired after %u frames",
             g_scene_velocity_width, g_scene_velocity_height, current_frame - g_scene_velocity_frame);
    reset_scene_velocity_high_water_locked();
}

bool accept_velocity_candidate_locked(const D3D12ViewInfo& info, bool allow_low_res, uint32_t current_frame) {
    decay_scene_velocity_high_water_locked(current_frame);

    const uint64_t area = resource_area(info);
    if (area == 0) {
        return false;
    }

    if (is_canonical_velocity_format(info.format) && area >= g_scene_velocity_area) {
        if (area > g_scene_velocity_area) {
            log_info("D3D12Bind: scene velocity high-water now %ux%u fmt=%u (previous %ux%u)",
                     info.width, info.height, static_cast<unsigned>(info.format),
                     g_scene_velocity_width, g_scene_velocity_height);
        }
        g_scene_velocity_area = area;
        g_scene_velocity_width = info.width;
        g_scene_velocity_height = info.height;
        g_scene_velocity_frame = current_frame;
        return true;
    }

    if (!allow_low_res && g_scene_velocity_area != 0 && area < g_scene_velocity_area) {
        char key[128];
        std::snprintf(key, sizeof(key), "velgate:%ux%u:%u:%ux%u", info.width, info.height,
                      static_cast<unsigned>(info.format), g_scene_velocity_width, g_scene_velocity_height);
        if (log_once_key(key)) {
            log_info("D3D12Bind: rejected low-res velocity %ux%u fmt=%u below scene high-water %ux%u",
                     info.width, info.height, static_cast<unsigned>(info.format),
                     g_scene_velocity_width, g_scene_velocity_height);
        }
        return false;
    }

    return true;
}

void remember_scene_velocity_candidate_locked(const D3D12ViewInfo& info, int score, const char* source) {
    if (!is_canonical_velocity_format(info.format)) {
        return;
    }
    if (g_scene_velocity_area == 0 || resource_area(info) < g_scene_velocity_area) {
        return;
    }
    if (is_better_velocity_candidate(info, score, g_scene_velocity_candidate)) {
        replace_candidate_locked(g_scene_velocity_candidate, info, score, source);
    }
}

bool scene_velocity_candidate_available_locked(uint32_t current_frame) {
    decay_scene_velocity_high_water_locked(current_frame);
    if (!g_scene_velocity_candidate.present || g_scene_velocity_candidate.info.resource == nullptr) {
        return false;
    }
    if (current_frame < g_scene_velocity_candidate.frame) {
        reset_scene_velocity_high_water_locked();
        return false;
    }
    if (env_bool("UEVR_FRAME_RESOURCES_HOLD_SCENE_VELOCITY", false)) {
        return true;
    }
    return (current_frame - g_scene_velocity_candidate.frame) <= kStickyVelocityMaxAgeFrames;
}

void consider_created_velocity_resource(ID3D12Resource* resource, const char* source) {
    if (resource == nullptr || !g_installed.load(std::memory_order_acquire)) {
        return;
    }

    D3D12ViewInfo info = describe(resource);
    populate_uevr_owned_if_candidate(info);
    if (!is_velocity_resource_create_candidate(info)) {
        return;
    }

    std::scoped_lock lock(g_state_mutex);
    const bool allow_low_res_velocity = env_bool("UEVR_FRAME_RESOURCES_ALLOW_LOW_RES_VELOCITY", false);
    const uint32_t current_frame = FrameResourceTracker::get().current_frame();
    if (!accept_velocity_candidate_locked(info, allow_low_res_velocity, current_frame)) {
        return;
    }

    const int s = velocity_intrinsic_score(info);
    remember_scene_velocity_candidate_locked(info, s, source);
    if (is_better_velocity_candidate(info, s, g_velocity_candidate)) {
        replace_candidate_locked(g_velocity_candidate, info, s, source);
    }
}

bool should_remember_sticky_velocity(const ResourceCandidate& candidate, const ResourceCandidate& sticky) {
    if (!candidate.present || candidate.info.resource == nullptr) {
        return false;
    }
    if (!sticky.present || sticky.info.resource == nullptr) {
        return true;
    }
    return is_better_velocity_candidate(candidate.info, candidate.score, sticky);
}

bool is_recent_candidate(uint32_t current_frame, const ResourceCandidate& candidate) {
    if (!candidate.present || candidate.info.resource == nullptr) {
        return false;
    }
    if (current_frame < candidate.frame) {
        return false;
    }
    return (current_frame - candidate.frame) <= kStickyVelocityMaxAgeFrames;
}

bool is_depth_shaped(const D3D12ViewInfo& v) {
    return !v.uevr_owned && is_depth_shaped_without_owner_check(v);
}

bool state_has(D3D12_RESOURCE_STATES states, D3D12_RESOURCE_STATES mask) {
    return (states & mask) != 0;
}

bool transitioned_through(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
                          D3D12_RESOURCE_STATES mask) {
    return state_has(before, mask) || state_has(after, mask);
}

void consider_barrier_resource(ID3D12GraphicsCommandList* command_list,
                               ID3D12Resource* resource,
                               D3D12_RESOURCE_STATES before,
                               D3D12_RESOURCE_STATES after,
                               const char* source) {
    if (resource == nullptr || !g_installed.load(std::memory_order_acquire)) {
        return;
    }

    D3D12ViewInfo info = describe(resource);
    if (!is_depth_shaped_without_owner_check(info) && !is_velocity_shaped_without_owner_check(info)) {
        return;
    }
    populate_uevr_owned_if_candidate(info);

    std::scoped_lock lock(g_state_mutex);
    const uint32_t current_frame = FrameResourceTracker::get().current_frame();

    if (is_depth_shaped(info) &&
        transitioned_through(before, after, D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ)) {
        int s = depth_intrinsic_score(info, nullptr);
        if (transitioned_through(before, after, D3D12_RESOURCE_STATE_DEPTH_WRITE)) {
            s += 2;
        }
        if (is_better_depth_candidate(s, g_depth_candidate)) {
            replace_candidate_locked(g_depth_candidate, info, s, source);
        }
    }

    // Descriptor creation often happens before this plugin is initialized. ResourceBarrier sees
    // the already-existing RDG resources every frame, so it can recover the canonical UE velocity
    // target without needing a CreateRenderTargetView hit.
    if (is_velocity_resource_create_candidate(info) &&
        transitioned_through(before, after, D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
        const bool allow_low_res_velocity = env_bool("UEVR_FRAME_RESOURCES_ALLOW_LOW_RES_VELOCITY", false);
        if (!accept_velocity_candidate_locked(info, allow_low_res_velocity, current_frame)) {
            return;
        }

        const int s = velocity_intrinsic_score(info) + 1;
        remember_scene_velocity_candidate_locked(info, s, source);
        if (is_better_velocity_candidate(info, s, g_velocity_candidate)) {
            replace_candidate_locked(g_velocity_candidate, info, s, source);
        }

        const bool leaving_writable_velocity_state =
            state_has(before, D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
            !state_has(after, D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // Snapshot only the FIRST velocity-shaped target that leaves the writable state each frame. The engine writes
        // the real SceneVelocity in the velocity pass (early); later RGBA16 post-process buffers (same shape) leave RT
        // afterward and would otherwise OVERWRITE the snapshot with the wrong, dense buffer. Capturing the first one — in
        // its live window, content still valid, state known (`after`) — gives us the authoritative encoded SceneVelocity.
        if (leaving_writable_velocity_state && resource_area(info) >= g_scene_velocity_area &&
            g_velocity_snapshot.frame != current_frame) {
            snapshot_velocity_resource_locked(command_list, info, after, current_frame, source);
        }
    }
}

void STDMETHODCALLTYPE detour_CreateRenderTargetView(ID3D12Device* self, ID3D12Resource* resource,
                                                     const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    if (auto orig = g_orig_create_rtv.load(std::memory_order_acquire)) {
        orig(self, resource, desc, dest);
    }
    if (!g_installed.load(std::memory_order_acquire)) {
        return; // tearing down: pass through, do no bookkeeping
    }
    try {
        if (resource != nullptr && dest.ptr != 0) {
            D3D12ViewInfo info = describe(resource);
            if (is_velocity_shaped_without_owner_check(info)) {
                populate_uevr_owned_if_candidate(info);
            }
            if (log_level() >= LogLevel::Trace) {
                char key[96];
                std::snprintf(key, sizeof(key), "rtv:%p:%ux%u:%u", (void*)resource, info.width, info.height,
                              (unsigned)info.format);
                if (log_once_key(key)) {
                    log_trace("d3d12 rtv record ptr=0x%p %ux%u fmt=%u flags=0x%x", (void*)resource, info.width,
                              info.height, (unsigned)info.format, (unsigned)info.flags);
                }
            }
            // Only track candidate-shaped RTVs: cuts mutex/map churn on the hot path and bounds the
            // map (and thus the use-after-free surface) to a handful of velocity-shaped targets.
            if (is_velocity_shaped(info)) {
                std::scoped_lock lock(g_state_mutex);
                store_view_locked(g_rtv_views, dest.ptr, info);
            }
        }
    } catch (...) {
        // never let bookkeeping escape into the game
    }
}

void STDMETHODCALLTYPE detour_CreateDepthStencilView(ID3D12Device* self, ID3D12Resource* resource,
                                                     const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    if (auto orig = g_orig_create_dsv.load(std::memory_order_acquire)) {
        orig(self, resource, desc, dest);
    }
    if (!g_installed.load(std::memory_order_acquire)) {
        return;
    }
    try {
        if (resource != nullptr && dest.ptr != 0) {
            D3D12ViewInfo info = describe(resource);
            if (is_depth_shaped_without_owner_check(info)) {
                populate_uevr_owned_if_candidate(info);
            }
            if (is_depth_shaped(info)) {
                std::scoped_lock lock(g_state_mutex);
                store_view_locked(g_dsv_views, dest.ptr, info);
            }
        }
    } catch (...) {
    }
}

HRESULT STDMETHODCALLTYPE detour_CreateCommittedResource(ID3D12Device* self,
                                                         const D3D12_HEAP_PROPERTIES* heap_properties,
                                                         D3D12_HEAP_FLAGS heap_flags,
                                                         const D3D12_RESOURCE_DESC* desc,
                                                         D3D12_RESOURCE_STATES initial_state,
                                                         const D3D12_CLEAR_VALUE* clear_value,
                                                         REFIID riid_resource,
                                                         void** resource_out) {
    auto orig = g_orig_create_committed.load(std::memory_order_acquire);
    if (orig == nullptr) {
        return E_FAIL;
    }

    const HRESULT hr = orig(self, heap_properties, heap_flags, desc, initial_state, clear_value, riid_resource,
                            resource_out);
    if (!g_installed.load(std::memory_order_acquire) || FAILED(hr) || resource_out == nullptr ||
        *resource_out == nullptr) {
        return hr;
    }

    try {
        ID3D12Resource* resource = nullptr;
        if (SUCCEEDED(reinterpret_cast<IUnknown*>(*resource_out)->QueryInterface(IID_PPV_ARGS(&resource)))) {
            consider_created_velocity_resource(resource, "CreateCommittedResource");
            resource->Release();
        }
    } catch (...) {
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE detour_CreatePlacedResource(ID3D12Device* self,
                                                      ID3D12Heap* heap,
                                                      UINT64 heap_offset,
                                                      const D3D12_RESOURCE_DESC* desc,
                                                      D3D12_RESOURCE_STATES initial_state,
                                                      const D3D12_CLEAR_VALUE* clear_value,
                                                      REFIID riid_resource,
                                                      void** resource_out) {
    auto orig = g_orig_create_placed.load(std::memory_order_acquire);
    if (orig == nullptr) {
        return E_FAIL;
    }

    const HRESULT hr = orig(self, heap, heap_offset, desc, initial_state, clear_value, riid_resource, resource_out);
    if (!g_installed.load(std::memory_order_acquire) || FAILED(hr) || resource_out == nullptr ||
        *resource_out == nullptr) {
        return hr;
    }

    try {
        ID3D12Resource* resource = nullptr;
        if (SUCCEEDED(reinterpret_cast<IUnknown*>(*resource_out)->QueryInterface(IID_PPV_ARGS(&resource)))) {
            consider_created_velocity_resource(resource, "CreatePlacedResource");
            resource->Release();
        }
    } catch (...) {
    }

    return hr;
}

void STDMETHODCALLTYPE detour_ResourceBarrier(ID3D12GraphicsCommandList* self, UINT num_barriers,
                                              const D3D12_RESOURCE_BARRIER* barriers) {
    if (auto orig = g_orig_resource_barrier.load(std::memory_order_acquire)) {
        orig(self, num_barriers, barriers);
    }
    if (!g_installed.load(std::memory_order_acquire) || barriers == nullptr) {
        return;
    }

    try {
        for (UINT i = 0; i < num_barriers; ++i) {
            const auto& barrier = barriers[i];
            if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                continue;
            }
            consider_barrier_resource(self,
                                      barrier.Transition.pResource,
                                      barrier.Transition.StateBefore,
                                      barrier.Transition.StateAfter,
                                      "ResourceBarrier");
        }
    } catch (...) {
    }
}

void STDMETHODCALLTYPE detour_OMSetRenderTargets(ID3D12GraphicsCommandList* self, UINT num_rtvs,
                                                 const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
                                                 BOOL single_handle_range,
                                                 const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    if (auto orig = g_orig_omset.load(std::memory_order_acquire)) {
        orig(self, num_rtvs, rtvs, single_handle_range, dsv);
    }
    if (!g_installed.load(std::memory_order_acquire)) {
        return;
    }
    try {
        std::scoped_lock lock(g_state_mutex);
        const uint32_t current_frame = FrameResourceTracker::get().current_frame();
        D3D12ViewInfo best_bound_rtv;
        bool have_bound_rtv = false;
        if (rtvs != nullptr && num_rtvs > 0) {
            const bool contiguous = single_handle_range != FALSE;
            const UINT count = (contiguous && g_rtv_descriptor_increment == 0) ? 1u : num_rtvs;
            for (UINT i = 0; i < count; ++i) {
                const SIZE_T ptr = contiguous
                                       ? (rtvs[0].ptr + static_cast<SIZE_T>(i) * g_rtv_descriptor_increment)
                                       : rtvs[i].ptr;
                if (ptr == 0) continue;
                auto it = g_rtv_views.find(ptr);
                if (it == g_rtv_views.end()) continue;
                it->second.last_seen_frame = current_frame;
                if (!have_bound_rtv || resource_area(it->second) > resource_area(best_bound_rtv)) {
                    best_bound_rtv = it->second;
                    have_bound_rtv = true;
                }
            }
        }

        if (dsv != nullptr && dsv->ptr != 0) {
            auto it = g_dsv_views.find(dsv->ptr);
            if (it != g_dsv_views.end() && is_depth_shaped(it->second)) {
                it->second.last_seen_frame = current_frame;
                const int s = depth_intrinsic_score(it->second, have_bound_rtv ? &best_bound_rtv : nullptr);
                if (is_better_depth_candidate(s, g_depth_candidate)) {
                    replace_candidate_locked(g_depth_candidate, it->second, s, "OMSetRenderTargets.DSV");
                }
            }
        }

        if (rtvs != nullptr && num_rtvs > 0) {
            const bool allow_low_res_velocity = env_bool("UEVR_FRAME_RESOURCES_ALLOW_LOW_RES_VELOCITY", false);
            // single_handle_range == TRUE: rtvs[0] starts a contiguous range and handle i lives at
            // rtvs[0].ptr + i * increment (we cached the RTV increment at install). Otherwise rtvs is
            // an array of num_rtvs individual handles. Only fall back to slot 0 if the increment is
            // unknown.
            const bool contiguous = single_handle_range != FALSE;
            const UINT count = (contiguous && g_rtv_descriptor_increment == 0) ? 1u : num_rtvs;
            for (UINT i = 0; i < count; ++i) {
                const SIZE_T ptr = contiguous
                                       ? (rtvs[0].ptr + static_cast<SIZE_T>(i) * g_rtv_descriptor_increment)
                                       : rtvs[i].ptr;
                if (ptr == 0) continue;
                auto it = g_rtv_views.find(ptr);
                if (it == g_rtv_views.end()) continue;
                it->second.last_seen_frame = current_frame;
                if (!is_velocity_shaped(it->second)) continue;
                if (!accept_velocity_candidate_locked(it->second, allow_low_res_velocity, current_frame)) continue;
                const int s = velocity_intrinsic_score(it->second);
                remember_scene_velocity_candidate_locked(it->second, s, "OMSetRenderTargets.SceneVelocity");
                // Keep the best velocity-shaped bind this frame. Ties prefer the largest canonical
                // RG16 target, which is the full-resolution scene velocity when projection growth is active.
                if (is_better_velocity_candidate(it->second, s, g_velocity_candidate)) {
                    replace_candidate_locked(g_velocity_candidate, it->second, s, "OMSetRenderTargets");
                }
            }
        }
    } catch (...) {
    }
}

// Resolve a vtable function pointer from a COM interface instance.
void* vtable_entry(void* com_object, unsigned index) {
    if (com_object == nullptr) return nullptr;
    void** vtbl = *reinterpret_cast<void***>(com_object);
    return vtbl != nullptr ? vtbl[index] : nullptr;
}

} // namespace

void D3D12BindProvider::install(ID3D12Device* device) {
    if (g_installed.load()) {
        return;
    }
    if (device == nullptr) {
        log_warn("D3D12Bind: no device; provider disabled");
        return;
    }
    const auto* fns = uevr::API::get()->param()->functions;
    if (fns == nullptr || fns->register_inline_hook == nullptr) {
        log_warn("D3D12Bind: register_inline_hook unavailable; provider disabled");
        return;
    }

    g_device = device;
    g_device->AddRef();

    // Cache the RTV descriptor increment so OMSetRenderTargets can walk a contiguous handle range.
    g_rtv_descriptor_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Device vtable methods.
    void* create_rtv = vtable_entry(device, kIdx_CreateRenderTargetView);
    void* create_dsv = vtable_entry(device, kIdx_CreateDepthStencilView);
    const bool enable_resource_create_hooks = env_bool("UEVR_FRAME_RESOURCES_ENABLE_RESOURCE_CREATE", false);
    void* create_committed = enable_resource_create_hooks ? vtable_entry(device, kIdx_CreateCommittedResource) : nullptr;
    void* create_placed = enable_resource_create_hooks ? vtable_entry(device, kIdx_CreatePlacedResource) : nullptr;

    // Command-list vtable: spin up a throwaway DIRECT list to read the shared vtable, then release
    // the instances (the patched function code persists in the runtime).
    void* resource_barrier = nullptr;
    void* omset = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                            IID_PPV_ARGS(&list)))) {
        resource_barrier = vtable_entry(list, kIdx_ResourceBarrier);
        omset = vtable_entry(list, kIdx_OMSetRenderTargets);
        list->Close();
    }
    if (list != nullptr) list->Release();
    if (alloc != nullptr) alloc->Release();

    int installed = 0;
    // Installs one hook and returns the trampoline (or nullptr). hook_id is set to the registration
    // id on success so uninstall() can undo it.
    auto install_one = [&](void* target, void* detour, const char* name, int& hook_id) -> void* {
        hook_id = -1;
        if (target == nullptr) {
            log_warn("D3D12Bind: could not resolve %s vtable", name);
            return nullptr;
        }
        void* original = nullptr;
        const int id = fns->register_inline_hook(target, detour, &original);
        if (id >= 0 && original != nullptr) {
            hook_id = id;
            ++installed;
            return original;
        }
        if (id >= 0 && fns->unregister_inline_hook != nullptr) {
            fns->unregister_inline_hook(id);
        }
        log_warn("D3D12Bind: failed to hook %s", name);
        return nullptr;
    };

    g_orig_create_rtv.store(reinterpret_cast<PFN_CreateRenderTargetView>(
        install_one(create_rtv, reinterpret_cast<void*>(&detour_CreateRenderTargetView),
                    "CreateRenderTargetView", g_hook_create_rtv)));
    g_orig_create_dsv.store(reinterpret_cast<PFN_CreateDepthStencilView>(
        install_one(create_dsv, reinterpret_cast<void*>(&detour_CreateDepthStencilView),
                    "CreateDepthStencilView", g_hook_create_dsv)));
    if (enable_resource_create_hooks) {
        g_orig_create_committed.store(reinterpret_cast<PFN_CreateCommittedResource>(
            install_one(create_committed, reinterpret_cast<void*>(&detour_CreateCommittedResource),
                        "CreateCommittedResource", g_hook_create_committed)));
        g_orig_create_placed.store(reinterpret_cast<PFN_CreatePlacedResource>(
            install_one(create_placed, reinterpret_cast<void*>(&detour_CreatePlacedResource),
                        "CreatePlacedResource", g_hook_create_placed)));
    } else {
        log_info("D3D12Bind resource-create hooks disabled; set UEVR_FRAME_RESOURCES_ENABLE_RESOURCE_CREATE=1 for diagnostics");
    }
    g_orig_resource_barrier.store(reinterpret_cast<PFN_ResourceBarrier>(
        install_one(resource_barrier, reinterpret_cast<void*>(&detour_ResourceBarrier), "ResourceBarrier",
                    g_hook_resource_barrier)));
    g_orig_omset.store(reinterpret_cast<PFN_OMSetRenderTargets>(
        install_one(omset, reinterpret_cast<void*>(&detour_OMSetRenderTargets), "OMSetRenderTargets",
                    g_hook_omset)));

    // Publish "installed" last so any detour firing during registration passes through without
    // touching half-initialized bookkeeping state.
    g_installed.store(installed > 0, std::memory_order_release);
    log_info("D3D12Bind provider installed (%d/%d hooks)", installed,
             enable_resource_create_hooks ? 6 : 4);
}

void D3D12BindProvider::uninstall() {
    if (!g_installed.load()) {
        return;
    }
    // Stop bookkeeping first so in-flight detours skip the maps, then remove the hooks, then drop
    // the trampolines. (on_device_reset runs at device teardown, when rendering is quiesced, so the
    // residual window between unhooking and the last in-flight detour call is not exercised.)
    g_installed.store(false, std::memory_order_release);

    const auto* fns = uevr::API::get()->param()->functions;
    if (fns != nullptr && fns->unregister_inline_hook != nullptr) {
        if (g_hook_create_rtv >= 0) fns->unregister_inline_hook(g_hook_create_rtv);
        if (g_hook_create_dsv >= 0) fns->unregister_inline_hook(g_hook_create_dsv);
        if (g_hook_create_committed >= 0) fns->unregister_inline_hook(g_hook_create_committed);
        if (g_hook_create_placed >= 0) fns->unregister_inline_hook(g_hook_create_placed);
        if (g_hook_resource_barrier >= 0) fns->unregister_inline_hook(g_hook_resource_barrier);
        if (g_hook_omset >= 0) fns->unregister_inline_hook(g_hook_omset);
    }
    g_hook_create_rtv = g_hook_create_dsv = g_hook_create_committed = g_hook_create_placed = -1;
    g_hook_resource_barrier = g_hook_omset = -1;
    g_orig_create_rtv.store(nullptr);
    g_orig_create_dsv.store(nullptr);
    g_orig_create_committed.store(nullptr);
    g_orig_create_placed.store(nullptr);
    g_orig_resource_barrier.store(nullptr);
    g_orig_omset.store(nullptr);
    g_rtv_descriptor_increment = 0;
    {
        std::scoped_lock lock(g_state_mutex);
        clear_views_locked(g_rtv_views);
        clear_views_locked(g_dsv_views);
        release_candidate_locked(g_velocity_candidate);
        release_candidate_locked(g_depth_candidate);
        release_candidate_locked(g_sticky_velocity_candidate);
        release_candidate_locked(g_sticky_depth_candidate);
        reset_scene_velocity_high_water_locked();
        release_velocity_snapshot_locked();
    }
    if (g_device != nullptr) {
        g_device->Release();
        g_device = nullptr;
    }
    log_info("D3D12Bind provider uninstalled");
}

bool D3D12BindProvider::installed() const {
    return g_installed.load();
}

void D3D12BindProvider::flush(FrameResourceTracker& tracker) {
    if (!g_installed.load()) {
        return;
    }

    ResourceCandidate depth_cand;
    ResourceCandidate cand;
    ResourceCandidate sticky_cand;
    ResourceCandidate scene_cand;
    ResourceCandidate sticky_depth_cand;
    const uint32_t current_frame = FrameResourceTracker::get().current_frame();
    {
        std::scoped_lock lock(g_state_mutex);
        prune_views_locked(g_rtv_views, current_frame);
        prune_views_locked(g_dsv_views, current_frame);
        if ((current_frame % 120u) == 0u) {
            log_info("D3D12Bind: tracked view maps rtv=%zu dsv=%zu", g_rtv_views.size(),
                     g_dsv_views.size());
        }

        depth_cand = g_depth_candidate;
        g_depth_candidate = {};    // consume; depth_cand owns the temporary reference
        cand = g_velocity_candidate;
        g_velocity_candidate = {}; // consume; cand owns the temporary reference

        if (should_remember_sticky_velocity(cand, g_sticky_velocity_candidate)) {
            replace_candidate_locked(g_sticky_velocity_candidate, cand.info, cand.score, cand.source);
            g_sticky_velocity_candidate.frame = cand.frame;
        } else if (g_sticky_velocity_candidate.present &&
                   !is_recent_candidate(current_frame, g_sticky_velocity_candidate)) {
            release_candidate_locked(g_sticky_velocity_candidate);
        }

        if (is_recent_candidate(current_frame, g_sticky_velocity_candidate)) {
            sticky_cand = copy_candidate_locked(g_sticky_velocity_candidate);
        }
        if (scene_velocity_candidate_available_locked(current_frame)) {
            scene_cand = copy_candidate_locked(g_scene_velocity_candidate);
        }

        // Maintain the sticky depth (mirror of the velocity sticky above): pin the best/largest recent
        // depth so a frame without an eye-depth DSV bind still serves a Valid depth to the combine.
        if (depth_cand.present && depth_cand.info.resource != nullptr &&
            is_better_sticky_depth(depth_cand.info, depth_cand.score, g_sticky_depth_candidate)) {
            replace_candidate_locked(g_sticky_depth_candidate, depth_cand.info, depth_cand.score, depth_cand.source);
            g_sticky_depth_candidate.frame = depth_cand.frame;
        } else if (g_sticky_depth_candidate.present &&
                   !is_recent_candidate(current_frame, g_sticky_depth_candidate)) {
            release_candidate_locked(g_sticky_depth_candidate);
        }
        if (is_recent_candidate(current_frame, g_sticky_depth_candidate)) {
            sticky_depth_cand = copy_candidate_locked(g_sticky_depth_candidate);
        }
    }

    // NOTE(eye/motion): both observations are emitted as FrameResourceEye::Both with default unit
    // motion scale. Per-eye velocity routing and motion_scale population are not implemented yet
    // (the bind provider sees the shared engine targets, not per-eye splits). See plan Phase 4+.
    // Serve the freshest good depth: prefer this frame's bind, else the recent sticky. Pick the better
    // of the two (score, then larger area) so the full-res eye depth wins over an incidental low-res DSV.
    ResourceCandidate served_depth;
    auto take_best_depth = [&](ResourceCandidate& option) {
        if (!option.present || option.info.resource == nullptr) {
            return;
        }
        if (served_depth.present && served_depth.info.resource != nullptr &&
            !is_better_sticky_depth(option.info, option.score, served_depth)) {
            return;
        }
        if (served_depth.info.resource != nullptr) {
            served_depth.info.resource->Release();
        }
        served_depth = option;
        option = {};
    };
    // A-vs-B diagnostic (UEVR_AFW_DEPTH_TRACE): log the FRESH per-frame depth bind the engine made this
    // frame. If fresh_ptr alternates between two eye-res resources -> engine renders per-eye (A, fix in our
    // provider); if it's one resource every frame (or mostly sticky) -> ghosting fix renders one eye (B,
    // fix in its render cadence). See afw-ghosting-fix-depth-conflict.
    {
        static int s_depthtrace = 0;
        if (std::getenv("UEVR_AFW_DEPTH_TRACE") != nullptr && s_depthtrace++ < 240) {
            log_info("[depthtrace] f=%u freshBind=%s fresh_ptr=0x%p %ux%u sticky_ptr=0x%p",
                     current_frame,
                     (depth_cand.present && depth_cand.info.resource != nullptr) ? "Y" : "N",
                     depth_cand.present ? (void*)depth_cand.info.resource : nullptr,
                     depth_cand.present ? depth_cand.info.width : 0u,
                     depth_cand.present ? depth_cand.info.height : 0u,
                     sticky_depth_cand.present ? (void*)sticky_depth_cand.info.resource : nullptr);
        }
    }
    take_best_depth(depth_cand);
    // PER-EYE DEPTH FIX (UEVR_AFW_PER_EYE_DEPTH): under the ghosting fix the engine binds TWO eye-res depth
    // buffers (one per eye, confirmed via UEVR_AFW_DEPTH_TRACE) but our eye-unaware sticky pins ONE and serves
    // it to both eyes -> per-eye depth collapses (verified: ON eye0==eye1 0.0008 vs the 0.004 real disparity).
    // When THIS frame has a fresh eye-res bind (the actually-rendered eye's depth), serve IT and skip the
    // sticky, so the combine's depthDesc[nEye] gets the right eye's depth (the same way the working ghosting-OFF
    // path already does). The sticky is still consulted on frames with no fresh eye-res bind. Gated; default off.
    {
        static const bool s_per_eye_depth = []() {
            const char* e = std::getenv("UEVR_AFW_PER_EYE_DEPTH");
            return e != nullptr && (e[0] == '1' || e[0] == 't' || e[0] == 'T' || e[0] == 'y' || e[0] == 'Y');
        }();
        const bool have_fresh_eyeres = served_depth.present && served_depth.info.resource != nullptr &&
                                       served_depth.info.width >= 1024u && served_depth.info.height >= 1024u;
        if (!s_per_eye_depth || !have_fresh_eyeres) {
            take_best_depth(sticky_depth_cand);
        }
    }
    if (depth_cand.info.resource != nullptr) depth_cand.info.resource->Release();
    if (sticky_depth_cand.info.resource != nullptr) sticky_depth_cand.info.resource->Release();

    if (served_depth.present && served_depth.info.resource != nullptr) {
        char key[96];
        std::snprintf(key, sizeof(key), "depthcand:%p:%ux%u:%u", (void*)served_depth.info.resource,
                      served_depth.info.width, served_depth.info.height, (unsigned)served_depth.info.format);
        if (log_once_key(key)) {
            log_info("d3d12 depth candidate ptr=0x%p %ux%u fmt=%u source=%s score=%d",
                     (void*)served_depth.info.resource, served_depth.info.width, served_depth.info.height,
                     (unsigned)served_depth.info.format, served_depth.source ? served_depth.source : "?",
                     served_depth.score);
        }
        if ((current_frame % 120u) == 0u) {
            log_info("D3D12Bind: serving depth %ux%u fmt=%u (sticky age %u frames)",
                     served_depth.info.width, served_depth.info.height, (unsigned)served_depth.info.format,
                     current_frame >= served_depth.frame ? current_frame - served_depth.frame : 0u);
        }

        ObservedFrameResource o;
        o.kind = FrameResourceKind::Depth;
        o.provider = FrameResourceProvider::D3D12Bind;
        o.eye = FrameResourceEye::Both;
        o.resource = served_depth.info.resource;
        o.format = served_depth.info.format;
        o.width = served_depth.info.width;
        o.height = served_depth.info.height;
        o.expected_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        // Stamp the served frame as current so a sticky depth (a few frames old, within the sticky
        // window) reads as Valid instead of Stale — the combine needs a depth EVERY frame to run.
        o.render_frame = current_frame;
        o.debug_name = "d3d12_depth_dsv";
        tracker.observe_resource(o);
        tracker.bump("d3d12_depth_candidate");
        served_depth.info.resource->Release();
    } else {
        tracker.observe_missing(FrameResourceKind::Depth, FrameResourceProvider::D3D12Bind,
                                "no depth-stencil view bound this frame");
    }

    ResourceCandidate selected_cand;
    bool selected_from_scene_resource = false;
    auto take_best = [&](ResourceCandidate& option, bool from_scene_resource) {
        if (!option.present || option.info.resource == nullptr) {
            return;
        }
        if (selected_cand.present && selected_cand.info.resource != nullptr &&
            !is_better_velocity_candidate(option.info, option.score, selected_cand)) {
            return;
        }
        if (selected_cand.info.resource != nullptr) {
            selected_cand.info.resource->Release();
        }
        selected_cand = option;
        option = {};
        selected_from_scene_resource = from_scene_resource;
    };

    take_best(cand, false);
    take_best(sticky_cand, false);
    take_best(scene_cand, true);

    if (cand.info.resource != nullptr) cand.info.resource->Release();
    if (sticky_cand.info.resource != nullptr) sticky_cand.info.resource->Release();
    if (scene_cand.info.resource != nullptr) scene_cand.info.resource->Release();

    if (selected_from_scene_resource) {
        const uint32_t original_frame = selected_cand.frame;
        const bool holding_scene_velocity = env_bool("UEVR_FRAME_RESOURCES_HOLD_SCENE_VELOCITY", false);
        if (holding_scene_velocity) {
            selected_cand.frame = current_frame;
        }
        if ((current_frame % 120u) == 0u) {
            log_info("D3D12Bind: using %s scene velocity ptr=0x%p %ux%u originally observed %u frames ago",
                     holding_scene_velocity ? "held" : "recent", (void*)selected_cand.info.resource,
                     selected_cand.info.width, selected_cand.info.height,
                     current_frame >= original_frame ? current_frame - original_frame : 0u);
        }
    }

    ObservedFrameResource snapshot_velocity{};
    bool have_snapshot_velocity = false;
    if (velocity_snapshot_enabled()) {
        std::scoped_lock lock(g_state_mutex);
        const bool snapshot_fresh =
            g_velocity_snapshot.resource != nullptr &&
            current_frame >= g_velocity_snapshot.frame &&
            (current_frame - g_velocity_snapshot.frame) <= kStickyVelocityMaxAgeFrames;
        const uint64_t snapshot_area =
            static_cast<uint64_t>(g_velocity_snapshot.width) * static_cast<uint64_t>(g_velocity_snapshot.height);
        const uint64_t selected_area = resource_area(selected_cand.info);
        const bool snapshot_matches_selection =
            !selected_cand.present || selected_area == 0 || snapshot_area >= selected_area ||
            (g_velocity_snapshot.width == selected_cand.info.width &&
             g_velocity_snapshot.height == selected_cand.info.height &&
             g_velocity_snapshot.format == selected_cand.info.format);

        if (snapshot_fresh && snapshot_matches_selection) {
            g_velocity_snapshot.resource->AddRef();
            snapshot_velocity.kind = FrameResourceKind::Velocity;
            snapshot_velocity.provider = FrameResourceProvider::D3D12Bind;
            snapshot_velocity.eye = FrameResourceEye::Both;
            snapshot_velocity.resource = g_velocity_snapshot.resource;
            snapshot_velocity.format = g_velocity_snapshot.format;
            snapshot_velocity.width = g_velocity_snapshot.width;
            snapshot_velocity.height = g_velocity_snapshot.height;
            snapshot_velocity.expected_state = D3D12_RESOURCE_STATE_COPY_DEST;
            snapshot_velocity.render_frame = g_velocity_snapshot.frame;
            snapshot_velocity.debug_name = "d3d12_velocity_snapshot";
            have_snapshot_velocity = true;
        }
    }

    if (have_snapshot_velocity) {
        if ((current_frame % 120u) == 0u) {
            log_info("D3D12Bind: exposing stable velocity snapshot ptr=0x%p %ux%u fmt=%u age=%u",
                     static_cast<void*>(snapshot_velocity.resource), snapshot_velocity.width,
                     snapshot_velocity.height, static_cast<unsigned>(snapshot_velocity.format),
                     current_frame >= snapshot_velocity.render_frame ? current_frame - snapshot_velocity.render_frame : 0u);
        }
        tracker.observe_resource(snapshot_velocity);
        tracker.bump("d3d12_velocity_snapshot");
        snapshot_velocity.resource->Release();
        if (selected_cand.info.resource != nullptr) {
            selected_cand.info.resource->Release();
        }
        return;
    }

    if (!selected_cand.present || selected_cand.info.resource == nullptr) {
        tracker.observe_missing(FrameResourceKind::Velocity, FrameResourceProvider::D3D12Bind,
                                "no velocity-shaped RTV bound this frame");
        if (selected_cand.info.resource != nullptr) {
            selected_cand.info.resource->Release();
        }
        return;
    }

    // Confidence model. A canonical RG16 target qualifies on its own (it is an unambiguous velocity
    // signal). A weak RGBA16 bind scores 0 for format, so it can never clear the bar on size alone —
    // that keeps HDR scene colour / bloom targets from being mislabelled as velocity. It is still
    // logged below so a title that genuinely uses an RGBA16 velocity buffer is discoverable.
    int confidence = 0;
    const bool canonical = is_canonical_velocity_format(selected_cand.info.format);
    if (canonical) {
        confidence += 3;
    }
    const FrameResourceView depth = tracker.get_latest(FrameResourceKind::Depth);
    const bool allow_low_res_velocity = env_bool("UEVR_FRAME_RESOURCES_ALLOW_LOW_RES_VELOCITY", false);
    // If this is the canonical high-water velocity resource, expose it even when the current depth
    // is larger. That preserves the intended bootstrap behavior: use 633x534 if it is the largest
    // velocity ever seen, but reject it after a larger scene velocity appears.
    const bool high_water_velocity = selected_from_scene_resource && canonical;
    const bool scene_sized_velocity = high_water_velocity || velocity_matches_scene_size(selected_cand.info, depth);
    if (depth.validity == FrameResourceValidity::Valid && depth.width > 0) {
        if (selected_cand.info.width == depth.width && selected_cand.info.height == depth.height) {
            confidence += 2; // scene-sized
        } else if (allow_low_res_velocity && selected_cand.info.width * 2 == depth.width &&
                   selected_cand.info.height * 2 == depth.height) {
            confidence += 2; // canonical half-res velocity; sample UVs still line up
        } else if (selected_cand.info.width >= depth.width && selected_cand.info.height >= depth.height) {
            confidence += 2; // full-resolution guard-band velocity is larger than the depth buffer.
        }
    } else if (selected_cand.info.width >= 256 && selected_cand.info.height >= 256) {
        confidence += 1; // plausibly scene-sized
    }

    char key[96];
    std::snprintf(key, sizeof(key), "velcand:%p:%ux%u:%u", (void*)selected_cand.info.resource,
                  selected_cand.info.width, selected_cand.info.height, (unsigned)selected_cand.info.format);
    if (log_once_key(key)) {
        log_info("d3d12 velocity candidate ptr=0x%p %ux%u fmt=%u source=%s canonical=%d scene_sized=%d allow_low_res=%d conf=%d",
                 (void*)selected_cand.info.resource, selected_cand.info.width, selected_cand.info.height,
                 (unsigned)selected_cand.info.format, selected_cand.source ? selected_cand.source : "?",
                 canonical ? 1 : 0, scene_sized_velocity ? 1 : 0, allow_low_res_velocity ? 1 : 0, confidence);
    }

    // Expose as a candidate only when confidence clears the bar. If
    // UEVR_FRAME_RESOURCES_SNAPSHOT_VELOCITY=1 and a fresh copy exists, the stable snapshot path
    // above wins so late consumers do not sample a reused transient RDG allocation.
    if (confidence >= 3 && scene_sized_velocity) {
        ObservedFrameResource o;
        o.kind = FrameResourceKind::Velocity;
        o.provider = FrameResourceProvider::D3D12Bind;
        o.eye = FrameResourceEye::Both;
        o.resource = selected_cand.info.resource;
        o.format = selected_cand.info.format;
        o.width = selected_cand.info.width;
        o.height = selected_cand.info.height;
        o.expected_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        o.render_frame = selected_cand.frame;
        o.debug_name = "d3d12_velocity_candidate";
        tracker.observe_resource(o);
        tracker.bump("d3d12_velocity_candidate");
    } else {
        tracker.observe_missing(FrameResourceKind::Velocity, FrameResourceProvider::D3D12Bind,
                                scene_sized_velocity ? "velocity-shaped bind below confidence threshold"
                                                     : "velocity is lower resolution than scene target");
    }

    selected_cand.info.resource->Release();
}

} // namespace afw_fr
