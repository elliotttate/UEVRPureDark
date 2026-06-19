#include <d3dcompiler.h>

#include <openvr.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <utility/String.hpp>
#include <utility/ScopeGuard.hpp>
#include <utility/Logging.hpp>

#include "Framework.hpp"
#include "../VR.hpp"

#include <../../directxtk12-src/Inc/ResourceUploadBatch.h>
#include <../../directxtk12-src/Inc/RenderTargetState.h>

#include "shaders/Compiled/alpha_luminance_sprite_ps_SpritePixelShader.inc"
#include "shaders/Compiled/alpha_luminance_sprite_ps_SpriteVertexShader.inc"

#include "d3d12/DirectXTK.hpp"

#include "shaders/Compiled/ue_velocity_combine_cs_VelocityCombineCS.inc"
#include "shaders/Compiled/afw_debug_visualize_cs_DebugVisualizeCS.inc"

#include "AFWFrameResourcesBridge.hpp"
#include "D3D12Component.hpp"

//#define AFR_DEPTH_TEMP_DISABLED

constexpr auto ENGINE_SRC_DEPTH = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr auto ENGINE_SRC_COLOR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

namespace {
bool afw_env_bool(const char* name, bool default_value) {
    if (const char* value = std::getenv(name); value != nullptr) {
        if (_stricmp(value, "0") == 0 || _stricmp(value, "false") == 0 || _stricmp(value, "off") == 0 ||
            _stricmp(value, "no") == 0) {
            return false;
        }

        if (_stricmp(value, "1") == 0 || _stricmp(value, "true") == 0 || _stricmp(value, "on") == 0 ||
            _stricmp(value, "yes") == 0) {
            return true;
        }
    }

    return default_value;
}

bool afw_d3d12_allow_swapchain_backbuffer_fallback() {
    return afw_env_bool("UEVR_AFW_ALLOW_SWAPCHAIN_BACKBUFFER", false);
}

float afw_env_float(const char* name, float default_value) {
    if (const char* value = std::getenv(name); value != nullptr && *value != '\0') {
        char* end{};
        const float parsed = std::strtof(value, &end);
        if (end != value) {
            return parsed;
        }
    }

    return default_value;
}

int afw_env_int(const char* name, int default_value) {
    if (const char* value = std::getenv(name); value != nullptr && *value != '\0') {
        char* end{};
        const long parsed = std::strtol(value, &end, 10);
        if (end != value) {
            return static_cast<int>(parsed);
        }
    }

    return default_value;
}

enum class AfwBridgeVelocityMode {
    Combined,
    Raw,
    Off,
};

AfwBridgeVelocityMode afw_bridge_velocity_mode() {
    if (const char* value = std::getenv("UEVR_AFW_VELOCITY_MODE"); value != nullptr && *value != '\0') {
        if (_stricmp(value, "combined") == 0 || _stricmp(value, "combine") == 0 ||
            _stricmp(value, "dense") == 0 || _stricmp(value, "dlss") == 0) {
            return AfwBridgeVelocityMode::Combined;
        }

        if (_stricmp(value, "raw") == 0 || _stricmp(value, "copy_raw") == 0 ||
            _stricmp(value, "passthrough") == 0 || _stricmp(value, "pass_through") == 0) {
            return AfwBridgeVelocityMode::Raw;
        }

        if (_stricmp(value, "off") == 0 || _stricmp(value, "disabled") == 0 ||
            _stricmp(value, "disable") == 0 || _stricmp(value, "none") == 0) {
            return AfwBridgeVelocityMode::Off;
        }

        SPDLOG_WARNING_EVERY_N_SEC(1, "[VR] Unknown UEVR_AFW_VELOCITY_MODE='{}'; using combined", value);
    }

    // Backward compatibility for the original boolean switch: disabling the combine means
    // "copy the raw helper velocity" instead of disabling velocity entirely.
    if (!afw_env_bool("UEVR_AFW_COMBINE_UE_VELOCITY", true)) {
        return AfwBridgeVelocityMode::Raw;
    }

    return AfwBridgeVelocityMode::Combined;
}

const char* afw_bridge_velocity_mode_name(AfwBridgeVelocityMode mode) {
    switch (mode) {
    case AfwBridgeVelocityMode::Combined:
        return "combined";
    case AfwBridgeVelocityMode::Raw:
        return "raw";
    case AfwBridgeVelocityMode::Off:
        return "off";
    default:
        return "unknown";
    }
}

const char* afw_motion_vectors_type_name(MVType type) {
    switch (type) {
    case Normal:
        return "Normal";
    case FromOtherEye:
        return "FromOtherEye";
    case ObjectOnly:
        return "ObjectOnly";
    default:
        return "Unknown";
    }
}

MVType afw_select_motion_vectors_type(FrameWarpMode mode, bool bridge_velocity_used, bool bridge_velocity_combined, bool fix_dlss) {
    if (const char* value = std::getenv("UEVR_AFW_MOTION_VECTORS_TYPE"); value != nullptr && *value != '\0') {
        if (_stricmp(value, "normal") == 0) {
            return Normal;
        }
        if (_stricmp(value, "from_other_eye") == 0 || _stricmp(value, "fromothereye") == 0 ||
            _stricmp(value, "other_eye") == 0 || _stricmp(value, "othereye") == 0) {
            return FromOtherEye;
        }
        if (_stricmp(value, "object_only") == 0 || _stricmp(value, "objectonly") == 0) {
            return ObjectOnly;
        }
    }

    if (bridge_velocity_combined) {
        return Normal;
    }

    if (fix_dlss) {
        return Normal;
    }

    // PDAFW's Normal mode is same-eye temporal. With DLSS off, Alternate/Combined AFW synthesize
    // the other eye, so keep the original cross-eye motion-vector interpretation even when the
    // helper plugin supplies a velocity texture. Same-eye PreviousFrameWarping is the bridge case
    // where Normal is the matching contract.
    if (bridge_velocity_used && mode == PreviousFrameWarping) {
        return Normal;
    }

    return FromOtherEye;
}

bool afw_bridge_velocity_format_allowed(uint32_t format) {
    // Mirror the plugin's canonical-velocity policy (afw_frame_resources is_canonical_velocity_format):
    // two-channel 16-bit RG targets (PF_G16R16) plus RGBA16 *UNORM* (PF_A16B16G16R16, the UE5.5/5.6
    // velocity-depth-encoded variant — what AFW2 uses). RGBA16 FLOAT is HDR colour, never velocity.
    switch (static_cast<DXGI_FORMAT>(format)) {
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return true;
    default:
        return false;
    }
}

// Maps the engine's depth format to a PDAFW-created copy format. Keep combined depth+stencil
// families typeless so CopyTextureRegion stays within the same format compatibility group.
DXGI_FORMAT afw_bridge_depth_srv_format(DXGI_FORMAT engine_format) {
    switch (engine_format) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32G8X24_TYPELESS;
    default:
        return engine_format; // already concrete + sampleable (engine handed us a resolved format)
    }
}

bool afw_bridge_needs_recreate(const TextureDesc& dst, const UEVR_FrameResourceView& src, DXGI_FORMAT format) {
    if (dst.pTexture == nullptr) {
        return true;
    }

    const auto dst_desc = dst.pTexture->GetDesc();
    return dst_desc.Width != src.width || dst_desc.Height != src.height || dst_desc.Format != format;
}

bool afw_bridge_ensure_texture(D3D12RendererAPI* renderer, TextureDesc& dst, const UEVR_FrameResourceView& src,
                               DXGI_FORMAT format, D3D12_RESOURCE_STATES state, bool create_uav, const char* label) {
    if (renderer == nullptr || src.d3d12_resource == nullptr || src.width == 0 || src.height == 0) {
        return false;
    }

    if (!afw_bridge_needs_recreate(dst, src, format)) {
        return true;
    }

    // The warp only ever samples these copies (SRV); a UAV on a depth/typeless format is invalid, so
    // depth passes create_uav=false. Velocity is sampled too, so it also opts out.
    if (!renderer->CreateTexture(static_cast<int>(src.width), static_cast<int>(src.height), format, state, dst,
                                 create_uav)) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] AFW frame resource bridge failed to create {} texture {}x{} fmt={}",
                                 label, src.width, src.height, src.format);
        return false;
    }
    if (dst.pTexture != nullptr && label != nullptr) {
        const std::wstring name = std::wstring{L"UEVR AFW "} + utility::widen(label);
        dst.pTexture->SetName(name.c_str());
    }

    return true;
}

struct AfwBridgeEyeResource {
    UEVR_FrameResourceView view{};
    D3D12_BOX source_box{};
    bool split_side_by_side{false};
};

AfwBridgeEyeResource afw_bridge_eye_resource(const UEVR_FrameResourceView& src, uint32_t eye_width,
                                             uint32_t eye_height, EyeIndex eye, bool allow_stereo_source_split) {
    AfwBridgeEyeResource result{};
    result.view = src;
    result.source_box = D3D12_BOX{0, 0, 0, src.width, src.height, 1};

    const bool matches_output_double_wide =
        eye_width != 0 && eye_height != 0 && src.width == eye_width * 2u && src.height == eye_height;
    const bool looks_like_stereo_source =
        allow_stereo_source_split && src.width >= 2u && (src.width % 2u) == 0u && src.height != 0;

    if (matches_output_double_wide || looks_like_stereo_source) {
        const uint32_t source_eye_width = matches_output_double_wide ? eye_width : (src.width / 2u);
        const uint32_t left = eye == EyeRight ? source_eye_width : 0u;
        result.view.width = source_eye_width;
        result.view.height = src.height;
        result.source_box.left = left;
        result.source_box.right = left + source_eye_width;
        result.split_side_by_side = true;
    }

    return result;
}

TextureDesc afw_bridge_source_desc(const UEVR_FrameResourceView& src, ImageType type) {
    TextureDesc desc{};
    desc.type = type;
    desc.pTexture = static_cast<ID3D12Resource*>(src.d3d12_resource);
    desc.initialState = static_cast<D3D12_RESOURCE_STATES>(src.expected_state);
    if (desc.initialState == D3D12_RESOURCE_STATE_COMMON) {
        desc.initialState = type == Depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    return desc;
}

void afw_transition_resource(ID3D12GraphicsCommandList* command_list, ID3D12Resource* resource,
                             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

void afw_copy_texture_region(ID3D12GraphicsCommandList* command_list, TextureDesc& dst, TextureDesc& src,
                             const D3D12_BOX& source_box) {
    if (command_list == nullptr || dst.pTexture == nullptr || src.pTexture == nullptr) {
        return;
    }

    auto src_state = src.initialState;
    if (src_state == D3D12_RESOURCE_STATE_COMMON) {
        src_state = src.type == Depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    auto dst_state = dst.initialState;
    if (dst_state == D3D12_RESOURCE_STATE_COMMON) {
        dst_state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    }

    afw_transition_resource(command_list, src.pTexture, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    afw_transition_resource(command_list, dst.pTexture, dst_state, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_TEXTURE_COPY_LOCATION dst_location{};
    dst_location.pResource = dst.pTexture;
    dst_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_location.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src_location{};
    src_location.pResource = src.pTexture;
    src_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_location.SubresourceIndex = 0;

    command_list->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, &source_box);

    afw_transition_resource(command_list, dst.pTexture, D3D12_RESOURCE_STATE_COPY_DEST, dst_state);
    afw_transition_resource(command_list, src.pTexture, D3D12_RESOURCE_STATE_COPY_SOURCE, src_state);
}

void afw_transition_resource(ID3D12GraphicsCommandList* command_list, ID3D12Resource* resource,
                             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (command_list == nullptr || resource == nullptr || before == after) {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    command_list->ResourceBarrier(1, &barrier);
}

void afw_uav_barrier(ID3D12GraphicsCommandList* command_list, ID3D12Resource* resource) {
    if (command_list == nullptr || resource == nullptr) {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.UAV.pResource = resource;
    command_list->ResourceBarrier(1, &barrier);
}

// One-shot debug readback of an RG16F (or any) GPU texture to disk so the combined velocity
// can be false-colored offline and compared to the engine's DLSSCombinedVelocity reference.
// Self-contained: own allocator/list/fence on the swapchain queue. Gated + dump-once by the caller.
void afw_dump_texture_to_disk(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* tex,
                              D3D12_RESOURCE_STATES before_state, const char* path) {
    if (device == nullptr || queue == nullptr || tex == nullptr) {
        return;
    }
    const auto desc = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT num_rows = 0; UINT64 row_size = 0, total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &num_rows, &row_size, &total);

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&readback)))) {
        return;
    }
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) return;

    auto barrier = [&](D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER br{}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource = tex; br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br.Transition.StateBefore = a; br.Transition.StateAfter = b; list->ResourceBarrier(1, &br);
    };
    barrier(before_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier(D3D12_RESOURCE_STATE_COPY_SOURCE, before_state);
    list->Close();

    ID3D12CommandList* lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, lists);
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1) { fence->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, 5000); }
    CloseHandle(ev);

    void* mapped = nullptr; D3D12_RANGE rr{0, static_cast<SIZE_T>(total)};
    if (SUCCEEDED(readback->Map(0, &rr, &mapped)) && mapped != nullptr) {
        std::ofstream f(path, std::ios::binary);
        if (f) {
            uint32_t hdr[4] = { static_cast<uint32_t>(desc.Width), static_cast<uint32_t>(desc.Height),
                                fp.Footprint.RowPitch, static_cast<uint32_t>(desc.Format) };
            f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
            f.write(reinterpret_cast<const char*>(mapped), static_cast<std::streamsize>(total));
        }
        D3D12_RANGE wr{0, 0}; readback->Unmap(0, &wr);
    }
}

struct AFWVelocityCombineConstants {
    float clip_to_prev_clip[16]{};
    uint32_t output_size[2]{};
    float inv_output_size[2]{};
    uint32_t velocity_size[2]{};
    uint32_t depth_size[2]{};
    uint32_t velocity_origin[2]{};
    uint32_t velocity_extent[2]{};
    uint32_t depth_origin[2]{};
    uint32_t depth_extent[2]{};
    uint32_t force_reconstruct{};
    uint32_t pad[3]{};
};

static_assert(sizeof(AFWVelocityCombineConstants) == 36 * sizeof(uint32_t));

struct AFWDebugVizConstants {
    // 0 velocity(pixel RG hue), 1 depth(R), 2 decoded source velocity hue,
    // 3 decoded source velocity X, 4 decoded source velocity Y,
    // 5 decoded source velocity magnitude, 6 source velocity validity.
    uint32_t mode{};
    float scale{1.0f};
    uint32_t input_size[2]{};
    uint32_t output_size[2]{};
    uint32_t pad[2]{};
};
static_assert(sizeof(AFWDebugVizConstants) == 8 * sizeof(uint32_t));
} // namespace

