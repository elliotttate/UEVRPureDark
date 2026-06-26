#include <d3dcompiler.h>

#include <cstring>
#include <cstdlib>
#include <fstream>

#include <openvr.h>
#include <utility/String.hpp>
#include <utility/ScopeGuard.hpp>
#include <utility/Logging.hpp>

#include "Framework.hpp"
#include "../VR.hpp"

#include <../../directxtk12-src/Inc/ResourceUploadBatch.h>
#include <../../directxtk12-src/Inc/RenderTargetState.h>

#include "shaders/Compiled/alpha_luminance_sprite_ps_SpritePixelShader.inc"
#include "shaders/Compiled/alpha_luminance_sprite_ps_SpriteVertexShader.inc"
#include "shaders/Compiled/afw_debug_visualize_cs_DebugVisualizeCS.inc"
#include "shaders/Compiled/ue_velocity_combine_cs_VelocityCombineCS.inc"

#include "d3d12/DirectXTK.hpp"

#include "AFWFrameResourcesBridge.hpp"
#include "D3D12Component.hpp"

//#define AFR_DEPTH_TEMP_DISABLED

constexpr auto ENGINE_SRC_DEPTH = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr auto ENGINE_SRC_COLOR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

namespace {
struct AfwProviderResource {
    UEVR_FrameResourceView view{};
    ID3D12Resource* texture{nullptr};
};

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
    float source_scale{1.0f};
    uint32_t velocity_encoded{}; // 1 = velocity SRV is the ENCODED RGBA16_UNORM buffer (decode in shader)
    uint32_t write_3d{};         // 1 = also write the full 3D velocity to u1 (opt-in); 0 = skip (default)
    // this-eye clip -> OTHER-eye clip (CURRENT frame). Used to remove the inter-eye stereo disparity that AFR
    // bakes into the engine's per-object velocity (UE computes a moving object's velocity vs the previous frame,
    // which under AFR is the OTHER eye, so the value carries the disparity -> the warp throws moving objects to a
    // second position = the 2-position-per-eye flicker). Subtracting the disparity leaves true same-eye motion.
    float clip_to_other_clip[16]{};
    uint32_t correct_disparity{}; // 1 = subtract the inter-eye disparity from the per-object (encoded) velocity
    uint32_t write_depth_eye{};   // 1 = also write the resampled eye-res depth to u2 (so PDAFW warps with a
                                  // depth that matches the eye resolution instead of the smaller DRS source)
    uint32_t _pad[2]{};
};

static_assert(sizeof(AFWVelocityCombineConstants) == 56 * sizeof(uint32_t));

// Constant buffer for the AFW debug buffer visualizer (afw_debug_visualize_cs.fx). Layout must match
// the cbuffer in that shader (8 x 32-bit).
struct AFWDebugVizConstants {
    uint32_t mode{};        // 0 MV hue,1 depth,2 MV hue,3 vel X,4 vel Y,5 vel mag,6 valid,7 src Z,8 combined Z
    float scale{1.0f};
    uint32_t input_size[2]{};
    uint32_t output_size[2]{};
    uint32_t source_encoded{}; // 1 = source is encoded RGBA16_UNORM (decode V.z from B+A)
    uint32_t pad{};
};
static_assert(sizeof(AFWDebugVizConstants) == 8 * sizeof(uint32_t));

void afw_transition_resource(ID3D12GraphicsCommandList* command_list, ID3D12Resource* resource,
                             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (command_list == nullptr || resource == nullptr || before == after) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
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
    barrier.UAV.pResource = resource;
    command_list->ResourceBarrier(1, &barrier);
}

bool get_provider_resource(bool velocity, AfwProviderResource& out) {
    if (!uevr_afw_bridge::enabled() || !uevr_afw_bridge::available()) {
        return false;
    }

    const bool ok = velocity ? uevr_afw_bridge::get_latest_velocity(&out.view)
                             : uevr_afw_bridge::get_latest_depth(&out.view);
    if (!ok || out.view.d3d12_resource == nullptr) {
        return false;
    }

    out.texture = static_cast<ID3D12Resource*>(out.view.d3d12_resource);
    return true;
}

// PDAFW's CreateTexture remaps R32G8X24_TYPELESS(19) -> D32_FLOAT_S8X24_UINT(20) at creation, and its
// CreateSRV switch has no case for a depth-format resource, so a depth copy routed through CreateTexture
// gets an INVALID shader-resource view and AFW samples garbage depth. Pick a copy format that is BOTH
// CopyResource-compatible with the engine depth (same format family / element size) AND maps to a valid
// sampling view through PDAFW's CreateSRV switch (19->21, 44->46) or passes straight through (R32_FLOAT,
// R16_UNORM). This is what lets AFW actually read depth.
DXGI_FORMAT afw_depth_copy_format(DXGI_FORMAT src) {
    switch (src) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return DXGI_FORMAT_R32G8X24_TYPELESS; // CreateSRV: 19 -> R32_FLOAT_X8X24_TYPELESS (samples depth)
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return DXGI_FORMAT_R24G8_TYPELESS; // CreateSRV: 44 -> R24_UNORM_X8_TYPELESS
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT; // same R32 family; CreateSRV passes the concrete format through
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_R16_UNORM; // same R16 family
    default:
        return src;
    }
}

// Create the per-eye AFW depth copy ourselves (NOT via PDAFW CreateTexture) so the resource keeps a
// sampleable format. SetupTextureDesc then builds a valid depth-sampling SRV via PDAFW's CreateSRV.
void resize_afw_depth_inputs(VR* vr, TextureDesc (&dest)[2], ID3D12Resource* source) {
    if (vr == nullptr || vr->d3d12Renderer == nullptr || source == nullptr) {
        return;
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        return;
    }
    const auto src_desc = source->GetDesc();
    const DXGI_FORMAT fmt = afw_depth_copy_format(src_desc.Format);
    for (int i = 0; i < 2; ++i) {
        if (dest[i].pTexture != nullptr && dest[i].pTexture->GetDesc().Width == src_desc.Width &&
            dest[i].pTexture->GetDesc().Height == src_desc.Height && dest[i].pTexture->GetDesc().Format == fmt) {
            continue;
        }

        const int prev_srv = (dest[i].pTexture != nullptr) ? dest[i].srvPos : -1;
        if (dest[i].pTexture != nullptr) {
            dest[i].pTexture->Release();
        }

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = src_desc.Width;
        rd.Height = src_desc.Height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = D3D12_RESOURCE_FLAG_NONE; // plain texture: SRV + CopyResource dest, no DSV/UAV/deny

        ID3D12Resource* tex = nullptr;
        const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&tex));
        if (FAILED(hr) || tex == nullptr) {
            SPDLOG_ERROR_EVERY_N_SEC(1, "[AFWFrameResources] failed to create AFW depth copy {}x{} fmt={} hr=0x{:08x}",
                                     static_cast<uint32_t>(src_desc.Width), src_desc.Height,
                                     static_cast<uint32_t>(fmt), static_cast<uint32_t>(hr));
            dest[i] = TextureDesc{};
            continue;
        }
        tex->SetName(L"UEVR AFW depth");

        dest[i] = TextureDesc{};
        dest[i].type = Depth;
        dest[i].pTexture = tex;
        dest[i].srvPos = prev_srv; // reuse the descriptor slot across resizes
        dest[i].uavPos = -1;
        dest[i].initialState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        vr->d3d12Renderer->SetupTextureDesc(dest[i]); // valid depth-sampling SRV
    }
}

