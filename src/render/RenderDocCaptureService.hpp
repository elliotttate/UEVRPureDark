#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "renderdoc_app.h"

namespace uevr::renderdoc_capture {

struct CapturePair {
    void* device{};
    void* window{};
};

struct BootstrapResult {
    void* module{};
    bool was_preloaded{};
    bool late_loaded{};
    bool api_loaded{};
    bool capture_safe{};
    bool d3d12_was_loaded{};
    bool dxgi_was_loaded{};
    bool loaded_before_graphics_modules{};
    int api_version_major{};
    int api_version_minor{};
    int api_version_patch{};
    std::string loaded_path{};
};

struct CaptureInfo {
    uint32_t index{};
    std::string path{};
    uint64_t timestamp{};
};

enum class ObjectKind : uint8_t {
    UevrDummyD3D12Device,
    UevrDummyDXGIFactory,
    UevrDummyDXGISwapChain,
    UevrDummyD3D12CommandQueue,
    UevrDummyD3D12CommandList,
    DxgiFactoryCreateResult,
    ObservedDXGIFactory,
    PresentD3D12Device,
    PresentDXGISwapChain,
    PresentD3D12CommandQueue,
    ExecuteD3D12CommandQueue,
    CreatedD3D12CommandList,
    ExecuteD3D12CommandList,
    CreatedD3D12Resource,
    ObservedD3D12Resource,
    CreatedD3D12DescriptorHeap,
    BoundD3D12DescriptorHeap,
    CreatedD3D12RootSignature,
    BoundD3D12RootSignature,
    CreatedD3D12PipelineState,
    BoundD3D12PipelineState,
    Count,
};

struct ObjectSnapshot {
    bool seen{};
    void* pointer{};
    std::string source{};
    std::string vtable_module{};
    bool renderdoc_wrapped{};
    uint64_t sequence{};
};

struct ObjectOwnershipInfo {
    ObjectKind kind{ObjectKind::Count};
    std::string name{};
    ObjectSnapshot first{};
    ObjectSnapshot current{};
};

BootstrapResult bootstrap(bool allow_late_load);
BootstrapResult status();
RENDERDOC_API_1_7_0* api();
bool is_api_loaded();
bool refresh_hooks();

void configure_default_options();
void set_default_capture_template_if_empty();

// Writes RenderDoc capture-file comments INTO an existing .rdc on disk.
// rdc_path is converted to UTF-8 and forwarded to the in-proc API's
// SetCaptureFileComments(filePath, comments). No-op (logged at debug) if the
// API or the function pointer is unavailable.
void write_capture_file_comments(const std::wstring& rdc_path, const std::string& comments);

// Sets the in-progress capture's title via the API's SetCaptureTitle. Only
// valid between StartFrameCapture/EndFrameCapture. Logged no-op if the API or
// the function pointer is unavailable in the loaded RenderDoc version.
void set_capture_title(const std::string& title);
void set_capture_template(const std::string& path_template);
std::string capture_template();
std::vector<CaptureInfo> captures();
std::string newest_capture_path();

void set_active_window(CapturePair pair);
CapturePair active_window();
const char* object_kind_name(ObjectKind kind);
void note_object(ObjectKind kind, void* object, const char* source);
std::vector<ObjectOwnershipInfo> object_ownership();
std::string module_path_for_address(const void* address);
std::string com_object_vtable_module(void* com_object);
bool com_object_looks_renderdoc_wrapped(void* com_object);
bool start_capture(CapturePair pair);
bool end_capture(CapturePair pair);
bool capture_blocking(CapturePair pair, std::chrono::milliseconds duration);

bool env_truthy_w(const wchar_t* name);
std::string env_string_a(const char* name);

} // namespace uevr::renderdoc_capture