namespace vrmod {
vr::EVRCompositorError D3D12Component::on_frame(VR* vr) {
    if (m_force_reset || m_last_afr_state != vr->is_using_afr()) {
        if (!setup()) {
            SPDLOG_ERROR_EVERY_N_SEC(1, "[D3D12 VR] Could not set up, trying again next frame");
            m_force_reset = true;
            return vr::VRCompositorError_None;
        }

        m_last_afr_state = vr->is_using_afr();
    }

    auto& hook = g_framework->get_d3d12_hook();

    hook->set_next_present_interval(0); // disable vsync for vr
    
    // get device
    auto device = hook->get_device();

    // get command queue
    auto command_queue = hook->get_command_queue();

    // get swapchain
    auto swapchain = hook->get_swap_chain();

    // get back buffer
    ComPtr<ID3D12Resource> backbuffer{};
    ComPtr<ID3D12Resource> real_backbuffer{};
    auto ue4_texture = VR::get()->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

    if (ue4_texture != nullptr) {
        backbuffer = (ID3D12Resource*)ue4_texture->get_native_resource();
    }

    if (FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&real_backbuffer)))) {
        spdlog::error("[VR] Failed to get real back buffer.");
        return vr::VRCompositorError_None;
    }

    if (vr->is_extreme_compatibility_mode_enabled()) {
        backbuffer = real_backbuffer;
    }

    if (backbuffer == nullptr && (vr->is_stereo_emulation_enabled() || afw_d3d12_allow_swapchain_backbuffer_fallback())) {
        backbuffer = real_backbuffer;
        if (vr->is_stereo_emulation_enabled()) {
            SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Stereo emulation: using DXGI swapchain backbuffer for D3D12 frame");
        } else {
            SPDLOG_WARNING_EVERY_N_SEC(1, "[VR] Fake stereo render target unavailable; using DXGI swapchain backbuffer for D3D12 diagnostic frame");
        }
    }

    if (backbuffer == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] Failed to get back buffer.");
        return vr::VRCompositorError_None;
    }

    const auto ui_should_invert_alpha = vr->get_overlay_component().should_invert_ui_alpha();

    // Update the UI overlay.
    auto runtime = vr->get_runtime();

    const auto is_same_frame = m_last_rendered_frame > 0 && m_last_rendered_frame == vr->m_render_frame_count;
    m_last_rendered_frame = vr->m_render_frame_count;

    const auto is_actually_afr = vr->is_using_afr();
    const auto is_afr = !is_same_frame && vr->is_using_afr();
    const auto is_left_eye_frame = is_afr && vr->m_render_frame_count % 2 == vr->m_left_eye_interval;
    const auto is_right_eye_frame = !is_afr || vr->m_render_frame_count % 2 == vr->m_right_eye_interval;

    // Sometimes this can happen if pipeline execution does not go exactly as planned
    // so we need to resynchronized or begin the frame again.
    if (runtime->ready()) {
        runtime->fix_frame();
    }

    const auto& ffsr = VR::get()->m_fake_stereo_hook;
    const auto ui_target = ffsr->get_render_target_manager()->get_ui_target();

    const auto frame_count = vr->m_render_frame_count;

    if (m_game_tex.texture.Get() == nullptr && backbuffer.Get() == real_backbuffer.Get()) {
        spdlog::info("[VR] Setting up game texture as copy of backbuffer");
        
        ComPtr<ID3D12Resource> backbuffer_copy{};
        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

        auto desc = backbuffer->GetDesc();
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        m_backbuffer_copy.reset();

        ComPtr<ID3D12Resource> backbuffer_copy2{};

        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&backbuffer_copy2)))) {
            spdlog::error("[VR] Failed to create backbuffer copy.");
            return vr::VRCompositorError_None;
        }

        if (!m_backbuffer_copy.setup(device, backbuffer_copy2.Get(), std::nullopt, std::nullopt, L"Backbuffer Copy")) {
            spdlog::error("[VR] Failed to fully setup backbuffer copy.");
            m_backbuffer_copy.reset();
        }

        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // UE backbuffer is not VR compatible, so we need to copy it to a new texture with this one.

        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&backbuffer_copy)))) {
            spdlog::error("[VR] Failed to create backbuffer copy.");
            return vr::VRCompositorError_None;
        }

        if (!m_game_tex.setup(device, backbuffer_copy.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"Game Texture")) {
            spdlog::error("[VR] Failed to fully setup game texture.");
            m_game_tex.reset();
        } else {
            for (auto& commands : m_game_tex_commands) {
                commands.setup(L"Game Texture Commands");
            }
        }
    } else if (backbuffer.Get() != real_backbuffer.Get() && m_game_tex.texture.Get() != backbuffer.Get()) {
        spdlog::info("[VR] Setting up game texture as reference to original");

        if (!m_game_tex.setup(device, backbuffer.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"Game Texture")) {
            spdlog::error("[VR] Failed to fully setup game texture.");
            m_game_tex.reset();
        }
    }

    if (vr->is_native_stereo_fix_enabled()) {
        const auto scene_capture = ffsr->get_render_target_manager()->get_scene_capture_render_target();
        const auto scene_capture_rt = scene_capture != nullptr ? (ID3D12Resource*)scene_capture->get_native_resource() : nullptr;

        if (scene_capture_rt != nullptr && m_scene_capture_tex.texture.Get() != scene_capture_rt) {
            spdlog::info("[VR] Setting up scene capture texture as reference to original");

            if (!m_scene_capture_tex.setup(device, scene_capture_rt, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"Scene Capture Texture")) {
                spdlog::error("[VR] Failed to fully setup scene capture texture.");
                m_scene_capture_tex.reset();
            }
        }

        if (scene_capture_rt == nullptr && m_scene_capture_tex.texture.Get() != nullptr) {
            spdlog::info("[VR] Resetting scene capture texture");

            m_scene_capture_tex.reset();
        }
    } else {
        m_scene_capture_tex.reset();
    }

    // We need to render the scene capture texture to the right side of the double wide texture
    auto pre_render = [&](d3d12::CommandContext& commands, ID3D12Resource* render_target) {
        if (render_target == nullptr) {
            return;
        }

        // Also the same for right, even though it's not a double wide texture
        D3D12_BOX left_src_box{
            .left = 0,
            .top = 0,
            .front = 0,
            .right = m_backbuffer_size[0] / 2,
            .bottom = m_backbuffer_size[1],
            .back = 1
        };

        commands.copy_region_stereo(
            m_game_tex.texture.Get(), m_scene_capture_tex.texture.Get(), render_target,
            &left_src_box, &left_src_box,
            0, 0, 0, m_backbuffer_size[0] / 2, 0, 0,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RENDER_TARGET
        );

        if (vr->afw_debug_view != 0) {
            if (vr->d3d12Renderer == nullptr) {
                SPDLOG_WARNING_EVERY_N_SEC(2, "[VR] OpenXR frame-resource debug view requested but PDAFW D3D12 renderer is unavailable");
                return;
            }

            TextureDesc debug_target{};
            debug_target.pTexture = render_target;
            debug_target.initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            vr->d3d12Renderer->SetupTextureDesc(debug_target);
            render_frame_resource_debug_view(vr, commands.cmd_list.Get(), vr->afw_debug_view,
                                             debug_target, nullptr);
        }
    };

    // For copying the real backbuffer if we need to
    if (m_game_tex.texture.Get() != nullptr && backbuffer == real_backbuffer) {
        const auto idx = swapchain->GetCurrentBackBufferIndex() % m_game_tex_commands.size();
        auto& command_ctx = m_game_tex_commands[idx];
        if (command_ctx.cmd_list != nullptr) {
            command_ctx.wait(INFINITE);
            float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
            command_ctx.clear_rtv(m_game_tex, (float*)&clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);
            command_ctx.copy(real_backbuffer.Get(), m_backbuffer_copy.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            //m_game_tex_commands[idx].copy(backbuffer.Get(), m_game_tex.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, ENGINE_SRC_COLOR);
            d3d12::render_srv_to_rtv(
                m_game_batch.get(),
                command_ctx.cmd_list.Get(),
                m_backbuffer_copy,
                m_game_tex,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_RENDER_TARGET
            );
            command_ctx.execute();
        }

        backbuffer = m_game_tex.texture;
    }

    if (ui_target != nullptr) {
        if (m_game_ui_tex.texture.Get() != ui_target->get_native_resource()) {
            if (!m_game_ui_tex.setup(device, 
                (ID3D12Resource*)ui_target->get_native_resource(), 
                DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
                L"Game UI Texture"))
            {
                spdlog::error("[VR] Failed to fully setup game UI texture.");
                m_game_ui_tex.reset();
            }
        }

        // Recreate UI texture if needed
        if (!vr->is_extreme_compatibility_mode_enabled()) {
            const auto native = (ID3D12Resource*)ui_target->get_native_resource();
            const auto is_same_native = native == m_last_checked_native;
            m_last_checked_native = native;

            if (native != nullptr && !is_same_native) {
                const auto desc = native->GetDesc();

                if (runtime->is_openxr()) {
                    if (auto it = vr->m_openxr->swapchains.find((uint32_t)runtimes::OpenXR::SwapchainIndex::UI);
                        it != vr->m_openxr->swapchains.end()) 
                    {
                        const auto& uisc = it->second;
                        if (desc.Width != uisc.width ||
                            desc.Height != uisc.height)
                        {
                            SPDLOG_INFO_EVERY_N_SEC(1, "[OpenXR] UI size changed, recreating [{}x{}]->[{}x{}]", desc.Width, desc.Height, uisc.width, uisc.height);
                            ffsr->set_should_recreate_textures(true);
                        }
                    }
                } else if (m_game_ui_tex.texture != nullptr) {
                    const auto ui_desc = m_game_ui_tex.texture->GetDesc();

                    if (desc.Width != ui_desc.Width || desc.Height != ui_desc.Height) {
                        SPDLOG_INFO_EVERY_N_SEC(1, "[OpenVR] UI size changed, recreating texture [{}x{}]->[{}x{}]", desc.Width, desc.Height, ui_desc.Width, ui_desc.Height);
                        ffsr->set_should_recreate_textures(true);
                    }
                }
            } else if (native == nullptr) {
                spdlog::error("[VR] Recreating UI texture because native resource is null");
                ffsr->set_should_recreate_textures(true);
            }
        }
    } else {
        m_game_ui_tex.reset(); // Probably fixes non-resident errors.
    }

    const float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const auto is_2d_screen = vr->is_using_2d_screen();

    auto draw_2d_view = [&](d3d12::CommandContext& commands, ID3D12Resource* render_target) {
        if (ui_should_invert_alpha && m_game_ui_tex.texture.Get() != nullptr && m_game_ui_tex.srv_heap != nullptr) {
            d3d12::render_srv_to_rtv(m_ui_batch_alpha_invert.get(), commands.cmd_list.Get(), m_game_ui_tex, m_game_ui_tex, std::nullopt, ENGINE_SRC_COLOR, ENGINE_SRC_COLOR);
        }

        draw_spectator_view(commands.cmd_list.Get(), is_right_eye_frame);

        if (is_2d_screen && m_game_tex.texture.Get() != nullptr && m_game_tex.srv_heap != nullptr) {
            // Clear previous frame
            for (auto& screen : m_2d_screen_tex) {
                commands.clear_rtv(screen, clear_color, ENGINE_SRC_COLOR);
            }

            // Render left side to left screen tex
            d3d12::render_srv_to_rtv(
                m_game_batch.get(),
                commands.cmd_list.Get(),
                m_game_tex,
                m_2d_screen_tex[0],
                RECT{0, 0, (LONG)((float)m_backbuffer_size[0] / 2.0f), (LONG)m_backbuffer_size[1]},
                ENGINE_SRC_COLOR,
                ENGINE_SRC_COLOR
            );

            if (m_game_ui_tex.texture.Get() != nullptr && m_game_ui_tex.srv_heap != nullptr) {
                d3d12::render_srv_to_rtv(
                    m_game_batch.get(),
                    commands.cmd_list.Get(),
                    m_game_ui_tex,
                    m_2d_screen_tex[0],
                    ENGINE_SRC_COLOR,
                    ENGINE_SRC_COLOR
                );
            }

            if (!is_afr) {
                // Render right side to right screen tex
                if (m_scene_capture_tex.texture.Get() != nullptr) {
                    d3d12::render_srv_to_rtv(
                        m_game_batch.get(),
                        commands.cmd_list.Get(),
                        m_scene_capture_tex,
                        m_2d_screen_tex[1],
                        ENGINE_SRC_COLOR,
                        ENGINE_SRC_COLOR
                    );
                } else {
                    d3d12::render_srv_to_rtv(
                        m_game_batch.get(),
                        commands.cmd_list.Get(),
                        m_game_tex,
                        m_2d_screen_tex[1],
                        RECT{(LONG)((float)m_backbuffer_size[0] / 2.0f), 0, (LONG)((float)m_backbuffer_size[0]), (LONG)m_backbuffer_size[1]},
                        ENGINE_SRC_COLOR,
                        ENGINE_SRC_COLOR
                    );
                }

                if (m_game_ui_tex.texture.Get() != nullptr && m_game_ui_tex.srv_heap != nullptr) {
                    d3d12::render_srv_to_rtv(
                        m_game_batch.get(),
                        commands.cmd_list.Get(),
                        m_game_ui_tex,
                        m_2d_screen_tex[1],
                        ENGINE_SRC_COLOR,
                        ENGINE_SRC_COLOR
                    );
                }
            }

            // Clear the RT so the entire background is black when submitting to the compositor
            commands.clear_rtv(m_game_tex, (float*)&clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);

            if (m_scene_capture_tex.texture.Get() != nullptr) {
                commands.clear_rtv(m_scene_capture_tex, (float*)&clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
        }
    };

    // Draws the spectator view
    auto clear_rt = [&](d3d12::CommandContext& commands) {
        if (m_game_ui_tex.texture.Get() == nullptr) {
            return;
        }

        const float ui_clear_color[] = { 0.0f, 0.0f, 0.0f, ui_should_invert_alpha ? 1.0f : 0.0f };
        commands.clear_rtv(m_game_ui_tex, (float*)&ui_clear_color, ENGINE_SRC_COLOR);
    };

    if (runtime->is_openvr() && m_openvr.ui_tex.texture.Get() != nullptr) {
        m_openvr.ui_tex.commands.wait(INFINITE);

        draw_2d_view(m_openvr.ui_tex.commands, nullptr);

        if (is_right_eye_frame) {
            if (is_2d_screen) {
                m_openvr.ui_tex.commands.copy(m_2d_screen_tex[0].texture.Get(), m_openvr.ui_tex.texture.Get(), ENGINE_SRC_COLOR);
            } else if (ui_target != nullptr) {
                m_openvr.ui_tex.commands.copy((ID3D12Resource*)ui_target->get_native_resource(), m_openvr.ui_tex.texture.Get(), ENGINE_SRC_COLOR);
            }
        } else if (is_2d_screen) {
            m_openvr.ui_tex.commands.copy(m_2d_screen_tex[0].texture.Get(), m_openvr.ui_tex.texture.Get(), ENGINE_SRC_COLOR);
        }

        clear_rt(m_openvr.ui_tex.commands);
        m_openvr.ui_tex.commands.execute();
    } else if (runtime->is_openxr() && runtime->ready() && vr->m_openxr->frame_began) {
        if (is_right_eye_frame) {
            if (is_2d_screen) {
                if (is_afr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, m_2d_screen_tex[0].texture.Get(), draw_2d_view, clear_rt, ENGINE_SRC_COLOR);
                } else {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, m_2d_screen_tex[0].texture.Get(), draw_2d_view, std::nullopt, ENGINE_SRC_COLOR);
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, m_2d_screen_tex[1].texture.Get(), std::nullopt, clear_rt, ENGINE_SRC_COLOR);
                }
            } else if (ui_target != nullptr) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, (ID3D12Resource*)ui_target->get_native_resource(), draw_2d_view, clear_rt, ENGINE_SRC_COLOR);
            }

            auto fw_rt = g_framework->get_rendertarget_d3d12();

            if (fw_rt && g_framework->is_drawing_anything()) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI, g_framework->get_rendertarget_d3d12().Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        } else if (is_2d_screen) {
            m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, m_2d_screen_tex[0].texture.Get(), draw_2d_view, clear_rt, ENGINE_SRC_COLOR);
        } else if (m_game_ui_tex.commands.ready()) {
            m_game_ui_tex.commands.wait(INFINITE);
            draw_2d_view(m_game_ui_tex.commands, nullptr);
            clear_rt(m_game_ui_tex.commands);
            m_game_ui_tex.commands.execute();
        }
    }

    /*else if (m_game_tex.texture.Get() != nullptr) {
        m_game_tex.commands.wait(INFINITE);
        draw_spectator_view(m_game_tex.commands.cmd_list.Get(), is_right_eye_frame);
        m_game_tex.commands.execute();
    }*/

    ComPtr<ID3D12Resource> scene_depth_tex{};

    if (vr->is_depth_enabled() && runtime->is_depth_allowed()) {
        auto& rt_pool = vr->get_render_target_pool_hook();
        scene_depth_tex = rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ");

        if (scene_depth_tex != nullptr) {
            const auto desc = scene_depth_tex->GetDesc();

            if (runtime->is_openxr()) {
                if (vr->m_openxr->needs_depth_resize(desc.Width, desc.Height) || m_openxr.made_depth_with_null_defaults) {
                    spdlog::info("[OpenXR] Depth size changed, recreating swapchains [{}x{}]", desc.Width, desc.Height);
                    m_openxr.create_swapchains(); // recreate swapchains to match the new depth size
                }
            }
        }

    #ifdef AFR_DEPTH_TEMP_DISABLED
        if (is_actually_afr) {
            scene_depth_tex.Reset();
        }
    #endif
    }

    // #############################
    // #Frame Warp Module Start
    // #############################

    bool is_using_afw = vr->is_using_afw();
    static bool s_applied_debug_view_env = false;
    if (!s_applied_debug_view_env) {
        s_applied_debug_view_env = true;
        if (std::getenv("UEVR_AFW_DEBUG_VIEW") != nullptr) {
            vr->afw_debug_view = std::clamp(afw_env_int("UEVR_AFW_DEBUG_VIEW", vr->afw_debug_view),
                                            0, VR::afw_debug_view_count - 1);
            SPDLOG_INFO("[VR] AFW debug view initialized from UEVR_AFW_DEBUG_VIEW={} ({})",
                        vr->afw_debug_view, VR::afw_debug_view_name(vr->afw_debug_view));
        }
    }

    const auto bb_desc = backbuffer->GetDesc();
    const auto eye_width = static_cast<uint32_t>(bb_desc.Width / 2);
    const auto eye_height = static_cast<uint32_t>(bb_desc.Height);

    if (!vr->rawDepthTex) {
        auto& rt_pool = vr->get_render_target_pool_hook();
        scene_depth_tex = rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ");
        if (scene_depth_tex)
            vr->rawDepthTex = scene_depth_tex.Get();
    }

    auto backbuffer_index = swapchain->GetCurrentBackBufferIndex();

    EyeIndex nEye = (frame_count % 2 == vr->m_left_eye_interval) ? EyeLeft : EyeRight;
    EyeIndex nEyeOther = (frame_count % 2 == vr->m_left_eye_interval) ? EyeRight : EyeLeft;
    auto eyeFrameBuffer = m_eyeFrameBuffers.eyeFrameBuffers[nEye];
    auto otherEyeFrameBuffer = m_eyeFrameBuffers.eyeFrameBuffers[nEyeOther];
    FrameWarpEvaluateParams params;

    const bool frame_warp_ready = is_using_afw && vr->d3d12Renderer != nullptr &&
                                  eyeFrameBuffer.color.pTexture != nullptr &&
                                  otherEyeFrameBuffer.color.pTexture != nullptr;
    if (is_using_afw && !frame_warp_ready) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] AFW frame buffers are not ready; skipping frame warp this frame");
        force_reset();
    }

    if (frame_warp_ready) {
    static TextureDesc backbufferDesc[6];
    if (backbufferDesc[backbuffer_index].pTexture != backbuffer.Get()) {
        backbufferDesc[backbuffer_index].pTexture = backbuffer.Get();
        backbufferDesc[backbuffer_index].initialState = D3D12_RESOURCE_STATE_PRESENT;
        vr->d3d12Renderer->SetupTextureDesc(backbufferDesc[backbuffer_index]);
    }
    static TextureDesc realBackbufferDesc[6];
    if (realBackbufferDesc[backbuffer_index].pTexture != real_backbuffer.Get()) {
        realBackbufferDesc[backbuffer_index].pTexture = real_backbuffer.Get();
        realBackbufferDesc[backbuffer_index].initialState = D3D12_RESOURCE_STATE_PRESENT;
        vr->d3d12Renderer->SetupTextureDesc(realBackbufferDesc[backbuffer_index]);
    }

    ID3D12Resource* uevr_depth_source_for_afw = nullptr;
    if (vr->rawDepthTex) {
        auto desc = vr->rawDepthTex->GetDesc();
        uevr_depth_source_for_afw = vr->rawDepthTex;
        vr->rawDepthTex = NULL;
        for (int i = 0; i < 2; i++) {
            if (vr->depthDesc[i].pTexture == NULL || vr->depthDesc[i].pTexture->GetDesc().Width != desc.Width ||
                vr->depthDesc[i].pTexture->GetDesc().Height != desc.Height) {
                vr->d3d12Renderer->CreateTexture(
                    desc.Width, desc.Height, desc.Format, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, vr->depthDesc[i], true);
            }
        }
    }
    if (vr->rawMotionVectorsTex) {
        auto desc = vr->rawMotionVectorsTex->GetDesc();
        vr->rawMotionVectorsTex = NULL;
        for (int i = 0; i < 2; i++) {
            if (vr->motionVectorsDesc[i].pTexture == NULL || vr->motionVectorsDesc[i].pTexture->GetDesc().Width != desc.Width ||
                vr->motionVectorsDesc[i].pTexture->GetDesc().Height != desc.Height) {
                vr->d3d12Renderer->CreateTexture(desc.Width, desc.Height, DXGI_FORMAT_R16G16_FLOAT,
                    D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, vr->motionVectorsDesc[i], true);
            }
        }
    }

    auto cmdList = vr->d3d12Renderer->BeginCommandList(backbuffer_index);
    bool afw_uevr_depth_used = false;
    bool afw_bridge_depth_used = false;
    bool afw_bridge_velocity_used = false;
    bool afw_bridge_velocity_combined = false;
    const bool afw_bridge_enabled = uevr_afw_bridge::enabled();
    const bool afw_bridge_legacy_fallback = !afw_bridge_enabled || uevr_afw_bridge::legacy_fallback();
    const auto afw_bridge_velocity_mode_value = afw_bridge_velocity_mode();
    const bool afw_bridge_use_velocity =
        afw_bridge_enabled && uevr_afw_bridge::use_velocity() &&
        afw_bridge_velocity_mode_value != AfwBridgeVelocityMode::Off;
    const auto afw_eye_color_desc = eyeFrameBuffer.color.pTexture->GetDesc();
    const uint32_t afw_eye_width = static_cast<uint32_t>(afw_eye_color_desc.Width);
    const uint32_t afw_eye_height = afw_eye_color_desc.Height;

    if (is_using_afw && uevr_depth_source_for_afw != nullptr && vr->depthDesc[nEye].pTexture != nullptr) {
        TextureDesc src{};
        src.type = Depth;
        src.pTexture = uevr_depth_source_for_afw;
        src.initialState = ENGINE_SRC_DEPTH;
        vr->d3d12Renderer->Copy(cmdList, vr->depthDesc[nEye], src);
        afw_uevr_depth_used = true;

        const auto src_desc = uevr_depth_source_for_afw->GetDesc();
        SPDLOG_INFO_EVERY_N_SEC(1, "[VR] AFW copied UEVR SceneDepthZ depth ptr={} {}x{} fmt={}",
                                static_cast<void*>(uevr_depth_source_for_afw), src_desc.Width, src_desc.Height,
                                static_cast<uint32_t>(src_desc.Format));
    }

    if (is_using_afw && afw_bridge_enabled) {
        if (!uevr_afw_bridge::available()) {
            SPDLOG_WARNING_EVERY_N_SEC(1, "[VR] AFW frame resource bridge is enabled but unavailable: {}",
                                       uevr_afw_bridge::describe_state());
        } else {
            UEVR_FrameResourceView depth_view{};
            if (uevr_afw_bridge::get_latest_depth(&depth_view) && depth_view.d3d12_resource != nullptr) {
                const bool depth_is_authoritative_rtpool =
                    depth_view.provider == UEVR_FRAME_RESOURCE_PROVIDER_RENDER_TARGET_POOL;
                const bool should_copy_bridge_depth = !afw_uevr_depth_used || depth_is_authoritative_rtpool;
                if (!should_copy_bridge_depth) {
                    SPDLOG_INFO_EVERY_N_SEC(2, "[VR] AFW bridge depth provider={} skipped; using UEVR SceneDepthZ depth",
                                            depth_view.provider);
                } else {
                    const auto depth_format = afw_bridge_depth_srv_format(static_cast<DXGI_FORMAT>(depth_view.format));
                    const auto depth_eye_resource =
                        afw_bridge_eye_resource(depth_view, afw_eye_width, afw_eye_height, nEye,
                                                vr->is_stereo_emulation_enabled());
                    if (afw_bridge_ensure_texture(vr->d3d12Renderer, vr->depthDesc[nEye], depth_eye_resource.view, depth_format,
                                                  D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, false, "depth")) {
                        auto src = afw_bridge_source_desc(depth_view, Depth);
                        if (depth_eye_resource.split_side_by_side) {
                            afw_copy_texture_region(cmdList, vr->depthDesc[nEye], src, depth_eye_resource.source_box);
                        } else {
                            vr->d3d12Renderer->Copy(cmdList, vr->depthDesc[nEye], src);
                        }
                        afw_bridge_depth_used = true;
                        SPDLOG_INFO_EVERY_N_SEC(1, "[VR] AFW bridge copied depth provider={} ptr={} {}x{} fmt={}->{} state=0x{:x} out={}x{} split={}",
                                                depth_view.provider, depth_view.d3d12_resource, depth_view.width,
                                                depth_view.height, depth_view.format, static_cast<uint32_t>(depth_format),
                                                depth_view.expected_state,
                                                vr->depthDesc[nEye].pTexture != nullptr ? vr->depthDesc[nEye].pTexture->GetDesc().Width : 0,
                                                vr->depthDesc[nEye].pTexture != nullptr ? vr->depthDesc[nEye].pTexture->GetDesc().Height : 0,
                                                depth_eye_resource.split_side_by_side ? 1 : 0);
                    }
                }
            } else {
                SPDLOG_WARNING_EVERY_N_SEC(1, "[VR] AFW bridge depth unavailable: provider={} validity={} reason={}",
                                           depth_view.provider, depth_view.validity,
                                           depth_view.reason != nullptr ? depth_view.reason : "");
            }

            if (afw_bridge_use_velocity) {
                UEVR_FrameResourceView velocity_view{};
                if (uevr_afw_bridge::get_latest_velocity(&velocity_view) && velocity_view.d3d12_resource != nullptr) {
                    if (afw_bridge_velocity_format_allowed(velocity_view.format)) {
                        const bool raw_velocity_requested =
                            afw_bridge_velocity_mode_value == AfwBridgeVelocityMode::Raw;
                        const bool combine_ue_velocity =
                            afw_bridge_velocity_mode_value == AfwBridgeVelocityMode::Combined &&
                            vr->depthDesc[nEye].pTexture != nullptr &&
                            eyeFrameBuffer.color.pTexture != nullptr;
                        const auto velocity_format = static_cast<DXGI_FORMAT>(velocity_view.format);
                        bool velocity_ready = false;

                        if (combine_ue_velocity) {
                            auto output_view = velocity_view;
                            const auto color_desc = eyeFrameBuffer.color.pTexture->GetDesc();
                            output_view.width = static_cast<uint32_t>(color_desc.Width);
                            output_view.height = color_desc.Height;
                            if (afw_bridge_ensure_texture(vr->d3d12Renderer, vr->motionVectorsDesc[nEye], output_view,
                                                          DXGI_FORMAT_R16G16_FLOAT,
                                                          D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, true,
                                                          "combined velocity")) {
                                velocity_ready = combine_ue_velocity_for_afw(
                                    vr, cmdList, velocity_view, vr->depthDesc[nEye], vr->motionVectorsDesc[nEye], nEye,
                                    vr->afw_force_reconstruct);
                                afw_bridge_velocity_combined = velocity_ready;
                                if (!velocity_ready) {
                                    SPDLOG_WARNING_EVERY_N_SEC(1, "[VR] AFW bridge failed to combine UE velocity; raw velocity fallback={}",
                                                               afw_env_bool("UEVR_AFW_RAW_VELOCITY_FALLBACK", true) ? 1 : 0);
                                }
                            }
                        }

                        if (!velocity_ready &&
                            (raw_velocity_requested || afw_env_bool("UEVR_AFW_RAW_VELOCITY_FALLBACK", true))) {
                            const auto velocity_eye_resource =
                                afw_bridge_eye_resource(velocity_view, afw_eye_width, afw_eye_height, nEye,
                                                        vr->is_stereo_emulation_enabled());
                            if (afw_bridge_ensure_texture(vr->d3d12Renderer, vr->motionVectorsDesc[nEye],
                                                          velocity_eye_resource.view,
                                                          velocity_format, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, false,
                                                          "raw velocity")) {
                                auto src = afw_bridge_source_desc(velocity_view, Image);
                                if (velocity_eye_resource.split_side_by_side) {
                                    afw_copy_texture_region(cmdList, vr->motionVectorsDesc[nEye], src,
                                                            velocity_eye_resource.source_box);
                                } else {
                                    vr->d3d12Renderer->Copy(cmdList, vr->motionVectorsDesc[nEye], src);
                                }
                                velocity_ready = true;
                            }
                        }

                        if (velocity_ready) {
                            afw_bridge_velocity_used = true;
                            vr->mvScale[0] = afw_env_float("UEVR_AFW_VELOCITY_SCALE_X", 1.0f);
                            vr->mvScale[1] = afw_env_float("UEVR_AFW_VELOCITY_SCALE_Y", 1.0f);
                            SPDLOG_INFO_EVERY_N_SEC(1, "[VR] AFW bridge {} UE velocity mode={} provider={} ptr={} src={}x{} fmt={} state=0x{:x} out={}x{} mvScale={},{}",
                                                    afw_bridge_velocity_combined ? "combined" : "copied raw",
                                                    afw_bridge_velocity_mode_name(afw_bridge_velocity_mode_value),
                                                    velocity_view.provider, velocity_view.d3d12_resource,
                                                    velocity_view.width, velocity_view.height, velocity_view.format,
                                                    velocity_view.expected_state,
                                                    vr->motionVectorsDesc[nEye].pTexture != nullptr ? vr->motionVectorsDesc[nEye].pTexture->GetDesc().Width : 0,
                                                    vr->motionVectorsDesc[nEye].pTexture != nullptr ? vr->motionVectorsDesc[nEye].pTexture->GetDesc().Height : 0,
                                                    vr->mvScale[0], vr->mvScale[1]);
                        }
                    } else {
                        SPDLOG_WARNING_EVERY_N_SEC(1, "[VR] AFW bridge rejected velocity fmt={} provider={} ptr={}; not a UE motion-vector format",
                                                   velocity_view.format, velocity_view.provider,
                                                   velocity_view.d3d12_resource);
                    }
                } else {
                    SPDLOG_WARNING_EVERY_N_SEC(1, "[VR] AFW bridge velocity unavailable: provider={} validity={} reason={}",
                                               velocity_view.provider, velocity_view.validity,
                                               velocity_view.reason != nullptr ? velocity_view.reason : "");
                }
            } else if (afw_bridge_enabled && uevr_afw_bridge::use_velocity() &&
                       afw_bridge_velocity_mode_value == AfwBridgeVelocityMode::Off) {
                SPDLOG_INFO_EVERY_N_SEC(5, "[VR] AFW bridge velocity mode=off; not supplying motion vectors");
            } else {
                SPDLOG_INFO_EVERY_N_SEC(5, "[VR] AFW bridge velocity consumption disabled; set UEVR_AFW_FRAME_RESOURCES_VELOCITY=1 to opt in");
            }
        }
    }

    // if (is_using_afw && is_right_eye_frame) {
    //     FLOAT red[4] = {1, 0, 0, 0};
    //     vr->d3d12Renderer->Clear(cmdList, backbufferDesc[backbuffer_index], red);
    // }

    const bool afw_has_depth = (afw_bridge_enabled && !afw_bridge_legacy_fallback)
        ? (afw_bridge_depth_used || afw_uevr_depth_used)
        : vr->depthDesc[nEye].pTexture != nullptr;

    if (is_using_afw && eyeFrameBuffer.color.pTexture && afw_has_depth) {
        static FrameBufferDesc s_CurrentEyeFrameBuffer{};

        D3D12_BOX src_box{.left = 0,
            .top = 0,
            .front = 0,
            .right = vr->is_extreme_compatibility_mode_enabled() ? m_backbuffer_size[0] : m_backbuffer_size[0] / 2,
            .bottom = m_backbuffer_size[1],
            .back = 1};

        const auto afw_color_dst_desc = eyeFrameBuffer.color.pTexture->GetDesc();
        const bool afw_full_source_viewport = afw_env_bool("UEVR_AFW_FULL_SOURCE_VIEWPORT", false);
        const D3D12_VIEWPORT afw_source_viewport{
            .TopLeftX = 0.0f,
            .TopLeftY = 0.0f,
            .Width = static_cast<float>(afw_color_dst_desc.Width),
            .Height = static_cast<float>(afw_color_dst_desc.Height),
            .MinDepth = 0.0f,
            .MaxDepth = 1.0f};
        const bool afw_projection_override_active =
            vr->get_horizontal_projection_override() != VR::HORIZONTAL_PROJECTION_OVERRIDE::HORIZONTAL_DEFAULT ||
            vr->get_vertical_projection_override() != VR::VERTICAL_PROJECTION_OVERRIDE::VERTICAL_DEFAULT;

        if (afw_projection_override_active) {
            SPDLOG_INFO_EVERY_N_SEC(2, "[VR] AFW guard-band source feed: src {}x{} -> PDAFW {}x{} full_viewport={} bounds L({}, {}, {}, {}) R({}, {}, {}, {})",
                                    src_box.right - src_box.left, src_box.bottom - src_box.top,
                                    afw_color_dst_desc.Width, afw_color_dst_desc.Height,
                                    afw_full_source_viewport ? 1 : 0,
                                    runtime->view_bounds[0][0], runtime->view_bounds[0][1],
                                    runtime->view_bounds[0][2], runtime->view_bounds[0][3],
                                    runtime->view_bounds[1][0], runtime->view_bounds[1][1],
                                    runtime->view_bounds[1][2], runtime->view_bounds[1][3]);
        }

        if (afw_full_source_viewport) {
            vr->d3d12Renderer->Crop(cmdList, eyeFrameBuffer.color, backbufferDesc[backbuffer_index], src_box, afw_source_viewport);
        } else {
            vr->d3d12Renderer->Crop(cmdList, eyeFrameBuffer.color, backbufferDesc[backbuffer_index], src_box);
        }

        const bool afw_prefill_warp_output = afw_env_bool("UEVR_AFW_PREFILL_WARP_OUTPUT", true);
        if (afw_prefill_warp_output && otherEyeFrameBuffer.color.pTexture != nullptr) {
            vr->d3d12Renderer->Copy(cmdList, otherEyeFrameBuffer.color, eyeFrameBuffer.color);
            SPDLOG_INFO_EVERY_N_SEC(2, "[VR] AFW prefilled warp output from native eye before EvaluateFrameWarp");
        }

        FLOAT black[4] = {0, 0, 0, 0};
        //if (vr->mDebug1) {
        //    vr->d3d12Renderer->Clear(cmdList, vr->motionVectorsDesc[nEye], black);
        //}

        s_CurrentEyeFrameBuffer.color = eyeFrameBuffer.color;
        s_CurrentEyeFrameBuffer.depth = vr->depthDesc[nEye];
        s_CurrentEyeFrameBuffer.motionVectors =
            (afw_bridge_velocity_used || afw_bridge_legacy_fallback) ? vr->motionVectorsDesc[nEye] : TextureDesc{};
        //if (vr->mDebug2) {
        //    vr->d3d12Renderer->Clear(cmdList, eyeFrameBuffer.depth, black);
        //    s_CurrentEyeFrameBuffer.depth = eyeFrameBuffer.depth;
        //}

        params.InCmdList = cmdList;
        params.InEyeFrameBuffer = &s_CurrentEyeFrameBuffer;
        params.InUIColorAlpha = NULL;
        params.IsHudlessColor = true;
        params.InMotionScale[0] = vr->mvScale[0];
        params.InMotionScale[1] = vr->mvScale[1];
        params.Mode = (FrameWarpMode)vr->m_framewarp_mode->value();
        params.MotionVectorsType =
            afw_select_motion_vectors_type(params.Mode, afw_bridge_velocity_used, afw_bridge_velocity_combined, vr->is_fix_dlss());
        params.EyeIndex = nEye;
        params.ClearBeforeWarping = vr->m_clear_before_framewarp->value();
        params.CameraData = &vr->cameraData[nEye];
        params.IgnoreMotionThreshold = vr->m_ignore_motion_threshold->value();
        params.Debug = vr->m_framewarp_debug->value();
        SPDLOG_INFO_EVERY_N_SEC(2, "[VR] AFW params mode={} mvType={} bridgeVelocity={} mvScale={},{}",
                                static_cast<int>(params.Mode),
                                afw_motion_vectors_type_name(params.MotionVectorsType),
                                afw_bridge_velocity_used ? 1 : 0, params.InMotionScale[0],
                                params.InMotionScale[1]);

        if (vr->m_enable_ui_fix->value() && vr->uiBufferDesc.pTexture) {
            params.InUIColorAlpha = &vr->uiBufferDesc;
            params.IsHudlessColor = false;
        }
        // Sharpening
        // if (vr->is_enable_sharpening() && vr->get_sharpness() > 0) {
        //    vr->d3d12Renderer->Sharpen(cmdList, eyeFrameBuffer.color, s_CurrentEyeFrameBuffer.color, vr->get_sharpness());
        //    s_CurrentEyeFrameBuffer.color = eyeFrameBuffer.color;
        //}
        EvaluateFrameWarp(params);

        // Cycle-able debug buffer visualizer (Ctrl+Shift+V): overwrite the eyes with a false-color
        // view of motion vectors / depth / raw velocity so they can be inspected directly in-headset.
        if (vr->afw_debug_view != 0) {
            // Pin to eye 0 so the view doesn't flicker as AFW alternates the natively-rendered eye.
            render_debug_view(vr, cmdList, vr->afw_debug_view, static_cast<EyeIndex>(0),
                              backbufferDesc[backbuffer_index], &otherEyeFrameBuffer.color);
        }

        D3D12_VIEWPORT vp{
            .TopLeftX = 0, .TopLeftY = 0, .Width = (float)src_box.right, .Height = (float)src_box.bottom, .MinDepth = 0, .MaxDepth = 1};
        // vr->d3d12Renderer->Blit(cmdList, backbufferDesc[backbuffer_index], otherEyeFrameBuffer.color, vp);
    } else if (is_using_afw && afw_bridge_enabled && !afw_bridge_legacy_fallback) {
        SPDLOG_WARNING_EVERY_N_SEC(1, "[VR] AFW bridge skipped EvaluateFrameWarp because bridged depth is not ready");
    }

    vr->d3d12Renderer->EndCommandList(backbuffer_index);
    }

    // #############################
    // #Frame Warp Module End
    // #############################

    // If m_frame_count is even, we're rendering the left eye.
    if (is_left_eye_frame) {
        m_submitted_left_eye = true;

        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            D3D12_BOX src_box{};
            src_box.left = 0;
            src_box.top = 0;
            src_box.bottom = m_backbuffer_size[1];
            src_box.front = 0;
            src_box.back = 1;

            if (vr->is_extreme_compatibility_mode_enabled()) {
                src_box.right = m_backbuffer_size[0];
            } else {
                src_box.right = m_backbuffer_size[0] / 2;
            }

            m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, &src_box);

            if (scene_depth_tex != nullptr) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
            }

            if (is_using_afw) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, otherEyeFrameBuffer.color.pTexture, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
                //if (scene_depth_tex != nullptr) {
                //    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE, otherEyeFrameBuffer.depth.pTexture,
                //    ENGINE_SRC_DEPTH, nullptr);
                //}
            }
        }

        // OpenVR texture
        // Copy the back buffer to the left eye texture
        if (runtime->is_openvr()) {
            if (!is_using_afw)
                m_openvr.copy_left(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);

            auto openvr = vr->get_runtime<runtimes::OpenVR>();
            const auto submit_pose = openvr->get_pose_for_submit();

            vr::D3D12TextureData_t left {
                m_openvr.get_left().texture.Get(),
                command_queue,
                0
            };
            
            vr::VRTextureWithPose_t left_eye{
                (void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto,
                submit_pose
            };
            const auto left_bounds = vr::VRTextureBounds_t{runtime->view_bounds[0][0], runtime->view_bounds[0][2],
                                                           runtime->view_bounds[0][1], runtime->view_bounds[0][3]};
            auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &left_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                return e;
            }
            if (is_using_afw) {
                vr::D3D12TextureData_t right{m_openvr.get_right().texture.Get(), command_queue, 0};

                vr::VRTextureWithPose_t right_eye{(void*)&right, vr::TextureType_DirectX12, vr::ColorSpace_Auto, submit_pose};
                const auto right_bounds = vr::VRTextureBounds_t{
                    runtime->view_bounds[1][0], runtime->view_bounds[1][2], runtime->view_bounds[1][1], runtime->view_bounds[1][3]};
                e = vr::VRCompositor()->Submit(vr::Eye_Right, &right_eye, &right_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);
                runtime->frame_synced = false;

                if (e != vr::VRCompositorError_None) {
                    spdlog::error("[VR] VRCompositor failed to submit right eye: {}", (int)e);
                    return e;
                } else {
                    vr->m_submitted = true;
                }
            }
        }
    } else {
        utility::ScopeGuard __{[&]() {
            m_submitted_left_eye = false;
        }};

        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            if (is_actually_afr && !is_afr && !m_submitted_left_eye) {
                D3D12_BOX src_box{};
                src_box.left = 0;
                src_box.top = 0;
                src_box.bottom = m_backbuffer_size[1];
                src_box.front = 0;
                src_box.back = 1;

                if (vr->is_extreme_compatibility_mode_enabled()) {
                    src_box.right = m_backbuffer_size[0];
                } else {
                    src_box.right = m_backbuffer_size[0] / 2;
                }

                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, &src_box);

                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
                }
            }

            if (is_actually_afr) {
                D3D12_BOX src_box{};

                if (!vr->is_extreme_compatibility_mode_enabled()) {
                    if (!is_afr) {
                        src_box.left = m_backbuffer_size[0] / 2;
                        src_box.right = m_backbuffer_size[0];
                        src_box.top = 0;
                        src_box.bottom = m_backbuffer_size[1];
                        src_box.front = 0;
                        src_box.back = 1;
                    } else { // Copy the left eye on AFR
                        src_box.left = 0;
                        src_box.right = m_backbuffer_size[0] / 2;
                        src_box.top = 0;
                        src_box.bottom = m_backbuffer_size[1];
                        src_box.front = 0;
                        src_box.back = 1;
                    }   
                } else {
                    src_box.left = 0;
                    src_box.right = m_backbuffer_size[0];
                    src_box.top = 0;
                    src_box.bottom = m_backbuffer_size[1];
                    src_box.front = 0;
                    src_box.back = 1;
                }

                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, &src_box);

                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
                }

                if (is_using_afw) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, otherEyeFrameBuffer.color.pTexture, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
                }
            } else {
                // Copy over the entire double wide instead
                if (m_scene_capture_tex.texture.Get() == nullptr) {
                    const auto copy_or_duplicate_backbuffer =
                        [this, vr, backbuffer](d3d12::CommandContext& commands, ID3D12Resource* render_target) {
                            if (backbuffer.Get() == nullptr || render_target == nullptr) {
                                return;
                            }

                            const auto src_desc = backbuffer->GetDesc();
                            const auto dst_desc = render_target->GetDesc();

                            const auto draw_debug_to_target = [&]() {
                                if (vr->afw_debug_view == 0) {
                                    return;
                                }

                                if (vr->d3d12Renderer == nullptr) {
                                    SPDLOG_WARNING_EVERY_N_SEC(2, "[VR] OpenXR frame-resource debug view requested but PDAFW D3D12 renderer is unavailable");
                                    return;
                                }

                                TextureDesc debug_target{};
                                debug_target.pTexture = render_target;
                                debug_target.initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
                                vr->d3d12Renderer->SetupTextureDesc(debug_target);
                                render_frame_resource_debug_view(vr, commands.cmd_list.Get(), vr->afw_debug_view,
                                                                 debug_target, nullptr);
                            };

                            if (src_desc.Width == dst_desc.Width && src_desc.Height == dst_desc.Height) {
                                commands.copy(backbuffer.Get(), render_target, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET);
                                draw_debug_to_target();
                                return;
                            }

                            const uint32_t eye_width = static_cast<uint32_t>(dst_desc.Width / 2u);
                            if (eye_width == 0 || dst_desc.Height == 0) {
                                return;
                            }

                            const uint32_t copy_width = static_cast<uint32_t>(std::min<uint64_t>(src_desc.Width, eye_width));
                            const uint32_t copy_height = std::min(src_desc.Height, dst_desc.Height);
                            if (copy_width == 0 || copy_height == 0) {
                                return;
                            }

                            D3D12_BOX src_box{};
                            src_box.left = 0;
                            src_box.top = 0;
                            src_box.front = 0;
                            src_box.right = copy_width;
                            src_box.bottom = copy_height;
                            src_box.back = 1;

                            SPDLOG_INFO_EVERY_N_SEC(2, "[VR] OpenXR stereo-emulation fallback copying {}x{} source into double-wide {}x{} eye halves",
                                                    src_desc.Width, src_desc.Height, dst_desc.Width, dst_desc.Height);
                            commands.copy_region(backbuffer.Get(), render_target, &src_box, 0, 0, 0,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET);
                            commands.copy_region(backbuffer.Get(), render_target, &src_box, eye_width, 0, 0,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET);
                            draw_debug_to_target();
                        };
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, nullptr,
                                  copy_or_duplicate_backbuffer, std::nullopt,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
                } else {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, nullptr, pre_render, std::nullopt, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
                }

                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
                }
            }
        }

        // OpenVR texture
        // Copy the back buffer to the left and right eye textures.
        if (runtime->is_openvr()) {
            auto openvr = vr->get_runtime<runtimes::OpenVR>();
            const auto submit_pose = openvr->get_pose_for_submit();

            if (!is_afr || is_using_afw) {
                if (!is_using_afw)
                    m_openvr.copy_left(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);

                vr::D3D12TextureData_t left {
                    m_openvr.get_left().texture.Get(),
                    command_queue,
                    0
                };

                vr::VRTextureWithPose_t left_eye{
                    (void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto,
                    submit_pose
                };
                const auto left_bounds = vr::VRTextureBounds_t{runtime->view_bounds[0][0], runtime->view_bounds[0][2],
                                                               runtime->view_bounds[0][1], runtime->view_bounds[0][3]};
                auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &left_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);

                if (e != vr::VRCompositorError_None) {
                    spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                    //return e; // dont return because it will just completely stop us from even getting to the right eye which could be catastrophic
                }
            }

            if (!is_using_afw) {
                if (!is_afr) {
                    if (m_scene_capture_tex.texture.Get() == nullptr) {
                        m_openvr.copy_right(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                    } else {
                        m_openvr.copy_left_to_right(m_scene_capture_tex.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                    }
                } else {
                    m_openvr.copy_left_to_right(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            }

            vr::D3D12TextureData_t right {
                m_openvr.get_right().texture.Get(),
                command_queue,
                0
            };

            vr::VRTextureWithPose_t right_eye{
                (void*)&right, vr::TextureType_DirectX12, vr::ColorSpace_Auto,
                submit_pose
            };
            const auto right_bounds = vr::VRTextureBounds_t{runtime->view_bounds[1][0], runtime->view_bounds[1][2],
                                                            runtime->view_bounds[1][1], runtime->view_bounds[1][3]};
            auto e = vr::VRCompositor()->Submit(vr::Eye_Right, &right_eye, &right_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);
            runtime->frame_synced = false;

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit right eye: {}", (int)e);
                return e;
            } else {
                vr->m_submitted = true;
            }

            ++m_openvr.texture_counter;
        }
    }

    if (is_right_eye_frame || is_using_afw) {
        if ((runtime->ready() && vr->get_synchronize_stage() == VR::SynchronizeStage::VERY_LATE) || !runtime->got_first_sync) {
            //vr->update_hmd_state();
        }
    }

    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    if (is_right_eye_frame || is_using_afw) {
        ////////////////////////////////////////////////////////////////////////////////
        // OpenXR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->is_openxr() && vr->m_openxr->ready()) {
            if (!vr->m_openxr->frame_began) {
                vr->m_openxr->begin_frame();
            }

            std::vector<XrCompositionLayerBaseHeader*> quad_layers{};

            auto& openxr_overlay = vr->get_overlay_component().get_openxr();

            if (vr->m_2d_screen_mode->value()) {
                const auto left_layer = openxr_overlay.generate_slate_layer(runtimes::OpenXR::SwapchainIndex::UI, XrEyeVisibility::XR_EYE_VISIBILITY_LEFT);
                const auto right_layer = openxr_overlay.generate_slate_layer(runtimes::OpenXR::SwapchainIndex::UI_RIGHT, XrEyeVisibility::XR_EYE_VISIBILITY_RIGHT);

                if (left_layer && m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::UI)) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&left_layer->get());
                }

                if (right_layer && m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT)) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&right_layer->get());
                }
            } else if (m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::UI)) {
                const auto slate_layer = openxr_overlay.generate_slate_layer();

                if (slate_layer) {
                    quad_layers.push_back(&slate_layer->get());
                }   
            }
            
            if (m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI)) {
                const auto framework_quad = openxr_overlay.generate_framework_ui_quad();
                if (framework_quad) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&framework_quad->get());
                }
            }

            auto result = vr->m_openxr->end_frame(quad_layers, scene_depth_tex.Get() != nullptr);

            if (result == XR_ERROR_LAYER_INVALID) {
                spdlog::info("[VR] Attempting to correct invalid layer");

                m_openxr.wait_for_all_copies();

                spdlog::info("[VR] Calling xrEndFrame again");
                result = vr->m_openxr->end_frame(quad_layers);
            }

            vr->m_openxr->needs_pose_update = true;
            vr->m_submitted = result == XR_SUCCESS;
        }

        ////////////////////////////////////////////////////////////////////////////////
        // OpenVR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->is_openvr()) {
            if (runtime->needs_pose_update) {
                vr->m_submitted = false;
                spdlog::info("[VR] Runtime needed pose update inside present (frame {})", vr->m_frame_count);
                return vr::VRCompositorError_None;
            }

            //++m_openvr.texture_counter;
        }

        // Allows the desktop window to be recorded.
        /*if (vr->m_desktop_fix->value()) {
            if (runtime->ready() && m_prev_backbuffer != backbuffer && m_prev_backbuffer != nullptr) {
                m_generic_commands[frame_count % 3].wait(INFINITE);
                m_generic_commands[frame_count % 3].copy(m_prev_backbuffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
                m_generic_commands[frame_count % 3].execute();
            }
        }*/
    }

    m_prev_backbuffer = backbuffer;

    return e;
}