void resize_afw_inputs(VR* vr, TextureDesc (&dest)[2], ID3D12Resource* source, DXGI_FORMAT override_format = DXGI_FORMAT_UNKNOWN) {
    if (vr == nullptr || vr->d3d12Renderer == nullptr || source == nullptr) {
        return;
    }

    const auto desc = source->GetDesc();
    const auto format = override_format == DXGI_FORMAT_UNKNOWN ? desc.Format : override_format;
    for (int i = 0; i < 2; ++i) {
        if (dest[i].pTexture == nullptr || dest[i].pTexture->GetDesc().Width != desc.Width ||
            dest[i].pTexture->GetDesc().Height != desc.Height || dest[i].pTexture->GetDesc().Format != format) {
            vr->d3d12Renderer->CreateTexture(
                static_cast<int>(desc.Width), static_cast<int>(desc.Height), format,
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, dest[i], true);
            // Name it "UEVR" so the provider's uevr_owned guard excludes our own combine-output buffer
            // from velocity re-discovery — otherwise it can be re-latched as the combine's input and the
            // combine reads its own previous frame (feedback loop -> drift/smear). Parity with the depth
            // copy + debug texture, which are already named.
            if (dest[i].pTexture != nullptr) {
                dest[i].pTexture->SetName(L"UEVR AFW motion vectors");
            }
        }
    }
}

void copy_provider_resource(VR* vr, ID3D12GraphicsCommandList* cmd_list, TextureDesc& dest, ID3D12Resource* source) {
    if (vr == nullptr || vr->d3d12Renderer == nullptr || cmd_list == nullptr || dest.pTexture == nullptr || source == nullptr) {
        return;
    }

    TextureDesc src{};
    src.pTexture = source;
    vr->d3d12Renderer->SetupTextureDesc(src);
    vr->d3d12Renderer->Copy(cmd_list, dest, src);
}

bool provider_view_shape_changed(UEVR_FrameResourceView& previous, const UEVR_FrameResourceView& current) {
    const bool changed = previous.width != current.width ||
        previous.height != current.height ||
        previous.format != current.format ||
        previous.provider != current.provider ||
        previous.motion_scale_x != current.motion_scale_x ||
        previous.motion_scale_y != current.motion_scale_y;

    if (changed) {
        previous = current;
    }

    return changed;
}

