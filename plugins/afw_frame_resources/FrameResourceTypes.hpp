// FrameResourceTypes.hpp
//
// Internal C++ types for the frame resource tracker. The enum class values are kept
// numerically identical to the UEVR_FRAME_RESOURCE_* constants in include/UEVRFrameResourcesAPI.h
// so conversion to/from the exported C ABI is a plain static_cast.
//
// This header has NO dependency on the UEVR plugin SDK, on PureDark, or on UESDK. It only needs
// the D3D12/DXGI headers for resource pointer + enum types, so the tracker core can compile into
// the standalone self-test executable.
#pragma once

#include <cstdint>
#include <d3d12.h>
#include <dxgi.h>

namespace afw_fr {

enum class FrameResourceKind : uint32_t {
    Depth = 0,
    Velocity = 1,
    CorrectedVelocity = 2,
    Color = 3,
    Output = 4,
    Count = 5,
};

enum class FrameResourceProvider : uint32_t {
    None = 0,
    RenderTargetPool = 1,
    D3D12Bind = 2,
    InternalCopy = 3,
    DlssNgx = 4, // optional observer only; never required
    Count = 5,
};

enum class FrameResourceEye : uint32_t {
    Unknown = 0,
    Left = 1,
    Right = 2,
    Both = 3,
    Count = 4,
};

enum class FrameResourceValidity : uint32_t {
    Invalid = 0,
    Valid = 1,
    MissingProvider = 2,
    MissingResource = 3,
    WrongRenderer = 4,
    WrongFormat = 5,
    WrongSize = 6,
    NotReady = 7,
    Stale = 8,
};

// What a caller receives. Provider-neutral: raw D3D12 resource plus metadata.
struct FrameResourceView {
    FrameResourceKind kind{FrameResourceKind::Depth};
    FrameResourceProvider provider{FrameResourceProvider::None};
    FrameResourceValidity validity{FrameResourceValidity::Invalid};
    FrameResourceEye eye{FrameResourceEye::Unknown};

    ID3D12Resource* resource{nullptr};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    uint32_t width{0};
    uint32_t height{0};

    D3D12_RESOURCE_STATES expected_state{D3D12_RESOURCE_STATE_COMMON};

    float motion_scale_x{1.0f};
    float motion_scale_y{1.0f};

    uint32_t render_frame{0};
    const char* debug_name{nullptr};
    const char* reason{nullptr};
};

// What a provider hands to the tracker when it discovers/loses a resource.
struct ObservedFrameResource {
    FrameResourceKind kind{FrameResourceKind::Depth};
    FrameResourceProvider provider{FrameResourceProvider::None};
    FrameResourceEye eye{FrameResourceEye::Unknown};

    ID3D12GraphicsCommandList* cmd{nullptr};
    ID3D12Resource* resource{nullptr};

    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    uint32_t width{0};
    uint32_t height{0};
    D3D12_RESOURCE_STATES expected_state{D3D12_RESOURCE_STATE_COMMON};

    float mv_scale_x{1.0f};
    float mv_scale_y{1.0f};

    uint32_t render_frame{0};
    const char* debug_name{nullptr};

    // Real providers leave this true so the tracker owns a COM reference while the
    // resource is exposed through the C ABI. Tests may set false for sentinel pointers.
    bool retain_reference{true};
};

const char* to_string(FrameResourceKind kind);
const char* to_string(FrameResourceProvider provider);
const char* to_string(FrameResourceEye eye);
const char* to_string(FrameResourceValidity validity);

} // namespace afw_fr