std::unique_ptr<DirectX::DX12::SpriteBatch> D3D12Component::setup_sprite_batch_pso(
    DXGI_FORMAT output_format, 
    std::span<const uint8_t> ps, 
    std::span<const uint8_t> vs, 
    std::optional<DirectX::SpriteBatchPipelineStateDescription> pd) 
{
    spdlog::info("[D3D12] Setting up sprite batch PSO");

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    auto swapchain = hook->get_swap_chain();

    DirectX::ResourceUploadBatch upload{ device };
    upload.Begin();

    if (!pd) {
        pd = DirectX::SpriteBatchPipelineStateDescription{DirectX::RenderTargetState{output_format, DXGI_FORMAT_UNKNOWN}};
    }

    if (ps.size() > 0) {
        pd->customPixelShader = D3D12_SHADER_BYTECODE{ps.data(), ps.size()};
    }

    if (vs.size() > 0) {
        pd->customVertexShader = D3D12_SHADER_BYTECODE{vs.data(), vs.size()};
    }

    auto batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, *pd);

    auto result = upload.End(command_queue);
    result.wait();

    spdlog::info("[D3D12] Sprite batch PSO setup complete");

    return batch;
}

bool D3D12Component::setup_velocity_combine_pipeline(ID3D12Device* device) {
    if (device == nullptr) {
        return false;
    }

    if (m_velocity_combine_root_signature != nullptr && m_velocity_combine_pso != nullptr) {
        return true;
    }

    D3D12_DESCRIPTOR_RANGE velocity_srv_range{};
    velocity_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    velocity_srv_range.NumDescriptors = 1;
    velocity_srv_range.BaseShaderRegister = 0;
    velocity_srv_range.RegisterSpace = 0;
    velocity_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE depth_srv_range{};
    depth_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    depth_srv_range.NumDescriptors = 1;
    depth_srv_range.BaseShaderRegister = 1;
    depth_srv_range.RegisterSpace = 0;
    depth_srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE output_uav_range{};
    output_uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    output_uav_range.NumDescriptors = 1;
    output_uav_range.BaseShaderRegister = 0;
    output_uav_range.RegisterSpace = 0;
    output_uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER root_parameters[4]{};
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = &velocity_srv_range;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[1].DescriptorTable.pDescriptorRanges = &depth_srv_range;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[2].DescriptorTable.pDescriptorRanges = &output_uav_range;
    root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    root_parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[3].Constants.ShaderRegister = 0;
    root_parameters[3].Constants.RegisterSpace = 0;
    root_parameters[3].Constants.Num32BitValues = sizeof(AFWVelocityCombineConstants) / sizeof(uint32_t);
    root_parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC root_signature_desc{};
    root_signature_desc.NumParameters = sizeof(root_parameters) / sizeof(root_parameters[0]);
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature_blob{};
    ComPtr<ID3DBlob> error_blob{};
    if (FAILED(D3D12SerializeRootSignature(
            &root_signature_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature_blob, &error_blob))) {
        const char* error = error_blob != nullptr ? static_cast<const char*>(error_blob->GetBufferPointer()) : "";
        SPDLOG_ERROR("[VR] Failed to serialize AFW UE velocity combine root signature: {}", error);
        return false;
    }

    if (FAILED(device->CreateRootSignature(
            0, signature_blob->GetBufferPointer(), signature_blob->GetBufferSize(),
            IID_PPV_ARGS(&m_velocity_combine_root_signature)))) {
        SPDLOG_ERROR("[VR] Failed to create AFW UE velocity combine root signature");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_velocity_combine_root_signature.Get();
    pso_desc.CS = D3D12_SHADER_BYTECODE{
        ue_velocity_combine_cs_VelocityCombineCS,
        sizeof(ue_velocity_combine_cs_VelocityCombineCS)};

    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_velocity_combine_pso)))) {
        SPDLOG_ERROR("[VR] Failed to create AFW UE velocity combine PSO");
        m_velocity_combine_root_signature.Reset();
        return false;
    }

    spdlog::info("[VR] AFW UE velocity combine pipeline setup complete");
    return true;
}