void log_provider_copy(bool velocity, const UEVR_FrameResourceView& view) {
    static UEVR_FrameResourceView s_last_depth_copy{};
    static UEVR_FrameResourceView s_last_velocity_copy{};
    auto& previous = velocity ? s_last_velocity_copy : s_last_depth_copy;

    if (!provider_view_shape_changed(previous, view)) {
        return;
    }

    if (velocity) {
        SPDLOG_INFO("[AFWFrameResources] AFW copied provider velocity {}x{} fmt={} provider={} scale=({}, {}) frame={}",
                    view.width, view.height, view.format, view.provider,
                    view.motion_scale_x, view.motion_scale_y, view.render_frame);
    } else {
        SPDLOG_INFO("[AFWFrameResources] AFW copied provider depth {}x{} fmt={} provider={} frame={}",
                    view.width, view.height, view.format, view.provider, view.render_frame);
    }
}
}

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

    const auto bb_desc = backbuffer->GetDesc();
    const auto eye_width = static_cast<uint32_t>(bb_desc.Width / 2);
    const auto eye_height = static_cast<uint32_t>(bb_desc.Height);

    AfwProviderResource provider_depth{};
    AfwProviderResource provider_velocity{};
    const bool provider_depth_valid = get_provider_resource(false, provider_depth);
    const bool provider_velocity_valid = uevr_afw_bridge::use_velocity() && get_provider_resource(true, provider_velocity);
    bool using_provider_depth = false;
    bool using_provider_velocity = false;

    // Only accept eye-resolution provider buffers. The engine also renders a smaller desktop-spectator
    // view (e.g. 1004x628) at a different FOV/aspect; feeding that to AFW (which warps the per-eye
    // 1680x1760 image) misregisters the depth / motion-vector field. Require at least the eye size.
    const auto provider_matches_eye = [&](const AfwProviderResource& r) {
        if (eye_width == 0 || eye_height == 0) {
            return true;
        }
        const uint32_t w = r.view.width, h = r.view.height;
        if (w == 0 || h == 0) {
            return false;
        }
        // Match the eye by ASPECT RATIO within a sane size band, NOT by ">= eye". Two failure modes the
        // old ">= eye" check caused on DRS/TSR titles (e.g. Black Myth: Wukong, which renders the scene at
        // ~67% then upscales the eye color):
        //   1) it REJECTED the real scene depth/velocity (1124x1176, a proportional downscale of the eye)
        //      -> AFW never consumed our provider buffers.
        //   2) it ACCEPTED anything larger than the eye, including a 10240x2048 virtual-shadow-map atlas
        //      that the depth scorer occasionally hands us -> copying that as depth FAULTS the device and
        //      crashes the game on the gameplay transition.
        // The combine resamples a smaller source into the eye-sized output via its ScaledPixel map, so a
        // proportional sub-eye buffer is correct to accept; an off-aspect/oversized buffer never is.
        const double eye_aspect = static_cast<double>(eye_width) / static_cast<double>(eye_height);
        const double buf_aspect = static_cast<double>(w) / static_cast<double>(h);
        const double aspect_err = (buf_aspect > eye_aspect ? buf_aspect - eye_aspect : eye_aspect - buf_aspect);
        const bool aspect_ok = aspect_err <= 0.10 * eye_aspect; // within 10% of the eye aspect ratio
        const double fx = static_cast<double>(w) / static_cast<double>(eye_width);
        const double fy = static_cast<double>(h) / static_cast<double>(eye_height);
        const bool size_ok = fx >= 0.40 && fy >= 0.40 && fx <= 2.0 && fy <= 2.0; // 40%..200% of eye
        const bool ok = aspect_ok && size_ok;
        if (!ok) {
            SPDLOG_INFO_EVERY_N_SEC(3, "[AFWFrameResources] rejecting non-eye-shaped provider buffer {}x{} (eye {}x{}, aspectErr={:.3f} fx={:.2f} fy={:.2f})",
                                    w, h, eye_width, eye_height, aspect_err, fx, fy);
        }
        return ok;
    };

    // motionVectorsDesc is R16G16_FLOAT and PDAFW's Copy is a raw CopyResource, which requires the source
    // to be the same 32-bit format family. The engine emits the eye-resolution velocity as BOTH
    // R16G16_FLOAT (raw float vectors — what AFW + the debug shader expect) and R16G16B16A16_UNORM
    // (64-bit encoded — incompatible, copies to garbage), alternating frame to frame. Accept only the
    // RG16F variant so the motion-vector buffer (and every velocity debug mode) stays valid + consistent.
    const auto provider_velocity_format_ok = [&](const AfwProviderResource& r) {
        const auto fmt = static_cast<DXGI_FORMAT>(r.view.format);
        // Accept R16G16_FLOAT (decoded, used directly) AND R16G16B16A16_UNORM (encoded — decoded by the combine shader).
        // The provider now serves the FIRST-per-frame LIVE-WINDOW SNAPSHOT of the velocity (the real encoded SceneVelocity
        // captured by the plugin's ResourceBarrier hook right after the velocity pass), so the RGBA16 it hands us is the
        // authoritative buffer, not the wrong dense post-process one.
        const bool ok = fmt == DXGI_FORMAT_R16G16_FLOAT || fmt == DXGI_FORMAT_R16G16B16A16_UNORM;
        if (!ok) {
            SPDLOG_INFO_EVERY_N_SEC(3, "[AFWFrameResources] skipping incompatible velocity fmt={} (need R16G16_FLOAT or R16G16B16A16_UNORM)",
                                    r.view.format);
        }
        return ok;
    };

    if (uevr_afw_bridge::enabled() && !uevr_afw_bridge::available()) {
        SPDLOG_WARN_ONCE("[AFWFrameResources] bridge enabled but provider API unavailable: {}",
                         uevr_afw_bridge::describe_state());
    }

    if (!vr->rawDepthTex && provider_depth_valid && provider_matches_eye(provider_depth)) {
        vr->rawDepthTex = provider_depth.texture;
        using_provider_depth = true;
    }

    // SVE override (Path A, step 1): use the engine's REAL encoded SceneVelocity, resolved to its ID3D12Resource
    // DURING Execute (the getter returns a value cached then — it never touches the freed FRHITexture at present, which
    // is what crashed before). This bypasses the RG16F-only provider filter (this IS the authoritative encoded buffer).
    // If the transient ID3D12Resource is dangling/aliased by present, this test will crash or dump garbage -> diagnose.
    bool sve_active = false;
    if (auto* sve_native = reinterpret_cast<ID3D12Resource*>(FFakeStereoRenderingHook::get_afw_sve_velocity_resource())) {
        provider_velocity.texture = sve_native;
        provider_velocity.view.width = (eye_width != 0) ? eye_width : 1680;
        provider_velocity.view.height = (eye_height != 0) ? eye_height : 1760;
        provider_velocity.view.format = static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_UNORM);
        provider_velocity.view.expected_state = static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON);
        sve_active = true;
        SPDLOG_INFO_ONCE("[AFW][SVE] combine using EXPLICIT SceneVelocity resource={:x}", reinterpret_cast<uintptr_t>(sve_native));
    }

    if (!vr->rawMotionVectorsTex && provider_matches_eye(provider_velocity) &&
        (sve_active || (provider_velocity_valid && provider_velocity_format_ok(provider_velocity)))) {
        vr->rawMotionVectorsTex = provider_velocity.texture;
        using_provider_velocity = true;
    }

    if (!vr->rawDepthTex && (!uevr_afw_bridge::enabled() || uevr_afw_bridge::legacy_fallback())) {
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
    if ((is_using_afw) && (!eyeFrameBuffer.color.pTexture || !otherEyeFrameBuffer.color.pTexture))
        force_reset();

    // Eye color can be null on early/reset frames (force_reset above doesn't refresh this local
    // copy). The AFW work below is already guarded on eyeFrameBuffer.color.pTexture; bail before any
    // unconditional deref so a not-yet-ready eye buffer can't fault the present/RHI thread.
    if (is_using_afw && !eyeFrameBuffer.color.pTexture) {
        return vr::VRCompositorError_None;
    }

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

    if (vr->rawDepthTex) {
        auto* raw_depth = vr->rawDepthTex;
        vr->rawDepthTex = NULL;
        resize_afw_depth_inputs(vr, vr->depthDesc, raw_depth);
    }
    if (vr->rawMotionVectorsTex) {
        auto* raw_motion_vectors = vr->rawMotionVectorsTex;
        vr->rawMotionVectorsTex = NULL;
        // motionVectorsDesc = the combine OUTPUT (dense camera-motion vectors AFW warps with), per-eye
        // R16G16_FLOAT. CRITICAL: size it to the AFW EYE color, NOT the engine velocity's render rect.
        // The eye has a guard band wider than the engine render; sizing the MV to the velocity left the
        // warp covering only that sub-rectangle of the eye -> a hard-edged un-warped band + ghosting.
        // The combine samples the smaller velocity/depth into this eye-sized output via its ScaledPixel map.
        ID3D12Resource* mv_size_src = eyeFrameBuffer.color.pTexture != nullptr
            ? eyeFrameBuffer.color.pTexture : raw_motion_vectors;
        if (mv_size_src != nullptr) {
            resize_afw_inputs(vr, vr->motionVectorsDesc, mv_size_src, DXGI_FORMAT_R16G16_FLOAT);
        }
    }

    // One-shot gate diagnostic: report exactly why the AFW combine does or doesn't run. The combine
    // block below requires is_using_afw && eye color && depthDesc; the copies require using_provider_*.
    SPDLOG_INFO_EVERY_N_SEC(2,
        "[AFWFrameResources] AFW gate afw={} eyeColor={} depthDesc={} mvDesc={} | pDepth(valid={} {}x{}) "
        "pVel(valid={} {}x{} fmt={}) usingDepth={} usingVel={}",
        is_using_afw, eyeFrameBuffer.color.pTexture != nullptr,
        vr->depthDesc[nEye].pTexture != nullptr, vr->motionVectorsDesc[nEye].pTexture != nullptr,
        provider_depth_valid, provider_depth.view.width, provider_depth.view.height,
        provider_velocity_valid, provider_velocity.view.width, provider_velocity.view.height,
        provider_velocity.view.format, using_provider_depth, using_provider_velocity);

    auto cmdList = vr->d3d12Renderer->BeginCommandList(backbuffer_index);
    // if (is_using_afw && is_right_eye_frame) {
    //     FLOAT red[4] = {1, 0, 0, 0};
    //     vr->d3d12Renderer->Clear(cmdList, backbufferDesc[backbuffer_index], red);
    // }

    if (is_using_afw && eyeFrameBuffer.color.pTexture && vr->depthDesc[nEye].pTexture) {
        static FrameBufferDesc s_CurrentEyeFrameBuffer{};

        D3D12_BOX src_box{.left = 0,
            .top = 0,
            .front = 0,
            .right = vr->is_extreme_compatibility_mode_enabled() ? m_backbuffer_size[0] : m_backbuffer_size[0] / 2,
            .bottom = m_backbuffer_size[1],
            .back = 1};

        vr->d3d12Renderer->Crop(cmdList, eyeFrameBuffer.color, backbufferDesc[backbuffer_index], src_box);
        FLOAT black[4] = {0, 0, 0, 0};
        //if (vr->mDebug1) {
        //    vr->d3d12Renderer->Clear(cmdList, vr->motionVectorsDesc[nEye], black);
        //}

        // A/B compare reference: when ON, the NGX hook already copied DLSS's ground-truth depth + MV into
        // depthDesc/motionVectorsDesc this frame. Skip our provider copies + combine so that reference
        // survives to the warp AND the debug view (otherwise our reconstruction would overwrite it). Flip the
        // menu toggle "Compare: Use DLSS Depth/MV" live to switch between DLSS's buffers and ours.
        const bool use_dlss_source = vr->afw_use_dlss_source();
        if (using_provider_depth && !use_dlss_source) {
            copy_provider_resource(vr, cmdList, vr->depthDesc[nEye], provider_depth.texture);
            log_provider_copy(false, provider_depth.view);
        }
        bool combined_velocity = false;
        if (using_provider_velocity && !use_dlss_source && vr->motionVectorsDesc[nEye].pTexture != nullptr) {
            log_provider_copy(true, provider_velocity.view);
            // Wrap the engine's raw (sparse) velocity directly as the combine's input SRV (no copy).
            auto& raw_vel = m_raw_velocity_desc[nEye];
            if (raw_vel.pTexture != provider_velocity.texture || raw_vel.shaderResourceViewHandle.ptr == 0) {
                raw_vel = TextureDesc{};
                raw_vel.type = Image;
                raw_vel.pTexture = provider_velocity.texture;
                raw_vel.initialState = static_cast<D3D12_RESOURCE_STATES>(provider_velocity.view.expected_state);
                if (raw_vel.initialState == D3D12_RESOURCE_STATE_COMMON) {
                    raw_vel.initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
                }
                vr->d3d12Renderer->SetupTextureDesc(raw_vel);
            }
            // Reconstruct dense camera/head-motion velocity from the depth buffer into motionVectorsDesc
            // (the engine velocity is sparse without DLSS). This is what makes the warp + every velocity
            // debug mode respond to head movement.
            if (using_provider_depth && vr->depthDesc[nEye].pTexture != nullptr && raw_vel.pTexture != nullptr) {
                combined_velocity = combine_ue_velocity_for_afw(vr, cmdList, raw_vel,
                                                                vr->depthDesc[nEye], vr->motionVectorsDesc[nEye], nEye);
            }
        }

        s_CurrentEyeFrameBuffer.color = eyeFrameBuffer.color;
        // Prefer the combine's eye-resolution depth (u2) so PDAFW warps with a depth that matches its eye
        // color + MV; fall back to the raw DRS-res provider depth if the combine didn't run this frame.
        s_CurrentEyeFrameBuffer.depth = (combined_velocity && m_afw_depth_eye_desc[nEye].pTexture != nullptr)
            ? m_afw_depth_eye_desc[nEye]
            : vr->depthDesc[nEye];
        s_CurrentEyeFrameBuffer.motionVectors = vr->motionVectorsDesc[nEye];
        //if (vr->mDebug2) {
        //    vr->d3d12Renderer->Clear(cmdList, eyeFrameBuffer.depth, black);
        //    s_CurrentEyeFrameBuffer.depth = eyeFrameBuffer.depth;
        //}

        params.InCmdList = cmdList;
        params.InEyeFrameBuffer = &s_CurrentEyeFrameBuffer;
        params.InUIColorAlpha = NULL;
        params.IsHudlessColor = true;
        // The combined velocity is dense per-eye motion in the output's own pixel space, so AFW consumes
        // it as Normal. Fall back to FromOtherEye when we couldn't reconstruct it. The warp strength is
        // env-tunable (UEVR_AFW_MV_SCALE, default 1.0) so the reprojection amount can be dialed to remove
        // over-warp ghosting without rebuilding (AFW alternates eyes, so the per-eye 2-frame ClipToPrevClip
        // can over-shift; ~0.5 compensates if the warp interval is a single frame).
        // Env override (UEVR_AFW_MV_SCALE) wins for headless tuning; otherwise the live menu slider.
        static const float s_afw_mv_scale_env = []() {
            if (const char* e = std::getenv("UEVR_AFW_MV_SCALE")) {
                const float v = static_cast<float>(std::atof(e));
                if (v >= 0.0f) return v;
            }
            return -1.0f; // sentinel: not set -> use the menu slider
        }();
        const float afw_mv_scale = (s_afw_mv_scale_env >= 0.0f) ? s_afw_mv_scale_env
                                                                 : vr->m_afw_mv_scale->value();
        params.MotionVectorsType = (vr->is_fix_dlss() || combined_velocity) ? Normal : FromOtherEye;
        params.InMotionScale[0] = combined_velocity ? afw_mv_scale : vr->mvScale[0];
        params.InMotionScale[1] = combined_velocity ? afw_mv_scale : vr->mvScale[1];
        SPDLOG_INFO_EVERY_N_SEC(2, "[VR] AFW params eye={} mvType={} combinedVel={} mvScale=({:.4f},{:.4f})",
                                static_cast<int>(nEye),
                                params.MotionVectorsType == Normal ? "Normal" : "FromOtherEye",
                                combined_velocity ? 1 : 0, params.InMotionScale[0], params.InMotionScale[1]);
        params.Mode = (FrameWarpMode)vr->m_framewarp_mode->value();
        params.EyeIndex = nEye;
        params.ClearBeforeWarping = vr->m_clear_before_framewarp->value();
        params.CameraData = &vr->cameraData[nEye];
        params.IgnoreMotionThreshold = vr->m_ignore_motion_threshold->value();
        params.Debug = vr->m_framewarp_debug->value();

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

        // AFW debug buffer visualizer (Unreal menu -> Alternate Frame Warping -> Debug Buffer View).
        // False-colors the depth / motion vectors AFW consumes and blits over the submitted eye.
        if (const int afw_debug_view = vr->m_afw_debug_view->value(); afw_debug_view != 0) {
            // Pin to a single eye so the view doesn't flicker between L/R parallax as AFW alternates the
            // freshly-rendered eye; the same eye image is shown in both eyes.
            render_debug_view(vr, cmdList, afw_debug_view, EyeLeft, backbufferDesc[backbuffer_index],
                              &otherEyeFrameBuffer.color);
        }

        D3D12_VIEWPORT vp{
            .TopLeftX = 0, .TopLeftY = 0, .Width = (float)src_box.right, .Height = (float)src_box.bottom, .MinDepth = 0, .MaxDepth = 1};
        // vr->d3d12Renderer->Blit(cmdList, backbufferDesc[backbuffer_index], otherEyeFrameBuffer.color, vp);
    }

    vr->d3d12Renderer->EndCommandList(backbuffer_index);

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
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, otherEyeFrameBuffer.color.pTexture, D3D12_RESOURCE_STATE_RENDER_TARGET, &src_box);
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
                    D3D12_BOX src_box2{.left = 0,
                        .top = 0,
                        .front = 0,
                        .right = vr->is_extreme_compatibility_mode_enabled() ? m_backbuffer_size[0] : m_backbuffer_size[0] / 2,
                        .bottom = m_backbuffer_size[1],
                        .back = 1};
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, otherEyeFrameBuffer.color.pTexture, D3D12_RESOURCE_STATE_RENDER_TARGET, &src_box2);
                }
            } else {
                // Copy over the entire double wide instead
                if (m_scene_capture_tex.texture.Get() == nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
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

    D3D12_DESCRIPTOR_RANGE output_uav_3d_range{};
    output_uav_3d_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    output_uav_3d_range.NumDescriptors = 1;
    output_uav_3d_range.BaseShaderRegister = 1; // register u1 (full 3D velocity)
    output_uav_3d_range.RegisterSpace = 0;
    output_uav_3d_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE output_uav_depth_range{};
    output_uav_depth_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    output_uav_depth_range.NumDescriptors = 1;
    output_uav_depth_range.BaseShaderRegister = 2; // register u2 (resampled eye-res depth for PDAFW)
    output_uav_depth_range.RegisterSpace = 0;
    output_uav_depth_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER root_parameters[6]{};
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

    root_parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[4].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[4].DescriptorTable.pDescriptorRanges = &output_uav_3d_range;
    root_parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    root_parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[5].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[5].DescriptorTable.pDescriptorRanges = &output_uav_depth_range;
    root_parameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

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

// One-shot debug readback of a GPU texture to disk (header: width,height,rowPitch,format; then raw bytes)
// so the actual velocity/depth bytes can be analyzed offline (settles encoded-vs-raw definitively).
// Self-contained: own allocator/list/fence on the swapchain queue. Gated + dump-once by the caller.
static void afw_dump_texture_to_disk(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* tex,
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

bool D3D12Component::combine_ue_velocity_for_afw(
    VR* vr,
    ID3D12GraphicsCommandList* command_list,
    TextureDesc& raw_velocity_desc,
    TextureDesc& depth_desc,
    TextureDesc& output_desc,
    EyeIndex eye) {
    if (vr == nullptr || vr->d3d12Renderer == nullptr || command_list == nullptr ||
        raw_velocity_desc.pTexture == nullptr || depth_desc.pTexture == nullptr ||
        output_desc.pTexture == nullptr) {
        return false;
    }

    auto device = g_framework->get_d3d12_hook()->get_device();
    if (!setup_velocity_combine_pipeline(device)) {
        return false;
    }

    // Per-eye full-3D velocity target (RGBA16F), the combine's SECOND output (u1). .xy = combined screen motion,
    // .z = depth motion V.z. Kept separate from motionVectorsDesc (which stays RG16F so PDAFW's raw CopyResource
    // is valid). PureDark's 3D reprojection + the "combined Z" debug view read this.
    // OPT-IN (default OFF via UEVR_AFW_3D_VELOCITY): when off, the combine is byte-identical to pre-3D behavior
    // (no 2nd UAV allocated -> nothing extra in PDAFW's shared descriptor heap; the shader skips the u1 write).
    static const bool s_enable_3d = []() {
        const char* e = std::getenv("UEVR_AFW_3D_VELOCITY");
        return e != nullptr && (e[0] == '1' || e[0] == 't' || e[0] == 'T' || e[0] == 'y' || e[0] == 'Y');
    }();
    auto& output_3d_desc = m_combined_velocity_3d_desc[eye];
    if (s_enable_3d) {
        const auto od = output_desc.pTexture->GetDesc();
        if (output_3d_desc.pTexture == nullptr ||
            output_3d_desc.pTexture->GetDesc().Width != od.Width ||
            output_3d_desc.pTexture->GetDesc().Height != od.Height) {
            if (!vr->d3d12Renderer->CreateTexture(static_cast<int>(od.Width), static_cast<int>(od.Height),
                    DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, output_3d_desc, true)) {
                SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] AFW failed to create 3D velocity target");
                return false;
            }
            output_3d_desc.pTexture->SetName(L"UEVR AFW combined velocity 3D");
            output_3d_desc.unorderedAccessViewHandle.ptr = 0;
            output_3d_desc.uavPos = -1;
        }
        if (output_3d_desc.unorderedAccessViewHandle.ptr == 0 || output_3d_desc.uavPos < 0) {
            const int uav_pos = vr->d3d12Renderer->CreateUAV(output_3d_desc.pTexture);
            if (uav_pos < 0) {
                SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] AFW failed to create 3D velocity UAV");
                return false;
            }
            output_3d_desc.uavPos = uav_pos;
            output_3d_desc.unorderedAccessViewHandle = vr->d3d12Renderer->GetGPUDescriptorHandle(uav_pos);
        }
    }

    // Eye-resolution resampled depth (u2): the provider depth is the game's DRS render-scale resolution
    // (e.g. 1124x1176) while PDAFW warps the eye at full resolution (e.g. 1680x1760). Hand PDAFW the raw
    // DRS depth and its depth-driven reprojection mis-samples (the "broken warp" — RenderDoc confirmed our
    // depth was 1124x1176 D32S8 vs the eye's 1680x1760). The combine already resamples the depth per OUTPUT
    // pixel (ScaledPixel + 3x3 dilation) for the MV, so it also writes that eye-res depth to u2 (R32_FLOAT,
    // matching PDAFW's m_*EyeDesc.depth). Default ON; UEVR_AFW_DEPTH_EYE_RESCALE=0 reverts to the raw depth.
    static const bool s_enable_depth_eye = []() {
        const char* e = std::getenv("UEVR_AFW_DEPTH_EYE_RESCALE");
        return e == nullptr || !(e[0] == '0' || e[0] == 'f' || e[0] == 'F' || e[0] == 'n' || e[0] == 'N');
    }();
    auto& depth_eye_desc = m_afw_depth_eye_desc[eye];
    if (s_enable_depth_eye) {
        const auto od = output_desc.pTexture->GetDesc();
        if (depth_eye_desc.pTexture == nullptr ||
            depth_eye_desc.pTexture->GetDesc().Width != od.Width ||
            depth_eye_desc.pTexture->GetDesc().Height != od.Height) {
            if (!vr->d3d12Renderer->CreateTexture(static_cast<int>(od.Width), static_cast<int>(od.Height),
                    DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, depth_eye_desc, true)) {
                SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] AFW failed to create eye-res depth target");
                return false;
            }
            depth_eye_desc.pTexture->SetName(L"UEVR AFW depth eye-res");
            depth_eye_desc.unorderedAccessViewHandle.ptr = 0;
            depth_eye_desc.uavPos = -1;
        }
        if (depth_eye_desc.unorderedAccessViewHandle.ptr == 0 || depth_eye_desc.uavPos < 0) {
            const int uav_pos = vr->d3d12Renderer->CreateUAV(depth_eye_desc.pTexture);
            if (uav_pos < 0) {
                SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] AFW failed to create eye-res depth UAV");
                return false;
            }
            depth_eye_desc.uavPos = uav_pos;
            depth_eye_desc.unorderedAccessViewHandle = vr->d3d12Renderer->GetGPUDescriptorHandle(uav_pos);
        }
    }

    // DIAGNOSTIC: ON-DEMAND dump of the ACTUAL velocity/depth/combined bytes. Gated by env
    // UEVR_AFW_VELDUMP=1. Create the trigger file afw_work\DUMP_NOW (e.g. WHILE driving the pawn so the
    // buffers hold a motion frame) and the next combine call dumps raw_velocity_<N>.bin /
    // combined_velocity_<N>.bin / depth_<N>.bin and removes the trigger. Numbered so a static baseline
    // dump and several motion dumps don't overwrite each other. This is the headless equivalent of a
    // RenderDoc capture for the AFW buffers (which RenderDoc can't drive on this game).
    if (std::getenv("UEVR_AFW_VELDUMP") != nullptr) {
        static const char* const s_trigger = "E:\\Github\\UEVRPureDark\\afw_work\\DUMP_NOW";
        if (GetFileAttributesA(s_trigger) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA(s_trigger);
            static int s_dump_idx = 0;
            const int n = s_dump_idx++;
            auto* queue = g_framework->get_d3d12_hook()->get_command_queue();
            char path[300];
            std::snprintf(path, sizeof(path), "E:\\Github\\UEVRPureDark\\afw_work\\raw_velocity_%d.bin", n);
            afw_dump_texture_to_disk(device, queue, raw_velocity_desc.pTexture, raw_velocity_desc.initialState, path);
            std::snprintf(path, sizeof(path), "E:\\Github\\UEVRPureDark\\afw_work\\combined_velocity_%d.bin", n);
            afw_dump_texture_to_disk(device, queue, output_desc.pTexture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, path);
            if (output_3d_desc.pTexture != nullptr) {
                std::snprintf(path, sizeof(path), "E:\\Github\\UEVRPureDark\\afw_work\\combined_velocity_3d_%d.bin", n);
                afw_dump_texture_to_disk(device, queue, output_3d_desc.pTexture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, path);
            }
            std::snprintf(path, sizeof(path), "E:\\Github\\UEVRPureDark\\afw_work\\depth_%d.bin", n);
            afw_dump_texture_to_disk(device, queue, depth_desc.pTexture, depth_desc.initialState, path);
            // Also dump the DECODED engine MV that DLSS itself is fed (captured by the NGX hook this frame),
            // so we can compare DLSS's ground-truth MV against our combine output in the SAME frame.
            if (vr->afw_dlss_mv_capture != nullptr) {
                std::snprintf(path, sizeof(path), "E:\\Github\\UEVRPureDark\\afw_work\\dlss_mv_%d.bin", n);
                afw_dump_texture_to_disk(device, queue, vr->afw_dlss_mv_capture,
                                         D3D12_RESOURCE_STATE_COMMON, path);
                SPDLOG_INFO("[VR] AFW VELDUMP #{} dlss_mv {}x{} fmt={}", n,
                            static_cast<uint32_t>(vr->afw_dlss_mv_capture->GetDesc().Width),
                            vr->afw_dlss_mv_capture->GetDesc().Height,
                            static_cast<int>(vr->afw_dlss_mv_capture->GetDesc().Format));
            }
            SPDLOG_INFO("[VR] AFW VELDUMP #{} wrote raw_velocity_{}/combined_velocity_{}/depth_{} (eye={} {}x{})",
                        n, n, n, n, static_cast<int>(eye),
                        static_cast<uint32_t>(output_desc.pTexture->GetDesc().Width), output_desc.pTexture->GetDesc().Height);
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
    const auto velocity_resource_desc = raw_velocity_desc.pTexture->GetDesc();

    const auto& camera_data = vr->cameraData[eye];
    glm::mat4 clip_to_prev_clip{};

    {
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

        // Projection self-consistency trace (UEVR_AFW_PROJ_TRACE): the combine reprojects the engine depth
        // ASSUMING it was rendered with camera_data.srcViewToClipMatrix. If enabling ghosting shifts the depth
        // VALUES (which it measurably does, max 0.114->0.052) but this projection does NOT shift to match, the
        // warp uses a mismatched depth = a REAL bug that gentle motion hides but fast head motion exposes.
        // Compare fresh vs ghosting: near/far live in P22/P32, the FOV scale in P00/P11.
        if (std::getenv("UEVR_AFW_PROJ_TRACE") != nullptr) {
            static int s_pt[2]{0, 0};
            if ((s_pt[ei]++ % 60) == 0) {
                const auto& P = camera_data.srcViewToClipMatrix;
                const auto& V2W = camera_data.srcViewToWorldMatrix;
                // eyePos = per-eye world position (translation column of view->world). eye0 vs eye1 MUST differ
                // by the IPD (the stereo offset) — that disparity is what the AFW stereo reprojection warps by.
                // If ghosting collapses eye0/eye1 to the SAME position (or the same view orientation), the warp
                // has no per-eye disparity -> the warped eye DOUBLES (the symptom). Compare eye0-vs-eye1 ON vs
                // OFF, same controlled way as the depth dump. right = view X axis in world (the IPD direction).
                SPDLOG_INFO("[AFW projtrace] eye={} P00={:.5f} P11={:.5f} P32={:.6f} | eyePos=({:.4f},{:.4f},{:.4f}) right=({:.4f},{:.4f},{:.4f})",
                            (int)eye, P[0][0], P[1][1], P[3][2],
                            V2W[3][0], V2W[3][1], V2W[3][2],
                            V2W[0][0], V2W[0][1], V2W[0][2]);
            }
        }

        // ---- Reconstruction reliability diagnostic ----------------------------------------------
        // The depth-reconstruction fallback (which must fill the guard-band borders AND cover the case
        // where the engine velocity buffer goes dead) is only as good as clip_to_prev_clip. If that
        // collapses to identity, the reconstruction is zero. Log periodically so we can confirm, with the
        // user moving, whether (a) the camera is moving, (b) the reconstruction sees it, and (c) the engine
        // view-matrix history is usable. Gated on UEVR_AFW_RECON_DIAG to avoid log spam.
        if (std::getenv("UEVR_AFW_RECON_DIAG") != nullptr) {
            static int s_diag_count[2]{0, 0};
            static glm::vec3 s_last_cam[2]{};
            const glm::vec3 cam_pos = glm::vec3(camera_data.srcViewToWorldMatrix[3]);
            const float cam_delta = glm::length(cam_pos - s_last_cam[ei]);
            auto dev_from_identity = [](const glm::mat4& m) {
                float d = 0.0f;
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        d = std::max(d, std::abs(m[c][r] - ((c == r) ? 1.0f : 0.0f)));
                return d;
            };
            auto mat_diff = [](const glm::mat4& a, const glm::mat4& b) {
                float d = 0.0f;
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        d = std::max(d, std::abs(a[c][r] - b[c][r]));
                return d;
            };
            const auto& hist = vr->render_view_matrix[eye];
            const float h01 = mat_diff(hist[0].curr, hist[1].curr);
            const float h12 = mat_diff(hist[1].curr, hist[2].curr);
            if ((s_diag_count[ei]++ % 90) == 0 || cam_delta > 0.01f) {
                SPDLOG_INFO("[AFW recon] eye={} cam=({:.2f},{:.2f},{:.2f}) camDelta={:.4f} clipToPrev_dev={:.4f} "
                            "engineHist[0v1]={:.4f} [1v2]={:.4f} sprev={}",
                            (int)eye, cam_pos.x, cam_pos.y, cam_pos.z, cam_delta,
                            dev_from_identity(clip_to_prev_clip), h01, h12,
                            s_prev_valid[ei] ? "cached" : "first");
            }
            s_last_cam[ei] = cam_pos;
        }
    }

    // Inter-eye disparity reprojection (this eye -> OTHER eye, CURRENT frame), built from the OTHER eye's
    // matrices the same way clip_to_prev_clip is built from the prev frame. Used to subtract the stereo
    // disparity that AFR bakes into the engine per-object velocity (the 2-position-per-eye flicker on moving
    // objects). Same depth-driven reprojection math as reconVel, just across eyes instead of across time.
    glm::mat4 clip_to_other_clip{};
    {
        const int other_ei = (eye == static_cast<EyeIndex>(1)) ? 0 : 1;
        const auto& cam_other = vr->cameraData[other_ei];
        clip_to_other_clip =
            cam_other.srcViewToClipMatrix *
            cam_other.srcWorldToViewMatrix *
            camera_data.srcViewToWorldMatrix *
            camera_data.srcClipToViewMatrix;
    }

    // UE matrices vs glm/HLSL column-major can disagree on handedness; live menu toggle so the correct
    // convention can be confirmed against the warp without rebuilding (UEVRPureDark4 parity).
    if (vr->m_afw_combine_transpose->value()) {
        clip_to_prev_clip = glm::transpose(clip_to_prev_clip);
        clip_to_other_clip = glm::transpose(clip_to_other_clip);
    }

    AFWVelocityCombineConstants constants{};
    std::memcpy(constants.clip_to_prev_clip, &clip_to_prev_clip[0][0], sizeof(constants.clip_to_prev_clip));
    std::memcpy(constants.clip_to_other_clip, &clip_to_other_clip[0][0], sizeof(constants.clip_to_other_clip));
    constants.output_size[0] = static_cast<uint32_t>(output_resource_desc.Width);
    constants.output_size[1] = output_resource_desc.Height;
    constants.inv_output_size[0] = constants.output_size[0] > 0 ? 1.0f / static_cast<float>(constants.output_size[0]) : 0.0f;
    constants.inv_output_size[1] = constants.output_size[1] > 0 ? 1.0f / static_cast<float>(constants.output_size[1]) : 0.0f;
    constants.velocity_size[0] = static_cast<uint32_t>(velocity_resource_desc.Width);
    constants.velocity_size[1] = velocity_resource_desc.Height;
    constants.depth_size[0] = static_cast<uint32_t>(depth_resource_desc.Width);
    constants.depth_size[1] = depth_resource_desc.Height;
    constants.velocity_extent[0] = constants.velocity_size[0];
    constants.velocity_extent[1] = constants.velocity_size[1];
    constants.depth_extent[0] = constants.depth_size[0];
    constants.depth_extent[1] = constants.depth_size[1];
    // 0 = hybrid: use the engine's per-object velocity for moving objects, reconstruct camera motion for
    // static geometry (so the character reprojects onto itself AND head motion still warps the scene).
    // 1 = camera-only everywhere (objects ghost). Live toggle for comparison.
    // Ghosting-fix velocity path. We USED to force camera-only reconstruction (force_reconstruct=1) under
    // ghosting because the engine VELOCITY carried the OTHER eye's motion — but that was a SYMPTOM of the
    // ghosting fix forcing the 2nd eye to the PRIMARY projection (set_stereo_pass(eSSP_PRIMARY)). With that
    // pass swap now skipped under AFW (FFakeStereoRenderingHook keeps the real SECONDARY projection), the
    // engine renders the 2nd eye's velocity+depth under its OWN per-eye projection, so the per-object velocity
    // is correct again. Forcing reconstruction was actively HARMFUL: it filled the WHOLE motion-vector buffer
    // with dense camera-motion AND warped the MOVING character by camera motion -> displaced "ghost" copies at
    // the wrong screen positions (the blue debug overlay spilled everywhere instead of sitting on the
    // character). So under ghosting we now use the SAME per-object velocity path as the (working) non-ghosting
    // case: the warp's motion vectors land on moving objects only. Verified in the MetaXR sim that ghosting-ON
    // then matches ghosting-OFF. Set UEVR_AFW_GHOSTING_FORCE_RECONSTRUCT=1 to restore the old camera-only
    // safeguard for A/B testing. The VR_AFWForceReconstruct menu toggle still forces it in EITHER mode.
    static const bool s_ghosting_force_recon = []() {
        const char* e = std::getenv("UEVR_AFW_GHOSTING_FORCE_RECONSTRUCT");
        return e != nullptr && (e[0] == '1' || e[0] == 't' || e[0] == 'T' || e[0] == 'y' || e[0] == 'Y');
    }();
    const bool ghosting_force_recon =
        s_ghosting_force_recon && vr->is_ghosting_fix_enabled() && vr->is_using_afr();
    constants.force_reconstruct =
        (vr->m_afw_force_reconstruct->value() || ghosting_force_recon) ? 1u : 0u;
    // Remove the AFR inter-eye disparity from the per-object (encoded) velocity so moving objects stop
    // flickering between two positions per eye. Default ON; UEVR_AFW_DISPARITY_CORRECT=0 disables for A/B.
    static const bool s_correct_disparity = []() {
        const char* e = std::getenv("UEVR_AFW_DISPARITY_CORRECT");
        return e == nullptr || !(e[0] == '0' || e[0] == 'f' || e[0] == 'F' || e[0] == 'n' || e[0] == 'N');
    }();
    constants.correct_disparity = s_correct_disparity ? 1u : 0u;
    // Scale applied to the dense engine (source) velocity before AFW warps with it. The engine field is
    // dense + DLSS-like in direction, but its per-frame magnitude over-shoots what AFW's reprojection can
    // warp cleanly (smears/melts the previous frame). Tune live via UEVR_AFW_SRC_SCALE (default 0.5) to
    // dial the warp strength down without rebuilding; the depth-reconstruct fallback is left at 1.0.
    static const float s_source_scale = []() {
        if (const char* e = std::getenv("UEVR_AFW_SRC_SCALE")) {
            const float v = static_cast<float>(std::atof(e));
            if (v >= 0.0f) return v;
        }
        return 1.0f; // agreement-with-reconstruction test (in shader) does the filtering, not the scale
    }();
    constants.source_scale = s_source_scale;
    // 1 when the provider handed us the ENCODED velocity-depth buffer (PF_A16B16G16R16 == RGBA16_UNORM, the
    // exact input DLSS's VelocityCombine reads). The shader then decodes it with UE's DecodeVelocityFromTexture
    // and uses the `EncodedVelocity.x > 0` written flag (byte-identical to DLSS); the RG16F variant is used raw.
    constants.velocity_encoded =
        (velocity_resource_desc.Format == DXGI_FORMAT_R16G16B16A16_UNORM) ? 1u : 0u;
    constants.write_3d = s_enable_3d ? 1u : 0u;
    constants.write_depth_eye = s_enable_depth_eye ? 1u : 0u;

    if (constants.output_size[0] == 0 || constants.output_size[1] == 0 ||
        constants.velocity_size[0] == 0 || constants.velocity_size[1] == 0 ||
        constants.depth_size[0] == 0 || constants.depth_size[1] == 0) {
        return false;
    }

    // One-line size report so the depth/velocity/output dimensions + aspect ratios are visible in the
    // log (the combine reconstructs camera motion per OUTPUT pixel from depth sampled via a proportional
    // stretch; if output FOV/aspect != depth FOV/aspect the reconstruction is misregistered). Pairs with
    // a RenderDoc capture. aspect = W/H; a mismatch between output and depth/velocity is the size bug.
    SPDLOG_INFO_EVERY_N_SEC(2,
        "[VR] AFW combine sizes eye={} out(mv)={}x{} (a={:.3f}) depth={}x{} (a={:.3f}) vel={}x{} (a={:.3f}) force_reconstruct={}",
        static_cast<int>(eye),
        constants.output_size[0], constants.output_size[1],
        constants.output_size[1] ? static_cast<float>(constants.output_size[0]) / constants.output_size[1] : 0.0f,
        constants.depth_size[0], constants.depth_size[1],
        constants.depth_size[1] ? static_cast<float>(constants.depth_size[0]) / constants.depth_size[1] : 0.0f,
        constants.velocity_size[0], constants.velocity_size[1],
        constants.velocity_size[1] ? static_cast<float>(constants.velocity_size[0]) / constants.velocity_size[1] : 0.0f,
        constants.force_reconstruct);

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
    // u1: the real 3D target when enabled; else a dummy (u0's UAV). The shader skips the u1 write when Write3D==0.
    command_list->SetComputeRootDescriptorTable(4, s_enable_3d ? output_3d_desc.unorderedAccessViewHandle
                                                               : output_desc.unorderedAccessViewHandle);
    // u2: the eye-res depth target when enabled; else a dummy (u0's UAV). Shader skips u2 write when WriteDepthEye==0.
    command_list->SetComputeRootDescriptorTable(5, s_enable_depth_eye ? depth_eye_desc.unorderedAccessViewHandle
                                                                      : output_desc.unorderedAccessViewHandle);

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
    if (s_enable_3d) {
        afw_transition_resource(command_list, output_3d_desc.pTexture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (s_enable_depth_eye) {
        afw_transition_resource(command_list, depth_eye_desc.pTexture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    command_list->Dispatch((constants.output_size[0] + 7) / 8, (constants.output_size[1] + 7) / 8, 1);

    afw_uav_barrier(command_list, output_desc.pTexture);
    afw_transition_resource(command_list, output_desc.pTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    if (s_enable_3d) {
        afw_uav_barrier(command_list, output_3d_desc.pTexture);
        afw_transition_resource(command_list, output_3d_desc.pTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }
    if (s_enable_depth_eye) {
        afw_uav_barrier(command_list, depth_eye_desc.pTexture);
        afw_transition_resource(command_list, depth_eye_desc.pTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }
    if (transition_velocity) {
        afw_transition_resource(command_list, raw_velocity_desc.pTexture,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, velocity_state);
    }

    return true;
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

// Cycle-able buffer visualizer: false-colors the depth / motion-vectors that AFW consumes and blits
// the result over the submitted backbuffer eye so it shows in the HMD / spectator mirror. view_mode is
// the AFW Debug Buffer View dropdown index (0 = off).
void D3D12Component::render_debug_view(VR* vr, ID3D12GraphicsCommandList* command_list, int view_mode,
                                       EyeIndex eye, TextureDesc& backbuffer_dst, TextureDesc* other_eye_dst) {
    if (vr == nullptr || vr->d3d12Renderer == nullptr || command_list == nullptr || view_mode <= 0 ||
        backbuffer_dst.pTexture == nullptr) {
        return;
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (!setup_debug_view_pipeline(device)) {
        return;
    }

    TextureDesc* input = nullptr;
    uint32_t shader_mode = 0;
    float scale = 1.0f;
    switch (view_mode) {
    // Combined = the dense camera-motion velocity AFW actually warps with (reconstructed into
    // motionVectorsDesc). Source = the raw engine SceneVelocity (per-object motion only, no camera) that
    // the combine takes as input — visualized straight from m_raw_velocity_desc, matching UEVRPureDark4.
    case 1: input = &vr->motionVectorsDesc[eye]; shader_mode = 0; scale = 1.0f / 200.0f; break; // combined MV
    case 2: input = &vr->motionVectorsDesc[eye]; shader_mode = 0; scale = 1.0f / 40.0f;  break; // combined MV (boosted)
    case 3: input = &vr->depthDesc[eye];         shader_mode = 1; scale = 1.0f;          break; // depth
    // Source velocity is in DLSS-style [-1,1] normalized space (valid |v| < 1, garbage > 1), so a small
    // scale (~3) maps it to a readable gradient instead of saturating like the old x40/x80. Env-tunable.
    case 4: input = &m_raw_velocity_desc[eye];   shader_mode = 2; scale = 3.0f;          break; // per-object velocity direction
    case 5: input = &m_raw_velocity_desc[eye];   shader_mode = 3; scale = 3.0f;          break; // per-object velocity X
    case 6: input = &m_raw_velocity_desc[eye];   shader_mode = 4; scale = 3.0f;          break; // per-object velocity Y
    case 7: input = &m_raw_velocity_desc[eye];   shader_mode = 5; scale = 3.0f;          break; // per-object velocity magnitude
    case 8: input = &m_raw_velocity_desc[eye];   shader_mode = 6; scale = 1.0f;          break; // per-object velocity validity
    // Depth-motion V.z (the new 3D .z). Tiny device-Z deltas (~5e-4), so a large scale to make them readable.
    case 9:  input = &m_raw_velocity_desc[eye];         shader_mode = 7; scale = 2000.0f; break; // raw source velocity Z (depth motion)
    case 10: input = &m_combined_velocity_3d_desc[eye]; shader_mode = 8; scale = 2000.0f; break; // combined velocity Z (depth motion)
    default: return;
    }
    if (input == nullptr || input->pTexture == nullptr) {
        return; // e.g. combined-Z (mode 10) when the 3D buffer is disabled (UEVR_AFW_3D_VELOCITY off)
    }
    // Live magnitude knob for the velocity false-color (so the gradient can be dialed without rebuilds).
    if (view_mode != 3) {
        static const float s_dbg_scale_mul = []() {
            if (const char* e = std::getenv("UEVR_AFW_DEBUG_SCALE")) {
                const float v = static_cast<float>(std::atof(e));
                if (v > 0.0f) return v;
            }
            return 1.0f;
        }();
        scale *= s_dbg_scale_mul;
    }
    if (input == nullptr || input->pTexture == nullptr || input->shaderResourceViewHandle.ptr == 0) {
        return;
    }

    // Size the false-color target to ONE eye so it samples the eye-resolution buffer 1:1 (no stretch),
    // then blit the SAME eye image into both eyes. The backbuffer is double-wide (both eyes side by side);
    // other_eye_dst is the warped per-eye buffer. Filling both backbuffer halves + the warped eye keeps
    // both submitted eyes identical, so it does not flicker as AFW alternates which eye is freshly rendered.
    const auto in_desc = input->pTexture->GetDesc();
    const uint32_t input_w = static_cast<uint32_t>(in_desc.Width);
    const auto dst_desc = backbuffer_dst.pTexture->GetDesc();
    const uint32_t bb_w = static_cast<uint32_t>(dst_desc.Width);
    const uint32_t out_h = dst_desc.Height;
    // Double-wide if the backbuffer holds both eyes side by side (≈ 2x the per-eye buffer width).
    const bool double_wide = input_w > 0 && bb_w >= (input_w + input_w / 2u);
    const uint32_t eye_w = double_wide ? bb_w / 2u : bb_w;

    if (m_debug_view_tex.pTexture == nullptr ||
        m_debug_view_tex.pTexture->GetDesc().Width != eye_w || m_debug_view_tex.pTexture->GetDesc().Height != out_h) {
        if (!vr->d3d12Renderer->CreateTexture(static_cast<int>(eye_w), static_cast<int>(out_h),
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

    AFWDebugVizConstants c{};
    c.mode = shader_mode; c.scale = scale;
    c.input_size[0] = input_w; c.input_size[1] = in_desc.Height;
    c.output_size[0] = eye_w; c.output_size[1] = out_h;
    // Mode 7 (source Z) decodes V.z from the encoded RGBA16_UNORM source; flag whether the input is that format.
    c.source_encoded = (in_desc.Format == DXGI_FORMAT_R16G16B16A16_UNORM) ? 1u : 0u;
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

    // The per-object source-velocity modes (4-8) read m_raw_velocity_desc, which the combine leaves in
    // its RENDER_TARGET state — sampling an SRV from a non-shader-resource state is invalid (garbage /
    // debug-layer errors). Transition the input to a shader-readable state for the dispatch, then back.
    // motionVectorsDesc/depthDesc are already ALL_SHADER_RESOURCE, so they skip this.
    const auto input_state = input->initialState;
    const bool transition_input =
        (input_state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0 && input->pTexture != nullptr;
    if (transition_input) {
        afw_transition_resource(command_list, input->pTexture, input_state,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    afw_transition_resource(command_list, m_debug_view_tex.pTexture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    command_list->Dispatch((eye_w + 7) / 8, (out_h + 7) / 8, 1);
    afw_uav_barrier(command_list, m_debug_view_tex.pTexture);
    afw_transition_resource(command_list, m_debug_view_tex.pTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

    if (transition_input) {
        afw_transition_resource(command_list, input->pTexture,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, input_state);
    }

    // Left half of the double-wide backbuffer.
    D3D12_VIEWPORT vp_left{0, 0, static_cast<float>(eye_w), static_cast<float>(out_h), 0, 1};
    vr->d3d12Renderer->Blit(command_list, backbuffer_dst, m_debug_view_tex, vp_left);
    // Right half (only if the backbuffer holds both eyes side by side).
    if (double_wide) {
        D3D12_VIEWPORT vp_right{static_cast<float>(eye_w), 0, static_cast<float>(eye_w), static_cast<float>(out_h), 0, 1};
        vr->d3d12Renderer->Blit(command_list, backbuffer_dst, m_debug_view_tex, vp_right);
    }
    // The warped eye (submitted separately by AFW).
    if (other_eye_dst != nullptr && other_eye_dst->pTexture != nullptr) {
        const auto od = other_eye_dst->pTexture->GetDesc();
        D3D12_VIEWPORT ovp{0, 0, static_cast<float>(od.Width), static_cast<float>(od.Height), 0, 1};
        vr->d3d12Renderer->Blit(command_list, *other_eye_dst, m_debug_view_tex, ovp);
    }
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

    if (!vr->is_extreme_compatibility_mode_enabled()) {
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
    if ((lastSize[0] != vr->get_hmd_width() || lastSize[1] != vr->get_hmd_height() || lastFormat != backbuffer_desc.Format)) {
        FrameWarpInitParams params = {vr->get_hmd_width(), vr->get_hmd_height(), backbuffer_desc.Format};
        spdlog::info("[VR] Before InitFrameWarp");
        m_eyeFrameBuffers = InitFrameWarp(params);
        spdlog::info("[VR] After InitFrameWarp");
        spdlog::info("[VR] m_eyeFrameBuffers[0]: {} ", (void*)m_eyeFrameBuffers.eyeFrameBuffers->color.pTexture);
        lastSize[0] = vr->get_hmd_width();
        lastSize[1] = vr->get_hmd_height();
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

    if (!vr->is_extreme_compatibility_mode_enabled()) {
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