bool D3D12Component::combine_ue_velocity_for_afw(
    VR* vr,
    ID3D12GraphicsCommandList* command_list,
    const UEVR_FrameResourceView& velocity_view,
    TextureDesc& depth_desc,
    TextureDesc& output_desc,
    EyeIndex eye,
    bool force_reconstruct) {
    if (vr == nullptr || vr->d3d12Renderer == nullptr || command_list == nullptr ||
        velocity_view.d3d12_resource == nullptr || depth_desc.pTexture == nullptr ||
        output_desc.pTexture == nullptr) {
        return false;
    }

    auto device = g_framework->get_d3d12_hook()->get_device();
    if (!setup_velocity_combine_pipeline(device)) {
        return false;
    }

    // Debug: one-shot readback of the (previous frame's) combined velocity for offline false-coloring.
    if (afw_env_bool("UEVR_AFW_DUMP_COMBINED_VELOCITY", false)) {
        static int s_combine_calls = 0;
        static bool s_dumped = false;
        ++s_combine_calls;
        if (!s_dumped && s_combine_calls >= 120 && output_desc.pTexture != nullptr) {
            afw_dump_texture_to_disk(device, g_framework->get_d3d12_hook()->get_command_queue(),
                                     output_desc.pTexture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                                     "E:\\Github\\UEVRPureDark\\afw_work\\combined_velocity.bin");
            s_dumped = true;
            SPDLOG_INFO("[VR] AFW dumped combined velocity to afw_work/combined_velocity.bin ({}x{})",
                        static_cast<uint32_t>(output_desc.pTexture->GetDesc().Width),
                        output_desc.pTexture->GetDesc().Height);
        }
    }

    auto& raw_velocity_desc = m_raw_velocity_desc[eye];
    if (raw_velocity_desc.pTexture != velocity_view.d3d12_resource ||
        raw_velocity_desc.initialState != static_cast<D3D12_RESOURCE_STATES>(velocity_view.expected_state) ||
        raw_velocity_desc.shaderResourceViewHandle.ptr == 0) {
        raw_velocity_desc = afw_bridge_source_desc(velocity_view, Image);
        vr->d3d12Renderer->SetupTextureDesc(raw_velocity_desc);
    }

    // Diagnostic: capture the bridged raw velocity INLINE on this command list (where its content is
    // valid) into a stable texture, then read it back. Reveals whether the combine is decoding real
    // velocity or a transient/aliased buffer. Gated + dump-once. Stable tex stays in COPY_DEST.
    if (afw_env_bool("UEVR_AFW_DUMP_RAW_VELOCITY", false) && raw_velocity_desc.pTexture != nullptr) {
        const auto rv_desc = raw_velocity_desc.pTexture->GetDesc();
        if (m_raw_velocity_stable.pTexture == nullptr ||
            m_raw_velocity_stable.pTexture->GetDesc().Width != rv_desc.Width ||
            m_raw_velocity_stable.pTexture->GetDesc().Height != rv_desc.Height) {
            vr->d3d12Renderer->CreateTexture(static_cast<int>(rv_desc.Width), rv_desc.Height, rv_desc.Format,
                                             D3D12_RESOURCE_STATE_COPY_DEST, m_raw_velocity_stable, false);
        }
        if (m_raw_velocity_stable.pTexture != nullptr) {
            // Inline copy every frame so the stable texture always holds the latest VALID velocity.
            auto src_state = raw_velocity_desc.initialState;
            if (src_state == D3D12_RESOURCE_STATE_COMMON) src_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            afw_transition_resource(command_list, raw_velocity_desc.pTexture, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            command_list->CopyResource(m_raw_velocity_stable.pTexture, raw_velocity_desc.pTexture);
            afw_transition_resource(command_list, raw_velocity_desc.pTexture, D3D12_RESOURCE_STATE_COPY_SOURCE, src_state);
            // Dump on request (Ctrl+Shift+D). The copy above is on this (not-yet-submitted) command list,
            // so dump the stable tex which holds the PREVIOUS frame's already-executed copy — a valid
            // motion frame if the camera is moving when requested.
            if (vr->afw_dump_raw_velocity_request) {
                afw_dump_texture_to_disk(device, g_framework->get_d3d12_hook()->get_command_queue(),
                                         m_raw_velocity_stable.pTexture, D3D12_RESOURCE_STATE_COPY_DEST,
                                         "E:\\Github\\UEVRPureDark\\afw_work\\raw_velocity.bin");
                vr->afw_dump_raw_velocity_request = false;
                SPDLOG_INFO("[VR] AFW dumped RAW velocity to afw_work/raw_velocity.bin ({}x{} fmt={})",
                            static_cast<uint32_t>(rv_desc.Width), rv_desc.Height,
                            static_cast<uint32_t>(rv_desc.Format));
            }
        }
    }

    if (output_desc.unorderedAccessViewHandle.ptr == 0 || output_desc.uavPos < 0) {
        const int uav_pos = vr->d3d12Renderer->CreateUAV(output_desc.pTexture);
        if (uav_pos < 0) {
            SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] AFW UE velocity combine failed to create output UAV");
            return false;
        }
        output_desc.uavPos = uav_pos;
        output_desc.unorderedAccessViewHandle = vr->d3d12Renderer->GetGPUDescriptorHandle(uav_pos);
    }

    if (raw_velocity_desc.shaderResourceViewHandle.ptr == 0 ||
        depth_desc.shaderResourceViewHandle.ptr == 0 ||
        output_desc.unorderedAccessViewHandle.ptr == 0) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] AFW UE velocity combine missing descriptors velocity={} depth={} output={}",
                                 raw_velocity_desc.shaderResourceViewHandle.ptr,
                                 depth_desc.shaderResourceViewHandle.ptr,
                                 output_desc.unorderedAccessViewHandle.ptr);
        return false;
    }

    const auto output_resource_desc = output_desc.pTexture->GetDesc();
    const auto depth_resource_desc = depth_desc.pTexture->GetDesc();

    const auto& camera_data = vr->cameraData[eye];
    glm::mat4 clip_to_prev_clip{};

    if (afw_env_bool("UEVR_AFW_COMBINE_USE_AFW_TARGET", false)) {
        clip_to_prev_clip =
            camera_data.destViewToClipMatrix *
            camera_data.destWorldToViewMatrix *
            camera_data.srcViewToWorldMatrix *
            camera_data.srcClipToViewMatrix;
    } else {
        // The engine history slot (render_view_matrix[eye][1]) is empty in the AFW path, which
        // collapsed ClipToPrevClip to identity (zero camera motion). Cache the previous frame's
        // world->view and view->clip per eye ourselves so the temporal reprojection is real.
        static glm::mat4 s_prev_w2v[2]{};
        static glm::mat4 s_prev_v2c[2]{};
        static bool s_prev_valid[2]{false, false};
        const int ei = (eye == static_cast<EyeIndex>(1)) ? 1 : 0;

        glm::mat4 prev_world_to_view = s_prev_valid[ei] ? s_prev_w2v[ei] : camera_data.srcWorldToViewMatrix;
        glm::mat4 prev_view_to_clip = s_prev_valid[ei] ? s_prev_v2c[ei] : camera_data.srcViewToClipMatrix;

        clip_to_prev_clip =
            prev_view_to_clip *
            prev_world_to_view *
            camera_data.srcViewToWorldMatrix *
            camera_data.srcClipToViewMatrix;

        s_prev_w2v[ei] = camera_data.srcWorldToViewMatrix;
        s_prev_v2c[ei] = camera_data.srcViewToClipMatrix;
        s_prev_valid[ei] = true;
    }

    // UE matrices vs glm/HLSL column-major can disagree on handedness; allow a live transpose
    // toggle (Ctrl+Shift+T) to confirm the correct convention without rebuilding.
    if (vr->afw_combine_transpose) {
        clip_to_prev_clip = glm::transpose(clip_to_prev_clip);
    }

    AFWVelocityCombineConstants constants{};
    std::memcpy(constants.clip_to_prev_clip, &clip_to_prev_clip[0][0], sizeof(constants.clip_to_prev_clip));
    constants.output_size[0] = static_cast<uint32_t>(output_resource_desc.Width);
    constants.output_size[1] = output_resource_desc.Height;
    constants.inv_output_size[0] = constants.output_size[0] > 0 ? 1.0f / static_cast<float>(constants.output_size[0]) : 0.0f;
    constants.inv_output_size[1] = constants.output_size[1] > 0 ? 1.0f / static_cast<float>(constants.output_size[1]) : 0.0f;
    constants.velocity_size[0] = velocity_view.width;
    constants.velocity_size[1] = velocity_view.height;
    constants.depth_size[0] = static_cast<uint32_t>(depth_resource_desc.Width);
    constants.depth_size[1] = depth_resource_desc.Height;
    constants.velocity_extent[0] = constants.velocity_size[0];
    constants.velocity_extent[1] = constants.velocity_size[1];
    constants.depth_extent[0] = constants.depth_size[0];
    constants.depth_extent[1] = constants.depth_size[1];
    constants.force_reconstruct = force_reconstruct ? 1u : 0u;

    if (constants.output_size[0] == 0 || constants.output_size[1] == 0 ||
        constants.velocity_size[0] == 0 || constants.velocity_size[1] == 0 ||
        constants.depth_size[0] == 0 || constants.depth_size[1] == 0) {
        return false;
    }

    auto configure_stereo_region = [vr, eye](const uint32_t size[2], uint32_t origin[2], uint32_t extent[2],
                                             const uint32_t output_size[2], const char* label) {
        if (size[0] == output_size[0] * 2u && size[1] == output_size[1]) {
            origin[0] = eye == EyeRight ? output_size[0] : 0u;
            extent[0] = output_size[0];
            extent[1] = output_size[1];
            return;
        }

        const bool looks_like_stereo_source =
            vr != nullptr && vr->is_stereo_emulation_enabled() &&
            size[0] >= 2u && (size[0] % 2u) == 0u &&
            size[1] > 0u && size[0] >= size[1] * 2u;
        if (looks_like_stereo_source) {
            const uint32_t source_eye_width = size[0] / 2u;
            origin[0] = eye == EyeRight ? source_eye_width : 0u;
            extent[0] = source_eye_width;
            extent[1] = size[1];
            SPDLOG_INFO_EVERY_N_SEC(2, "[VR] AFW UE velocity combine sampling {} side-by-side source {}x{} as eye region x={} w={} h={} -> output {}x{}",
                                    label, size[0], size[1], origin[0], extent[0], extent[1],
                                    output_size[0], output_size[1]);
        }
    };

    configure_stereo_region(constants.velocity_size, constants.velocity_origin, constants.velocity_extent,
                            constants.output_size, "velocity");
    configure_stereo_region(constants.depth_size, constants.depth_origin, constants.depth_extent,
                            constants.output_size, "depth");

    ID3D12DescriptorHeap* heaps[] = { vr->d3d12Renderer->GetViewHeap() };
    command_list->SetDescriptorHeaps(1, heaps);
    command_list->SetComputeRootSignature(m_velocity_combine_root_signature.Get());
    command_list->SetPipelineState(m_velocity_combine_pso.Get());
    command_list->SetComputeRootDescriptorTable(0, raw_velocity_desc.shaderResourceViewHandle);
    command_list->SetComputeRootDescriptorTable(1, depth_desc.shaderResourceViewHandle);
    command_list->SetComputeRootDescriptorTable(2, output_desc.unorderedAccessViewHandle);
    command_list->SetComputeRoot32BitConstants(
        3, sizeof(AFWVelocityCombineConstants) / sizeof(uint32_t), &constants, 0);

    auto velocity_state = raw_velocity_desc.initialState;
    if (velocity_state == D3D12_RESOURCE_STATE_COMMON) {
        velocity_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    const bool transition_velocity = (velocity_state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0;
    if (transition_velocity) {
        afw_transition_resource(command_list, raw_velocity_desc.pTexture, velocity_state,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    afw_transition_resource(command_list, output_desc.pTexture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    command_list->Dispatch((constants.output_size[0] + 7) / 8, (constants.output_size[1] + 7) / 8, 1);

    afw_uav_barrier(command_list, output_desc.pTexture);
    afw_transition_resource(command_list, output_desc.pTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    if (transition_velocity) {
        afw_transition_resource(command_list, raw_velocity_desc.pTexture,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, velocity_state);
    }

    return true;
}

bool D3D12Component::prepare_frame_resource_combined_motion_vectors(
    VR* vr,
    ID3D12GraphicsCommandList* command_list,
    EyeIndex eye,
    TextureDesc& output_dst,
    bool force_reconstruct) {
    if (vr == nullptr || vr->d3d12Renderer == nullptr || command_list == nullptr ||
        output_dst.pTexture == nullptr) {
        return false;
    }

    if (!uevr_afw_bridge::enabled() || !uevr_afw_bridge::available()) {
        SPDLOG_WARNING_EVERY_N_SEC(2, "[VR] Combined debug motion vectors unavailable: {}",
                                   uevr_afw_bridge::describe_state());
        return false;
    }

    UEVR_FrameResourceView depth_view{};
    if (!uevr_afw_bridge::get_latest_depth(&depth_view) || depth_view.d3d12_resource == nullptr) {
        SPDLOG_WARNING_EVERY_N_SEC(2, "[VR] Combined debug motion vectors missing depth: provider={} validity={} reason={}",
                                   depth_view.provider, depth_view.validity,
                                   depth_view.reason != nullptr ? depth_view.reason : "");
        return false;
    }

    UEVR_FrameResourceView velocity_view{};
    if (!uevr_afw_bridge::get_latest_velocity(&velocity_view) || velocity_view.d3d12_resource == nullptr) {
        SPDLOG_WARNING_EVERY_N_SEC(2, "[VR] Combined debug motion vectors missing velocity: provider={} validity={} reason={}",
                                   velocity_view.provider, velocity_view.validity,
                                   velocity_view.reason != nullptr ? velocity_view.reason : "");
        return false;
    }

    if (!afw_bridge_velocity_format_allowed(velocity_view.format)) {
        SPDLOG_WARNING_EVERY_N_SEC(2, "[VR] Combined debug motion vectors rejected velocity fmt={} provider={} ptr={}",
                                   velocity_view.format, velocity_view.provider, velocity_view.d3d12_resource);
        return false;
    }

    const auto dst_desc = output_dst.pTexture->GetDesc();
    const uint32_t out_w = static_cast<uint32_t>(dst_desc.Width);
    const uint32_t out_h = dst_desc.Height;
    if (out_w == 0 || out_h == 0) {
        return false;
    }

    const auto depth_format = afw_bridge_depth_srv_format(static_cast<DXGI_FORMAT>(depth_view.format));
    if (!afw_bridge_ensure_texture(vr->d3d12Renderer, vr->depthDesc[eye], depth_view, depth_format,
                                   D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, false, "debug combined depth")) {
        return false;
    }

    auto depth_src = afw_bridge_source_desc(depth_view, Depth);
    const D3D12_BOX depth_box{0, 0, 0, depth_view.width, depth_view.height, 1};
    afw_copy_texture_region(command_list, vr->depthDesc[eye], depth_src, depth_box);

    auto output_view = velocity_view;
    output_view.width = out_w;
    output_view.height = out_h;
    if (!afw_bridge_ensure_texture(vr->d3d12Renderer, m_combined_debug_velocity_desc[eye], output_view,
                                   DXGI_FORMAT_R16G16_FLOAT,
                                   D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, true,
                                   "debug combined velocity")) {
        return false;
    }

    const bool combined = combine_ue_velocity_for_afw(
        vr, command_list, velocity_view, vr->depthDesc[eye], m_combined_debug_velocity_desc[eye], eye,
        force_reconstruct);
    if (combined) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[VR] OpenXR frame-resource debug view prepared combined/dense motion vectors velocity={}x{} depth={}x{} out={}x{} forceReconstruct={}",
                                velocity_view.width, velocity_view.height,
                                depth_view.width, depth_view.height,
                                out_w, out_h,
                                force_reconstruct ? 1 : 0);
    }

    return combined;
}

bool D3D12Component::setup_debug_view_pipeline(ID3D12Device* device) {
    if (device == nullptr) {
        return false;
    }
    if (m_debug_view_root_signature != nullptr && m_debug_view_pso != nullptr) {
        return true;
    }

    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srv_range.NumDescriptors = 1; srv_range.BaseShaderRegister = 0;
    srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_DESCRIPTOR_RANGE uav_range{};
    uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uav_range.NumDescriptors = 1; uav_range.BaseShaderRegister = 0;
    uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rp[3]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[0].DescriptorTable.NumDescriptorRanges = 1;
    rp[0].DescriptorTable.pDescriptorRanges = &srv_range; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &uav_range; rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[2].Constants.ShaderRegister = 0;
    rp[2].Constants.Num32BitValues = sizeof(AFWDebugVizConstants) / sizeof(uint32_t); rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd{}; rsd.NumParameters = 3; rsd.pParameters = rp; rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        SPDLOG_ERROR("[VR] Failed to serialize AFW debug-view root signature: {}",
                     err != nullptr ? static_cast<const char*>(err->GetBufferPointer()) : "");
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                           IID_PPV_ARGS(&m_debug_view_root_signature)))) {
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_debug_view_root_signature.Get();
    pso.CS = D3D12_SHADER_BYTECODE{afw_debug_visualize_cs_DebugVisualizeCS, sizeof(afw_debug_visualize_cs_DebugVisualizeCS)};
    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_debug_view_pso)))) {
        m_debug_view_root_signature.Reset();
        return false;
    }
    spdlog::info("[VR] AFW debug-view pipeline setup complete");
    return true;
}

bool D3D12Component::render_frame_resource_debug_view(VR* vr, ID3D12GraphicsCommandList* command_list, int view_mode,
                                                      TextureDesc& backbuffer_dst, TextureDesc* other_eye_dst) {
    if (vr == nullptr || vr->d3d12Renderer == nullptr || command_list == nullptr || view_mode <= 0) {
        return false;
    }

    if (!uevr_afw_bridge::enabled() || !uevr_afw_bridge::available()) {
        SPDLOG_WARNING_EVERY_N_SEC(2, "[VR] Frame-resource debug view unavailable: {}",
                                   uevr_afw_bridge::describe_state());
        return false;
    }

    constexpr EyeIndex debug_eye = EyeLeft;

    if (view_mode >= 1 && view_mode <= 3) {
        const bool have_existing_motion_vectors =
            m_combined_debug_velocity_desc[debug_eye].pTexture != nullptr &&
            m_combined_debug_velocity_desc[debug_eye].shaderResourceViewHandle.ptr != 0;
        const bool force_reconstruct = view_mode == 3 || vr->afw_force_reconstruct;
        const bool have_combined_motion_vectors =
            prepare_frame_resource_combined_motion_vectors(vr, command_list, debug_eye, backbuffer_dst,
                                                           force_reconstruct);
        if (!have_combined_motion_vectors && !have_existing_motion_vectors) {
            SPDLOG_WARNING_EVERY_N_SEC(2, "[VR] Combined/dense debug motion vectors unavailable");
            return false;
        }
        SPDLOG_INFO_EVERY_N_SEC(2, "[VR] OpenXR frame-resource debug view drawing {}{}",
                                VR::afw_debug_view_name(view_mode),
                                have_combined_motion_vectors ? "" : " (existing texture)");
        render_debug_view(vr, command_list, view_mode, debug_eye, backbuffer_dst, other_eye_dst);
        return true;
    }

    if (view_mode == 4) {
        UEVR_FrameResourceView depth_view{};
        if (!uevr_afw_bridge::get_latest_depth(&depth_view) || depth_view.d3d12_resource == nullptr) {
            SPDLOG_WARNING_EVERY_N_SEC(2, "[VR] Frame-resource debug depth unavailable: provider={} validity={} reason={}",
                                       depth_view.provider, depth_view.validity,
                                       depth_view.reason != nullptr ? depth_view.reason : "");
            return false;
        }

        const auto depth_format = afw_bridge_depth_srv_format(static_cast<DXGI_FORMAT>(depth_view.format));
        if (!afw_bridge_ensure_texture(vr->d3d12Renderer, vr->depthDesc[debug_eye], depth_view, depth_format,
                                       D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, false, "debug depth")) {
            return false;
        }

        auto src = afw_bridge_source_desc(depth_view, Depth);
        const D3D12_BOX source_box{0, 0, 0, depth_view.width, depth_view.height, 1};
        afw_copy_texture_region(command_list, vr->depthDesc[debug_eye], src, source_box);
        SPDLOG_INFO_EVERY_N_SEC(2, "[VR] OpenXR frame-resource debug view drawing depth provider={} ptr={} {}x{} fmt={}",
                                depth_view.provider, depth_view.d3d12_resource, depth_view.width,
                                depth_view.height, depth_view.format);
        render_debug_view(vr, command_list, view_mode, debug_eye, backbuffer_dst, other_eye_dst);
        return true;
    }

    UEVR_FrameResourceView velocity_view{};
    if (!uevr_afw_bridge::get_latest_velocity(&velocity_view) || velocity_view.d3d12_resource == nullptr) {
        SPDLOG_WARNING_EVERY_N_SEC(2, "[VR] Frame-resource debug velocity unavailable: provider={} validity={} reason={}",
                                   velocity_view.provider, velocity_view.validity,
                                   velocity_view.reason != nullptr ? velocity_view.reason : "");
        return false;
    }

    auto& raw_velocity_desc = m_raw_velocity_desc[debug_eye];
    if (raw_velocity_desc.pTexture != velocity_view.d3d12_resource ||
        raw_velocity_desc.initialState != static_cast<D3D12_RESOURCE_STATES>(velocity_view.expected_state) ||
        raw_velocity_desc.shaderResourceViewHandle.ptr == 0) {
        raw_velocity_desc = afw_bridge_source_desc(velocity_view, Image);
        vr->d3d12Renderer->SetupTextureDesc(raw_velocity_desc);
    }

    SPDLOG_INFO_EVERY_N_SEC(2, "[VR] OpenXR frame-resource debug view drawing {} provider={} ptr={} {}x{} fmt={}",
                            VR::afw_debug_view_name(view_mode), velocity_view.provider,
                            velocity_view.d3d12_resource, velocity_view.width,
                            velocity_view.height, velocity_view.format);
    render_debug_view(vr, command_list, view_mode, debug_eye, backbuffer_dst, other_eye_dst);
    return true;
}

void D3D12Component::render_debug_view(VR* vr, ID3D12GraphicsCommandList* command_list, int view_mode, EyeIndex eye,
                                       TextureDesc& backbuffer_dst, TextureDesc* other_eye_dst) {
    if (vr == nullptr || vr->d3d12Renderer == nullptr || command_list == nullptr || view_mode <= 0) {
        return;
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (!setup_debug_view_pipeline(device)) {
        return;
    }

    TextureDesc* input = nullptr;
    uint32_t shader_mode = 0;
    float scale = 1.0f;
    bool transition_input = false;
    D3D12_RESOURCE_STATES input_prev_state = D3D12_RESOURCE_STATE_COMMON;
    switch (view_mode) {
    case 1: input = &m_combined_debug_velocity_desc[eye]; shader_mode = 0; scale = 1.0f / 200.0f; break; // combined/PDAFW MV
    case 2: input = &m_combined_debug_velocity_desc[eye]; shader_mode = 0; scale = 1.0f / 40.0f; break;  // boosted combined/PDAFW MV
    case 3: input = &m_combined_debug_velocity_desc[eye]; shader_mode = 0; scale = 1.0f / 40.0f; break;  // reconstruct-only combined/PDAFW MV
    case 4: input = &vr->depthDesc[eye];         shader_mode = 1; scale = 1.0f; break;          // depth
    case 5: input = &m_raw_velocity_desc[eye];   shader_mode = 2; scale = 40.0f;                // source velocity direction
            input_prev_state = input->initialState; transition_input = true; break;
    case 6: input = &m_raw_velocity_desc[eye];   shader_mode = 3; scale = 80.0f;                // source velocity X
            input_prev_state = input->initialState; transition_input = true; break;
    case 7: input = &m_raw_velocity_desc[eye];   shader_mode = 4; scale = 80.0f;                // source velocity Y
            input_prev_state = input->initialState; transition_input = true; break;
    case 8: input = &m_raw_velocity_desc[eye];   shader_mode = 5; scale = 40.0f;                // source velocity magnitude
            input_prev_state = input->initialState; transition_input = true; break;
    case 9: input = &m_raw_velocity_desc[eye];   shader_mode = 6; scale = 1.0f;                 // source velocity validity
            input_prev_state = input->initialState; transition_input = true; break;
    default: return;
    }
    if (input == nullptr || input->pTexture == nullptr || input->shaderResourceViewHandle.ptr == 0) {
        return;
    }

    const auto dst_desc = (other_eye_dst != nullptr && other_eye_dst->pTexture != nullptr)
        ? other_eye_dst->pTexture->GetDesc() : backbuffer_dst.pTexture->GetDesc();
    const uint32_t out_w = static_cast<uint32_t>(dst_desc.Width);
    const uint32_t out_h = dst_desc.Height;

    if (m_debug_view_tex.pTexture == nullptr ||
        m_debug_view_tex.pTexture->GetDesc().Width != out_w || m_debug_view_tex.pTexture->GetDesc().Height != out_h) {
        if (!vr->d3d12Renderer->CreateTexture(static_cast<int>(out_w), static_cast<int>(out_h),
                                              DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                                              m_debug_view_tex, true)) {
            return;
        }
        if (m_debug_view_tex.pTexture != nullptr) {
            m_debug_view_tex.pTexture->SetName(L"UEVR AFW debug false color");
        }
        m_debug_view_tex.unorderedAccessViewHandle.ptr = 0;
        m_debug_view_tex.uavPos = -1;
    }
    if (m_debug_view_tex.unorderedAccessViewHandle.ptr == 0 || m_debug_view_tex.uavPos < 0) {
        const int uav = vr->d3d12Renderer->CreateUAV(m_debug_view_tex.pTexture);
        if (uav < 0) {
            return;
        }
        m_debug_view_tex.uavPos = uav;
        m_debug_view_tex.unorderedAccessViewHandle = vr->d3d12Renderer->GetGPUDescriptorHandle(uav);
    }

    const auto in_desc = input->pTexture->GetDesc();
    AFWDebugVizConstants c{};
    c.mode = shader_mode; c.scale = scale;
    c.input_size[0] = static_cast<uint32_t>(in_desc.Width); c.input_size[1] = in_desc.Height;
    c.output_size[0] = out_w; c.output_size[1] = out_h;
    if (c.input_size[0] == 0 || c.input_size[1] == 0 || c.output_size[0] == 0 || c.output_size[1] == 0) {
        return;
    }

    ID3D12DescriptorHeap* heaps[] = { vr->d3d12Renderer->GetViewHeap() };
    command_list->SetDescriptorHeaps(1, heaps);
    command_list->SetComputeRootSignature(m_debug_view_root_signature.Get());
    command_list->SetPipelineState(m_debug_view_pso.Get());
    command_list->SetComputeRootDescriptorTable(0, input->shaderResourceViewHandle);
    command_list->SetComputeRootDescriptorTable(1, m_debug_view_tex.unorderedAccessViewHandle);
    command_list->SetComputeRoot32BitConstants(2, sizeof(AFWDebugVizConstants) / sizeof(uint32_t), &c, 0);

    if (transition_input) {
        auto st = input_prev_state;
        if (st == D3D12_RESOURCE_STATE_COMMON) st = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if ((st & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0) {
            afw_transition_resource(command_list, input->pTexture, st, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            input_prev_state = st;
        } else {
            transition_input = false;
        }
    }
    afw_transition_resource(command_list, m_debug_view_tex.pTexture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    command_list->Dispatch((out_w + 7) / 8, (out_h + 7) / 8, 1);
    afw_uav_barrier(command_list, m_debug_view_tex.pTexture);
    afw_transition_resource(command_list, m_debug_view_tex.pTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    if (transition_input) {
        afw_transition_resource(command_list, input->pTexture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                input_prev_state);
    }

    // Blit the false-colored buffer into both submitted eyes so it shows in the HMD / mirror.
    if (other_eye_dst != nullptr && other_eye_dst->pTexture != nullptr) {
        D3D12_VIEWPORT vp{0, 0, static_cast<float>(out_w), static_cast<float>(out_h), 0, 1};
        vr->d3d12Renderer->Blit(command_list, *other_eye_dst, m_debug_view_tex, vp);
    }
    if (backbuffer_dst.pTexture != nullptr) {
        const auto bb = backbuffer_dst.pTexture->GetDesc();
        D3D12_VIEWPORT vp{0, 0, static_cast<float>(bb.Width), static_cast<float>(bb.Height), 0, 1};
        vr->d3d12Renderer->Blit(command_list, backbuffer_dst, m_debug_view_tex, vp);
    }
}

void D3D12Component::draw_spectator_view(ID3D12GraphicsCommandList* command_list, bool is_right_eye_frame) {
    if (command_list == nullptr || m_game_ui_tex.texture == nullptr) {
        return;
    }

    if (m_game_ui_tex.srv_heap == nullptr || m_game_ui_tex.srv_heap->Heap() == nullptr) {
        return;
    }

    if (m_game_tex.texture == nullptr || m_game_tex.srv_heap == nullptr || m_game_tex.srv_heap->Heap() == nullptr) {
        return;
    }

    const auto& vr = VR::get();

    if (!vr->is_hmd_active() || !vr->m_desktop_fix->value()) {
        return;
    }

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};
    const auto index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&backbuffer)))) {
        return;
    }

    if (index >= m_backbuffer_textures.size()) {
        m_backbuffer_textures.resize(index + 1);
        spdlog::info("[VR] Resized backbuffer textures to {}", index + 1);

        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx_ptr = m_backbuffer_textures[index];
    
    if (backbuffer_ctx_ptr == nullptr) {
        // if this has happened, assume the rest of the textures are also null
        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx = *backbuffer_ctx_ptr;

    const auto desc = backbuffer->GetDesc();

    if (backbuffer_ctx.texture.Get() != backbuffer.Get()) {
        if (!backbuffer_ctx.setup(device, backbuffer.Get(), std::nullopt, std::nullopt, L"Backbuffer")) {
            spdlog::error("[VR] Failed to setup backbuffer RTV (D3D12)");
            return;
        }

        spdlog::info("[VR] Created backbuffer RTV (D3D12)");
    }

    if (backbuffer_ctx.rtv_heap == nullptr || backbuffer_ctx.rtv_heap->Heap() == nullptr) {
        spdlog::error("[VR] Backbuffer RTV heap is null (D3D12)");
        return;
    }

    // Copy the previous right eye frame to the left eye frame
    const auto prev_index = (index + m_backbuffer_textures.size() - 1) % m_backbuffer_textures.size();
    if ((vr->is_using_afr() || vr->is_using_afw()) && !is_right_eye_frame && m_backbuffer_textures[prev_index]->texture != nullptr) {
        const auto& last_right_eye_buffer = m_backbuffer_textures[prev_index]->texture;

        if (backbuffer.Get() != last_right_eye_buffer.Get()) {
            m_generic_commands[index % 3].wait(INFINITE);
            m_generic_commands[index % 3].copy(last_right_eye_buffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
            m_generic_commands[index % 3].execute();

            return;
        }
    }

    auto& batch = m_backbuffer_batch;

    D3D12_VIEWPORT viewport{};
    viewport.Width = (float)desc.Width;
    viewport.Height = (float)desc.Height;
    viewport.MaxDepth = 1.0f;
    
    batch->SetViewport(viewport);

    D3D12_RECT scissor_rect{};
    scissor_rect.left = 0;
    scissor_rect.top = 0;
    scissor_rect.right = (LONG)desc.Width;
    scissor_rect.bottom = (LONG)desc.Height;

    // Transition backbuffer to D3D12_RESOURCE_STATE_RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backbuffer.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list->ResourceBarrier(1, &barrier);

    // Set RTV to backbuffer
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_heaps[] = { backbuffer_ctx.get_rtv() };
    command_list->OMSetRenderTargets(1, rtv_heaps, FALSE, nullptr);

    // Clear backbuffer
    const float bb_clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    command_list->ClearRenderTargetView(backbuffer_ctx.get_rtv(), bb_clear_color, 0, nullptr);

    // Setup viewport and scissor rects
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor_rect);

    batch->Begin(command_list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate);

    RECT dest_rect{ 0, 0, (LONG)desc.Width, (LONG)desc.Height };

    const auto aspect_ratio = (float)desc.Width / (float)desc.Height;

    const auto eye_width = ((float)m_backbuffer_size[0] / 2.0f);
    const auto eye_height = (float)m_backbuffer_size[1];
    const auto eye_aspect_ratio = eye_width / eye_height;

    const auto original_centerw = (float)eye_width / 2.0f;
    const auto original_centerh = (float)eye_height / 2.0f;

    ///////////////
    // Eye (game) texture
    ///////////////
    // only show one half of the double wide texture (right side)
    RECT source_rect{};

    // Show left side when using AFR or native stereo fix
    if (vr->is_using_afr() || vr->is_native_stereo_fix_enabled() || vr->is_using_afw()) {
        source_rect.left = 0;
        source_rect.top = 0;
        source_rect.right = m_backbuffer_size[0] / 2;
        source_rect.bottom = m_backbuffer_size[1];
    } else {
        source_rect.left = (LONG)m_backbuffer_size[0] / 2;
        source_rect.top = 0;
        source_rect.right = m_backbuffer_size[0];
        source_rect.bottom = m_backbuffer_size[1];
    }

    // Correct left/top/right/bottom to match the aspect ratio of the game
    if (eye_aspect_ratio > aspect_ratio) {
        const auto new_width = eye_height * aspect_ratio;
        const auto new_centerw = new_width / 2.0f;
        source_rect.left = (LONG)(original_centerw - new_centerw);
        source_rect.right = (LONG)(original_centerw + new_centerw);
    } else {
        const auto new_height = eye_width / aspect_ratio;
        const auto new_centerh = new_height / 2.0f;
        source_rect.top = (LONG)(original_centerh - new_centerh);
        source_rect.bottom = (LONG)(original_centerh + new_centerh);
    }

    // Set descriptor heaps
    ID3D12DescriptorHeap* game_heaps[] = { m_game_tex.srv_heap->Heap() };
    command_list->SetDescriptorHeaps(1, game_heaps);

    batch->Draw(m_game_tex.get_srv_gpu(), 
        DirectX::XMUINT2{ (uint32_t)m_backbuffer_size[0], (uint32_t)m_backbuffer_size[1] },
        dest_rect,
        &source_rect, 
        DirectX::Colors::White);

    //////
    // UI
    //////
    // Set descriptor heaps
    ID3D12DescriptorHeap* ui_heaps[] = { m_game_ui_tex.srv_heap->Heap() };
    command_list->SetDescriptorHeaps(1, ui_heaps);

    batch->Draw(m_game_ui_tex.get_srv_gpu(), 
        DirectX::XMUINT2{ (uint32_t)desc.Width, (uint32_t)desc.Height },
        dest_rect, 
        DirectX::Colors::White);

    batch->End();

    // Transition backbuffer to D3D12_RESOURCE_STATE_PRESENT
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    command_list->ResourceBarrier(1, &barrier);
}

void D3D12Component::clear_backbuffer() {
    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    if (device == nullptr || swapchain == nullptr) {
        return;
    }

    ComPtr<ID3D12Resource> backbuffer{};
    const auto index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&backbuffer)))) {
        return;
    }

    if (backbuffer == nullptr) {
        return;
    }

    if (index >= m_backbuffer_textures.size()) {
        m_backbuffer_textures.resize(index + 1);
        spdlog::info("[VR] Resized backbuffer textures to {}", index + 1);

        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx_ptr = m_backbuffer_textures[index];
    
    if (backbuffer_ctx_ptr == nullptr) {
        // if this has happened, assume the rest of the textures are also null
        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx = *backbuffer_ctx_ptr;

    if (backbuffer_ctx.texture.Get() != backbuffer.Get()) {
        if (!backbuffer_ctx.setup(device, backbuffer.Get(), std::nullopt, std::nullopt, L"Backbuffer")) {
            spdlog::error("[VR] Failed to setup backbuffer RTV (D3D12)");
            return;
        }

        spdlog::info("[VR] Created backbuffer RTV (D3D12)");
    }

    // oh well
    if (backbuffer_ctx.rtv_heap == nullptr || backbuffer_ctx.rtv_heap->Heap() == nullptr) {
        return;
    }

    // Clear the backbuffer
    backbuffer_ctx.commands.wait(0);
    const float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    backbuffer_ctx.commands.clear_rtv(backbuffer_ctx.texture.Get(), backbuffer_ctx.get_rtv(), clear_color, D3D12_RESOURCE_STATE_PRESENT);
    backbuffer_ctx.commands.execute();
}

void D3D12Component::on_post_present(VR* vr) {
    if (m_graphics_memory != nullptr) {
        auto& hook = g_framework->get_d3d12_hook();

        auto device = hook->get_device();
        auto command_queue = hook->get_command_queue();

        m_graphics_memory->Commit(command_queue);
    }

    // Clear the (real) backbuffer if VR is enabled. Otherwise it will flicker and all sorts of nasty things.
    if (vr->is_hmd_active()) {
        clear_backbuffer();
    }
}

void D3D12Component::on_reset(VR* vr) {
    m_force_reset = true;

    auto runtime = vr->get_runtime();

    for (auto& ctx : m_openvr.left_eye_tex) {
        ctx.reset();
    }

    for (auto& ctx : m_openvr.right_eye_tex) {
        ctx.reset();
    }

    for (auto& commands : m_generic_commands) {
        commands.reset();
    }

    for (auto& commands : m_game_tex_commands) {
        commands.reset();
    }

    for (auto& backbuffer : m_backbuffer_textures) {
        backbuffer.reset();
    }

    for (auto & screen : m_2d_screen_tex) {
        screen.reset();
    }

    m_openvr.ui_tex.reset();
    m_game_ui_tex.reset();
    m_game_tex.reset();
    m_scene_capture_tex.reset();
    m_backbuffer_batch.reset();
    m_game_batch.reset();
    m_ui_batch_alpha_invert.reset();
    m_graphics_memory.reset();

    if (runtime->is_openxr() && runtime->loaded) {
        m_openxr.wait_for_all_copies();

        auto& rt_pool = vr->get_render_target_pool_hook();
        ComPtr<ID3D12Resource> scene_depth_tex{rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ")};

        bool needs_depth_resize = false;

        if (scene_depth_tex != nullptr) {
            const auto desc = scene_depth_tex->GetDesc();
            needs_depth_resize = vr->m_openxr->needs_depth_resize(desc.Width, desc.Height);

            if (needs_depth_resize) {
                spdlog::info("[VR] SceneDepthZ needs resize ({}x{})", desc.Width, desc.Height);
            }
        }


        if (m_openxr.last_resolution[0] != vr->get_hmd_width() || m_openxr.last_resolution[1] != vr->get_hmd_height() ||
            vr->m_openxr->swapchains.empty() ||
            g_framework->get_d3d12_rt_size()[0] != vr->m_openxr->swapchains[(uint32_t)runtimes::OpenXR::SwapchainIndex::UI].width ||
            g_framework->get_d3d12_rt_size()[1] != vr->m_openxr->swapchains[(uint32_t)runtimes::OpenXR::SwapchainIndex::UI].height ||
            m_last_afr_state != vr->is_using_afr() ||
            needs_depth_resize)
        {
            m_openxr.create_swapchains();
            m_last_afr_state = vr->is_using_afr();
        }

        // end the frame before something terrible happens
        //vr->m_openxr.synchronize_frame();
        //vr->m_openxr.begin_frame();
        //vr->m_openxr.end_frame();
    }

    m_prev_backbuffer.Reset();
    m_openvr.texture_counter = 0;
}

bool D3D12Component::setup() {
    SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Setting up d3d12 textures...");

    auto vr = VR::get();
    on_reset(vr.get());
    
    m_prev_backbuffer.Reset();

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    auto ue4_texture = vr->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

    if (ue4_texture != nullptr) {
        backbuffer = (ID3D12Resource*)ue4_texture->get_native_resource();
    }

    ComPtr<ID3D12Resource> real_backbuffer{};
    if (FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&real_backbuffer)))) {
        spdlog::error("[VR] Failed to get real back buffer (D3D12).");
        return false;
    }

    if (vr->is_extreme_compatibility_mode_enabled()) {
        backbuffer = real_backbuffer;
    }

    if (backbuffer == nullptr && (vr->is_stereo_emulation_enabled() || afw_d3d12_allow_swapchain_backbuffer_fallback())) {
        backbuffer = real_backbuffer;
        if (vr->is_stereo_emulation_enabled()) {
            SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Stereo emulation: using DXGI swapchain backbuffer for D3D12 setup");
        } else {
            SPDLOG_WARNING_EVERY_N_SEC(1, "[VR] Fake stereo render target unavailable; using DXGI swapchain backbuffer for D3D12 setup");
        }
    }

    if (backbuffer == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] Failed to get back buffer (D3D12).");
        return false;
    }

    if (m_graphics_memory == nullptr) {
        m_graphics_memory = std::make_unique<DirectX::DX12::GraphicsMemory>(device);
    }

    const auto real_backbuffer_desc = real_backbuffer->GetDesc();

    auto backbuffer_desc = backbuffer->GetDesc();

    spdlog::info("[VR] D3D12 Real backbuffer width: {}, height: {}, format: {}", real_backbuffer_desc.Width, real_backbuffer_desc.Height, (uint32_t)real_backbuffer_desc.Format);

    backbuffer_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    backbuffer_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    backbuffer_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

    if (!vr->is_extreme_compatibility_mode_enabled() && !vr->is_stereo_emulation_enabled()) {
        backbuffer_desc.Width /= 2; // The texture we get from UE is both eyes combined. we will copy the regions later.
    }

    spdlog::info("[VR] D3D12 RT width: {}, height: {}, format: {}", backbuffer_desc.Width, backbuffer_desc.Height, (uint32_t)backbuffer_desc.Format);

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    if (vr->is_using_2d_screen()) {
        auto screen_desc = backbuffer_desc;
        screen_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        screen_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        screen_desc.Width = (uint32_t)g_framework->get_d3d12_rt_size().x;
        screen_desc.Height = (uint32_t)g_framework->get_d3d12_rt_size().y;

        for (auto& context : m_2d_screen_tex) {
            ComPtr<ID3D12Resource> screen_tex{};
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &screen_desc, ENGINE_SRC_COLOR, nullptr,
                    IID_PPV_ARGS(&screen_tex)))) {
                spdlog::error("[VR] Failed to create 2D screen texture.");
                continue;
            }

            screen_tex->SetName(L"2D Screen Texture");

            if (!context.setup(device, screen_tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"2D Screen")) {
                spdlog::error("[VR] Failed to setup 2D screen context.");
                continue;
            }
        }
    }

    // #############################
    // #Frame Warp Module Start
    // #############################
    static uint32_t lastSize[2]{0, 0};
    static DXGI_FORMAT lastFormat = DXGI_FORMAT_UNKNOWN;
    if (vr->m_rendering_method->value() == VR::RenderingMethod::ALTERNATE_FRAMEWARP && vr->d3d12Renderer == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] Alternate Frame Warp requested, but PDAFWPlugin is unavailable");
    }

    if (vr->is_using_afw() &&
        (lastSize[0] != vr->get_hmd_width() || lastSize[1] != vr->get_hmd_height() || lastFormat != backbuffer_desc.Format)) {
        FrameWarpInitParams params = {static_cast<int>(vr->get_hmd_width()), static_cast<int>(vr->get_hmd_height()), backbuffer_desc.Format};
        spdlog::info("[VR] Before InitFrameWarp");
        m_eyeFrameBuffers = InitFrameWarp(params);
        spdlog::info("[VR] After InitFrameWarp");
        spdlog::info("[VR] m_eyeFrameBuffers[0]: {} ", (void*)m_eyeFrameBuffers.eyeFrameBuffers[0].color.pTexture);
        if (m_eyeFrameBuffers.eyeFrameBuffers[0].color.pTexture == nullptr ||
            m_eyeFrameBuffers.eyeFrameBuffers[1].color.pTexture == nullptr) {
            spdlog::error("[VR] InitFrameWarp returned null eye frame buffers; disabling Alternate Frame Warp");
            vr->d3d12Renderer = nullptr;
            return false;
        }
        lastSize[0] = vr->get_hmd_width();
        lastSize[1] = vr->get_hmd_height();
        lastFormat = backbuffer_desc.Format;
    }
    // #############################
    // #Frame Warp Module End
    // #############################

    if (vr->get_runtime()->is_openvr()) {
        for (auto& ctx : m_openvr.left_eye_tex) {
            if (m_eyeFrameBuffers.eyeFrameBuffers[0].color.pTexture != NULL) {
                ctx.texture = m_eyeFrameBuffers.eyeFrameBuffers[0].color.pTexture;
            } else {
                if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_desc,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&ctx.texture)))) {
                    spdlog::error("[VR] Failed to create left eye texture.");
                    return false;
                }
            }

            ctx.texture->SetName(L"OpenVR Left Eye Texture");
            if (!ctx.commands.setup(L"OpenVR Left Eye")) {
                spdlog::error("[VR] Failed to setup left eye context.");
                return false;
            }
        }

        for (auto& ctx : m_openvr.right_eye_tex) {
            if (m_eyeFrameBuffers.eyeFrameBuffers[1].color.pTexture != NULL) {
                ctx.texture = m_eyeFrameBuffers.eyeFrameBuffers[1].color.pTexture;
            } else {
                if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_desc,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&ctx.texture)))) {
                    spdlog::error("[VR] Failed to create right eye texture.");
                    return false;
                }
            }

            ctx.texture->SetName(L"OpenVR Right Eye Texture");
            if (!ctx.commands.setup(L"OpenVR Right Eye")) {
                spdlog::error("[VR] Failed to setup right eye context.");
                return false;
            }
        }

        // Set up the UI texture. it's the desktop resolution.
        auto ui_desc = backbuffer_desc;
        ui_desc.Width = (uint32_t)g_framework->get_d3d12_rt_size().x;
        ui_desc.Height = (uint32_t)g_framework->get_d3d12_rt_size().y;

        ComPtr<ID3D12Resource> ui_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &ui_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&ui_tex)))) {
            spdlog::error("[VR] Failed to create UI texture.");
            return false;
        }

        ui_tex->SetName(L"OpenVR UI Texture");

        if (!m_openvr.ui_tex.setup(device, ui_tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"OpenVR UI")) {
            spdlog::error("[VR] Failed to setup OpenVR UI context.");
            return false;
        }
    }

    for (auto& commands : m_generic_commands) {
        if (!commands.setup(L"Generic commands")) {
            return false;
        }
    }

    if (!vr->is_extreme_compatibility_mode_enabled() && !vr->is_stereo_emulation_enabled()) {
        m_backbuffer_size[0] = backbuffer_desc.Width * 2;
    } else {
        m_backbuffer_size[0] = backbuffer_desc.Width;
    }

    m_backbuffer_size[1] = backbuffer_desc.Height;

    m_backbuffer_batch = setup_sprite_batch_pso(real_backbuffer_desc.Format);
    m_game_batch = setup_sprite_batch_pso(backbuffer_desc.Format);

    // Custom blend state to flip the alpha in-place of the UI texture without an intermediate render target
    {
        DirectX::SpriteBatchPipelineStateDescription invert_alpha_in_place_pd{DirectX::RenderTargetState{backbuffer_desc.Format, DXGI_FORMAT_UNKNOWN}};

        auto& bd = invert_alpha_in_place_pd.blendDesc;
        auto& bdrt = bd.RenderTarget[0];
        bdrt.BlendEnable = TRUE;

        bdrt.SrcBlend = D3D12_BLEND_ONE;
        bdrt.DestBlend = D3D12_BLEND_ZERO;
        bdrt.BlendOp = D3D12_BLEND_OP_ADD;

        bdrt.SrcBlendAlpha = D3D12_BLEND_ONE;
        bdrt.DestBlendAlpha = D3D12_BLEND_ZERO;
        bdrt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        bdrt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        m_ui_batch_alpha_invert = setup_sprite_batch_pso(
            backbuffer_desc.Format, 
            alpha_luminance_sprite_ps_SpritePixelShader, 
            alpha_luminance_sprite_ps_SpriteVertexShader, 
            invert_alpha_in_place_pd
        );
    }

    spdlog::info("[VR] d3d12 textures have been setup");
    m_force_reset = false;

    return true;
}

void D3D12Component::OpenXR::initialize(XrSessionCreateInfo& session_info) {
    std::scoped_lock _{this->mtx};

	auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();

    this->binding.device = device;
    this->binding.queue = command_queue;

    spdlog::info("[VR] Searching for xrGetD3D12GraphicsRequirementsKHR...");
    PFN_xrGetD3D12GraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(VR::get()->m_openxr->instance, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)(&fn));

    XrGraphicsRequirementsD3D12KHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    gr.adapterLuid = device->GetAdapterLuid();
    gr.minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

    spdlog::info("[VR] Calling xrGetD3D12GraphicsRequirementsKHR");
    fn(VR::get()->m_openxr->instance, VR::get()->m_openxr->system, &gr);

    session_info.next = &this->binding;
}

std::optional<std::string> D3D12Component::OpenXR::create_swapchains() {
    std::scoped_lock _{this->mtx};

    spdlog::info("[VR] Creating OpenXR swapchains for D3D12");

    this->destroy_swapchains();
    
    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    auto vr = VR::get();
    bool has_actual_vr_backbuffer = false;

    if (vr != nullptr && vr->m_fake_stereo_hook != nullptr) {
        auto ue4_texture = vr->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

        if (ue4_texture != nullptr) {
            backbuffer = (ID3D12Resource*)ue4_texture->get_native_resource();
            has_actual_vr_backbuffer = backbuffer != nullptr;
        }
    }
    
    // Get the existing backbuffer
    // so we can get the format and stuff.
    if (backbuffer == nullptr && FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer.");
        return "Failed to get back buffer.";
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    auto backbuffer_desc = backbuffer->GetDesc();
    auto& openxr = vr->m_openxr;

    this->contexts.clear();

    auto create_swapchain = [&](uint32_t i, const XrSwapchainCreateInfo& swapchain_create_info, const D3D12_RESOURCE_DESC& desc) -> std::optional<std::string> {
        // Create the swapchain.
        runtimes::OpenXR::Swapchain swapchain{};
        swapchain.width = swapchain_create_info.width;
        swapchain.height = swapchain_create_info.height;

        if (xrCreateSwapchain(openxr->session, &swapchain_create_info, &swapchain.handle) != XR_SUCCESS) {
            spdlog::error("[VR] D3D12: Failed to create swapchain.");
            return "Failed to create swapchain.";
        }

        vr->m_openxr->swapchains[i] = swapchain;

        uint32_t image_count{};
        auto result = xrEnumerateSwapchainImages(swapchain.handle, 0, &image_count, nullptr);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images.");
            return "Failed to enumerate swapchain images.";
        }

        SPDLOG_INFO("[VR] Runtime wants {} images for swapchain {}", image_count, i);

        auto& ctx = this->contexts[i];

        ctx.textures.clear();
        ctx.textures.resize(image_count);
        ctx.texture_contexts.clear();
        ctx.texture_contexts.resize(image_count);

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            ctx.texture_contexts[j] = std::make_unique<d3d12::TextureContext>();
            ctx.texture_contexts[j]->commands.setup((std::wstring{L"OpenXR commands "} + std::to_wstring(i) + L" " + std::to_wstring(j)).c_str());
        }

        result = xrEnumerateSwapchainImages(swapchain.handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)&ctx.textures[0]);
        
        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images after texture creation.");
            return "Failed to enumerate swapchain images after texture creation.";
        }

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j].texture->AddRef();
            const auto ref_count = ctx.textures[j].texture->Release();

            spdlog::info("[VR] AFTER Swapchain texture {} {} ref count: {}", i, j, ref_count);
        }

        if (swapchain_create_info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) {
            for (uint32_t j = 0; j < image_count; ++j) {
                XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wait_info.timeout = XR_INFINITE_DURATION;
                XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

                uint32_t index{};
                xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &index);
                xrWaitSwapchainImage(swapchain.handle, &wait_info);

                auto& texture_ctx = ctx.texture_contexts[index];
                texture_ctx->texture = ctx.textures[index].texture;

                // Depth stencil textures don't need an RTV.
                if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) {
                    if (ctx.texture_contexts[index]->create_rtv(device, (DXGI_FORMAT)swapchain_create_info.format)) {
                        const float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        texture_ctx->commands.clear_rtv(ctx.textures[index].texture, texture_ctx->get_rtv(), clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);
                        texture_ctx->commands.execute();
                        texture_ctx->commands.wait(100);
                    } else {
                        spdlog::error("[VR] Failed to create RTV for swapchain image {}.", index);
                    }
                }

                texture_ctx->texture.Reset();
                texture_ctx->rtv_heap.reset();

                xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }
        }

        return std::nullopt;
    };

    const auto double_wide_multiple = vr->is_using_afr() ? 1 : 2;

    XrSwapchainCreateInfo standard_swapchain_create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    standard_swapchain_create_info.arraySize = 1;
    standard_swapchain_create_info.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    standard_swapchain_create_info.width = vr->get_hmd_width() * double_wide_multiple;
    standard_swapchain_create_info.height = vr->get_hmd_height();
    standard_swapchain_create_info.mipCount = 1;
    standard_swapchain_create_info.faceCount = 1;
    standard_swapchain_create_info.sampleCount = backbuffer_desc.SampleDesc.Count;
    standard_swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

    auto hmd_desc = backbuffer_desc;
    hmd_desc.Width = vr->get_hmd_width() * double_wide_multiple;
    hmd_desc.Height = vr->get_hmd_height();
    hmd_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    hmd_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    hmd_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    // Above is outdated, we will just use a double wide texture
    if (!vr->is_using_afr()) {
        spdlog::info("[VR] Creating double wide swapchain for eyes");
        spdlog::info("[VR] Width: {}", vr->get_hmd_width() * 2);
        spdlog::info("[VR] Height: {}", vr->get_hmd_height());

        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }
    } else {
        spdlog::info("[VR] Creating AFR swapchain for eyes");
        spdlog::info("[VR] Width: {}", vr->get_hmd_width());
        spdlog::info("[VR] Height: {}", vr->get_hmd_height());

        spdlog::info("[VR] Creating AFR left eye swapchain");
        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }

        spdlog::info("[VR] Creating AFR right eye swapchain");
        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }
    }

    auto virtual_desktop_dummy_desc = backbuffer_desc;
    auto virtual_desktop_dummy_swapchain_create_info = standard_swapchain_create_info;

    virtual_desktop_dummy_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    virtual_desktop_dummy_desc.Width = 4;
    virtual_desktop_dummy_desc.Height = 4;
    virtual_desktop_dummy_swapchain_create_info.width = 4;
    virtual_desktop_dummy_swapchain_create_info.height = 4;
    virtual_desktop_dummy_swapchain_create_info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT; // so we dont need to acquire/release/wait

    // The virtual desktop dummy texture
    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DUMMY_VIRTUAL_DESKTOP, virtual_desktop_dummy_swapchain_create_info, virtual_desktop_dummy_desc)) {
        return err;
    }

    auto desktop_rt_swapchain_create_info = standard_swapchain_create_info;
    desktop_rt_swapchain_create_info.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    desktop_rt_swapchain_create_info.width = g_framework->get_d3d12_rt_size().x;
    desktop_rt_swapchain_create_info.height = g_framework->get_d3d12_rt_size().y;

    auto desktop_rt_desc = backbuffer_desc;
    desktop_rt_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    desktop_rt_desc.Width = g_framework->get_d3d12_rt_size().x;
    desktop_rt_desc.Height = g_framework->get_d3d12_rt_size().y;

    desktop_rt_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    desktop_rt_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    // The UI texture
    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    // Depth textures
    if (vr->get_openxr_runtime()->is_depth_allowed()) {
        // Even when using AFR, the depth tex is always the size of a double wide.
        // That's kind of unfortunate in terms of how many copies we have to do but whatever.
        auto depth_swapchain_create_info = standard_swapchain_create_info;
        depth_swapchain_create_info.format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        depth_swapchain_create_info.createFlags = 0;
        depth_swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
        depth_swapchain_create_info.width = vr->get_hmd_width() * 2;
        depth_swapchain_create_info.height = vr->get_hmd_height();

        auto depth_desc = backbuffer_desc;
        depth_desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        //depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depth_desc.DepthOrArraySize = 1;

        depth_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        depth_desc.Width = vr->get_hmd_width() * 2;
        depth_desc.Height = vr->get_hmd_height();

        auto& rt_pool = vr->get_render_target_pool_hook();
        auto depth_tex = rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ");

        if (depth_tex != nullptr) {
            this->made_depth_with_null_defaults = false;
            depth_desc = depth_tex->GetDesc();

            if (depth_desc.Format == DXGI_FORMAT_R24G8_TYPELESS) {
                depth_swapchain_create_info.format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            }

            spdlog::info("[VR] Depth texture size: {}x{}", depth_desc.Width, depth_desc.Height);
            spdlog::info("[VR] Depth texture format: {}", (uint32_t)depth_desc.Format);
            spdlog::info("[VR] Depth texture flags: {}", (uint32_t)depth_desc.Flags);

            if (depth_desc.Width > hmd_desc.Width || depth_desc.Height > hmd_desc.Height) {
                spdlog::info("[VR] Depth texture is larger than the HMD");
                //depth_desc.Width = hmd_desc.Width;
                //depth_desc.Height = hmd_desc.Height;
            }

            depth_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            depth_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

            depth_swapchain_create_info.width = depth_desc.Width;
            depth_swapchain_create_info.height = depth_desc.Height;
        } else {
            this->made_depth_with_null_defaults = true;
            spdlog::error("[VR] Depth texture is null! Using default values");
            depth_desc.Width = vr->get_hmd_width() * 2;
            depth_desc.Height = vr->get_hmd_height();
        }

        if (!vr->is_using_afr()) {
            spdlog::info("[VR] Creating double wide depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH, depth_swapchain_create_info, depth_desc)) {
                return err;
            }
        } else {
            spdlog::info("[VR] Creating AFR depth swapchain");
            spdlog::info("[VR] Creating AFR left eye depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, depth_swapchain_create_info, depth_desc)) {
                return err;
            }

            spdlog::info("[VR] Creating AFR right eye depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE, depth_swapchain_create_info, depth_desc)) {
                return err;
            }
        }
    }

    this->last_resolution = {vr->get_hmd_width(), vr->get_hmd_height()};

    return std::nullopt;
}

void D3D12Component::OpenXR::destroy_swapchains() {
    std::scoped_lock _{this->mtx};

    if (this->contexts.empty()) {
        return;
    }
    
    auto& vr = VR::get();
    std::scoped_lock __{vr->m_openxr->swapchain_mtx};

    spdlog::info("[VR] Destroying swapchains.");

    this->wait_for_all_copies();

    for (auto& it : this->contexts) {
        auto& ctx = it.second;
        const auto i = it.first;

        //ctx.texture_contexts.clear();
        for (auto& texture_context : ctx.texture_contexts) {
            if (texture_context != nullptr) {
                texture_context->reset();
            }
        }

        ctx.texture_contexts.clear();

        std::vector<ID3D12Resource*> needs_release{};

        for (auto& tex : ctx.textures) {
            if (tex.texture != nullptr) {
                tex.texture->AddRef();
                needs_release.push_back(tex.texture);
            }
        }

        if (vr->m_openxr->swapchains.contains(i)) {
            const auto result = xrDestroySwapchain(vr->m_openxr->swapchains[i].handle);

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] Failed to destroy swapchain {}.", i);
            } else {
                spdlog::info("[VR] Destroyed swapchain {}.", i);
            }
        } else {
            spdlog::error("[VR] Swapchain {} does not exist.", i);
        }

        for (auto& tex : needs_release) {
            if (const auto ref_count = tex->Release(); ref_count != 0) {
                spdlog::info("[VR] Memory leak detected in swapchain texture {} ({} refs)", i, ref_count);
            } else {
                spdlog::info("[VR] Swapchain texture {} released.", i);
            }
        }
        
        ctx.textures.clear();
    }

    this->contexts.clear();
    vr->m_openxr->swapchains.clear();
}

void D3D12Component::OpenXR::copy(
    uint32_t swapchain_idx, 
    ID3D12Resource* resource, 
    std::optional<std::function<void(d3d12::CommandContext&, ID3D12Resource*)>> pre_commands, 
    std::optional<std::function<void(d3d12::CommandContext&)>> additional_commands, 
    D3D12_RESOURCE_STATES src_state, 
    D3D12_BOX* src_box) 
{
    std::scoped_lock _{this->mtx};

    auto vr = VR::get();

    if (vr->m_openxr->frame_state.shouldRender != XR_TRUE) {
        return;
    }

    if (!vr->m_openxr->frame_began) {
        if (vr->get_synchronize_stage() != VR::SynchronizeStage::VERY_LATE) {
            spdlog::error("[VR] OpenXR: Frame not begun when trying to copy.");
            return;
        }
    }

    if (!this->contexts.contains(swapchain_idx)) {
        spdlog::error("[VR] OpenXR: Trying to copy to swapchain {} but it doesn't exist.", swapchain_idx);
        return;
    }

    if (!vr->m_openxr->swapchains.contains(swapchain_idx)) {
        spdlog::error("[VR] OpenXR: Trying to copy to swapchain {} but it doesn't exist.", swapchain_idx);
        return;
    }

    if (this->contexts[swapchain_idx].num_textures_acquired > 0) {
        spdlog::info("[VR] Already acquired textures for swapchain {}?", swapchain_idx);
    }

    const auto& swapchain = vr->m_openxr->swapchains[swapchain_idx];
    auto& ctx = this->contexts[swapchain_idx];

    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

    uint32_t texture_index{};
    auto result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);

    if (result == XR_ERROR_RUNTIME_FAILURE) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        spdlog::info("[VR] Attempting to correct...");

        for (auto& texture_ctx : ctx.texture_contexts) {
            texture_ctx->commands.reset();
        }

        texture_index = 0;
        result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);
    }


    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
    } else {
        ctx.num_textures_acquired++;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        //wait_info.timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)).count();
        wait_info.timeout = XR_INFINITE_DURATION;
        result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        } else {
            auto& texture_ctx = ctx.texture_contexts[texture_index];
            texture_ctx->commands.wait(INFINITE);

            if (pre_commands) {
                (*pre_commands)(texture_ctx->commands, ctx.textures[texture_index].texture);
            }

            // We may simply just want to render to the render target directly
            // hence, a null resource is allowed.
            if (resource != nullptr) {
                if (src_box == nullptr) {
                    const auto is_depth = swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH || 
                                        swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE || 
                                        swapchain_idx == (uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE;
                    const auto dst_state = is_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;

                    texture_ctx->commands.copy(
                        resource, 
                        ctx.textures[texture_index].texture, 
                        src_state, 
                        dst_state);
                } else {
                    texture_ctx->commands.copy_region(
                        resource, 
                        ctx.textures[texture_index].texture, src_box,
                        src_state, 
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            }

            if (additional_commands) {
                (*additional_commands)(texture_ctx->commands);
            }

            texture_ctx->commands.execute();

            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            auto result = xrReleaseSwapchainImage(swapchain.handle, &release_info);

            // SteamVR shenanigans.
            if (result == XR_ERROR_RUNTIME_FAILURE) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                spdlog::info("[VR] Attempting to correct...");

                result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

                if (result != XR_SUCCESS) {
                    spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                }

                for (auto& texture_ctx : ctx.texture_contexts) {
                    texture_ctx->commands.wait(INFINITE);
                }

                result = xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                return;
            }

            ctx.num_textures_acquired--;
            ctx.ever_acquired = true;
        }
    }
}
} // namespace vrmod
