#include "render/RenderDocCaptureService.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace uevr::renderdoc_capture {
namespace {

std::mutex g_mutex{};
HMODULE g_module{};
RENDERDOC_API_1_7_0* g_api{};
bool g_get_api_attempted{};
bool g_late_loaded{};
bool g_capture_safe{};
bool g_d3d12_was_loaded{};
bool g_dxgi_was_loaded{};
bool g_loaded_before_graphics_modules{};
std::string g_loaded_path{};
CapturePair g_active_pair{};

struct StoredObjectOwnership {
    ObjectSnapshot first{};
    ObjectSnapshot current{};
};

using RenderDocRefreshHooksFn = void(RENDERDOC_CC*)();

std::array<StoredObjectOwnership, static_cast<size_t>(ObjectKind::Count)> g_object_ownership{};
uint64_t g_object_sequence{};

std::optional<std::filesystem::path> current_module_dir() {
    HMODULE self{};
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&current_module_dir),
            &self)) {
        return std::nullopt;
    }

    wchar_t path[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(self, path, static_cast<DWORD>(std::size(path)));
    if (len == 0 || len >= std::size(path)) {
        return std::nullopt;
    }

    return std::filesystem::path{path}.parent_path();
}

std::wstring env_string_w(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD len = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0 || len >= std::size(buf)) {
        return {};
    }
    return std::wstring{buf, len};
}

std::vector<std::filesystem::path> renderdoc_search_paths() {
    std::vector<std::filesystem::path> paths{};

    if (auto explicit_path = env_string_w(L"UEVR_RENDERDOC_DLL"); !explicit_path.empty()) {
        paths.emplace_back(std::move(explicit_path));
    }
    if (auto explicit_path = env_string_w(L"UEVR_SN2_RD_CAPTURE_DLL"); !explicit_path.empty()) {
        paths.emplace_back(std::move(explicit_path));
    }
    if (auto dir = current_module_dir()) {
        paths.emplace_back(*dir / L"renderdoc.dll");
    }

    paths.emplace_back(L"renderdoc.dll");
    paths.emplace_back(L"C:\\Program Files\\RenderDoc\\renderdoc.dll");
    paths.emplace_back(L"C:\\Program Files (x86)\\RenderDoc\\renderdoc.dll");
    paths.emplace_back(L"E:\\Github\\renderdoc\\x64\\Development\\renderdoc.dll");
    paths.emplace_back(L"E:\\Github\\renderdoc\\x64\\Release\\renderdoc.dll");

    return paths;
}

bool initialise_api_locked() {
    if (g_api != nullptr) {
        return true;
    }

    if (g_module == nullptr) {
        g_module = GetModuleHandleA("renderdoc.dll");
        if (g_module == nullptr) {
            return false;
        }
    }

    if (g_get_api_attempted) {
        return g_api != nullptr;
    }
    g_get_api_attempted = true;

    auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(g_module, "RENDERDOC_GetAPI"));
    if (get_api == nullptr) {
        spdlog::warn("[RenderDoc] renderdoc.dll loaded but RENDERDOC_GetAPI is missing");
        return false;
    }

    void* api_ptr{};
    if (get_api(eRENDERDOC_API_Version_1_7_0, &api_ptr) != 1 || api_ptr == nullptr) {
        api_ptr = nullptr;
        get_api(eRENDERDOC_API_Version_1_6_0, &api_ptr);
    }
    if (api_ptr == nullptr) {
        get_api(eRENDERDOC_API_Version_1_4_0, &api_ptr);
    }

    g_api = static_cast<RENDERDOC_API_1_7_0*>(api_ptr);
    if (g_api == nullptr) {
        spdlog::warn("[RenderDoc] renderdoc.dll loaded but GetAPI returned no compatible interface");
        return false;
    }

    int major{}, minor{}, patch{};
    g_api->GetAPIVersion(&major, &minor, &patch);
    spdlog::info("[RenderDoc] API loaded: v{}.{}.{}", major, minor, patch);
    return true;
}

std::string module_path_locked() {
    if (g_module == nullptr) {
        return {};
    }

    wchar_t path[MAX_PATH * 2]{};
    const DWORD len = GetModuleFileNameW(g_module, path, static_cast<DWORD>(std::size(path)));
    if (len == 0 || len >= std::size(path)) {
        return {};
    }

    return std::filesystem::path{path}.string();
}

bool path_is_renderdoc(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower.find("renderdoc.dll") != std::string::npos;
}

BootstrapResult make_result_locked(bool was_preloaded) {
    BootstrapResult result{};
    result.module = static_cast<void*>(g_module);
    result.was_preloaded = was_preloaded;
    result.late_loaded = g_late_loaded;
    result.capture_safe = g_capture_safe;
    result.d3d12_was_loaded = g_d3d12_was_loaded;
    result.dxgi_was_loaded = g_dxgi_was_loaded;
    result.loaded_before_graphics_modules = g_loaded_before_graphics_modules;
    result.loaded_path = !g_loaded_path.empty() ? g_loaded_path : module_path_locked();

    if (g_api != nullptr) {
        result.api_loaded = true;
        g_api->GetAPIVersion(&result.api_version_major, &result.api_version_minor, &result.api_version_patch);
    }

    return result;
}

CapturePair sanitise_pair(CapturePair pair) {
    if (pair.device == nullptr || pair.window == nullptr) {
        return {};
    }
    return pair;
}

std::string module_path_for_address_impl(const void* address);

void* com_method_at(void* com_object, size_t index) {
    if (com_object == nullptr) {
        return nullptr;
    }

    __try {
        void*** object = reinterpret_cast<void***>(com_object);
        if (object == nullptr || *object == nullptr) {
            return nullptr;
        }
        return (*object)[index];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* first_com_method(void* com_object) {
    return com_method_at(com_object, 0);
}

bool com_object_has_renderdoc_method(void* com_object) {
    if (com_object == nullptr) {
        return false;
    }

    for (size_t i = 0; i < 32; ++i) {
        if (path_is_renderdoc(module_path_for_address_impl(com_method_at(com_object, i)))) {
            return true;
        }
    }

    return false;
}

std::string module_path_for_address_impl(const void* address) {
    if (address == nullptr) {
        return {};
    }

    HMODULE module{};
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address),
            &module)) {
        return {};
    }

    wchar_t path[MAX_PATH * 2]{};
    const DWORD len = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (len == 0 || len >= std::size(path)) {
        return {};
    }

    return std::filesystem::path{path}.string();
}

size_t object_kind_index(ObjectKind kind) {
    return static_cast<size_t>(kind);
}

ObjectSnapshot make_object_snapshot(void* object, const char* source) {
    ObjectSnapshot snapshot{};
    snapshot.seen = object != nullptr;
    snapshot.pointer = object;
    snapshot.source = source != nullptr ? std::string{source} : std::string{};
    snapshot.vtable_module = module_path_for_address_impl(first_com_method(object));
    snapshot.renderdoc_wrapped = path_is_renderdoc(snapshot.vtable_module) || com_object_has_renderdoc_method(object);
    return snapshot;
}

} // namespace

bool env_truthy_w(const wchar_t* name) {
    wchar_t buf[8]{};
    const DWORD len = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    return len > 0 && len < std::size(buf) && buf[0] != L'\0' && buf[0] != L'0';
}

std::string env_string_a(const char* name) {
    char buf[2048]{};
    const DWORD len = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0 || len >= sizeof(buf)) {
        return {};
    }
    return std::string{buf, len};
}

BootstrapResult bootstrap(bool allow_late_load) {
    std::lock_guard lock{g_mutex};

    if (env_truthy_w(L"UEVR_DISABLE_RENDERDOC_BOOTSTRAP")) {
        spdlog::info("[RenderDoc] bootstrap skipped (UEVR_DISABLE_RENDERDOC_BOOTSTRAP=1)");
        return {};
    }

    const bool d3d12_was_loaded = GetModuleHandleA("d3d12.dll") != nullptr;
    const bool dxgi_was_loaded = GetModuleHandleA("dxgi.dll") != nullptr;
    if (g_module == nullptr) {
        g_d3d12_was_loaded = d3d12_was_loaded;
        g_dxgi_was_loaded = dxgi_was_loaded;
        g_loaded_before_graphics_modules = !d3d12_was_loaded && !dxgi_was_loaded;
    }

    bool was_preloaded = false;
    if (g_module == nullptr) {
        g_module = GetModuleHandleA("renderdoc.dll");
        was_preloaded = g_module != nullptr;
        if (was_preloaded) {
            g_loaded_path = module_path_locked();
            g_capture_safe = true;
            g_loaded_before_graphics_modules = true;
            spdlog::info("[RenderDoc] preloaded by launcher: 0x{:x}",
                         reinterpret_cast<uintptr_t>(g_module));
        }
    } else {
        was_preloaded = !g_late_loaded;
    }

    if (g_module == nullptr && allow_late_load) {
        const bool suspended_launch = env_truthy_w(L"UEVR_RENDERDOC_LAUNCHED_SUSPENDED");
        for (const auto& path : renderdoc_search_paths()) {
            g_module = LoadLibraryW(path.c_str());
            if (g_module != nullptr) {
                g_late_loaded = true;
                g_loaded_path = path.string();
                g_capture_safe = g_loaded_before_graphics_modules || suspended_launch;
                if (g_loaded_before_graphics_modules) {
                    spdlog::info("[RenderDoc] OPT-IN early load from {} before D3D12/DXGI modules.",
                                 g_loaded_path);
                } else if (suspended_launch) {
                    spdlog::info("[RenderDoc] OPT-IN suspended-launch load from {} after graphics modules were present; "
                                 "capture safety now depends on wrapper proof before game resume.",
                                 g_loaded_path);
                } else {
                    spdlog::warn("[RenderDoc] OPT-IN load from {} after graphics modules were present "
                                 "(d3d12_loaded={} dxgi_loaded={}). Capture safety is degraded.",
                                 g_loaded_path, g_d3d12_was_loaded, g_dxgi_was_loaded);
                }
                break;
            }
        }
    }

    if (g_module == nullptr) {
        return {};
    }

    initialise_api_locked();
    if (g_api != nullptr) {
        configure_default_options();
        set_default_capture_template_if_empty();
    }

    return make_result_locked(was_preloaded);
}

BootstrapResult status() {
    std::lock_guard lock{g_mutex};
    const bool was_preloaded = g_module != nullptr && !g_late_loaded;
    return make_result_locked(was_preloaded);
}

RENDERDOC_API_1_7_0* api() {
    std::lock_guard lock{g_mutex};
    if (g_module == nullptr) {
        g_module = GetModuleHandleA("renderdoc.dll");
        if (g_module == nullptr) {
            return nullptr;
        }
        g_loaded_path = module_path_locked();
        if (!g_late_loaded) {
            g_capture_safe = true;
            g_loaded_before_graphics_modules = true;
        }
    }
    initialise_api_locked();
    return g_api;
}

bool is_api_loaded() {
    return api() != nullptr;
}

bool refresh_hooks() {
    RenderDocRefreshHooksFn refresh{};

    {
        std::lock_guard lock{g_mutex};
        if (g_module == nullptr) {
            g_module = GetModuleHandleA("renderdoc.dll");
            if (g_module == nullptr) {
                spdlog::warn("[RenderDoc] hook refresh skipped because renderdoc.dll is not loaded");
                return false;
            }
            g_loaded_path = module_path_locked();
            if (!g_late_loaded) {
                g_capture_safe = true;
                g_loaded_before_graphics_modules = true;
            }
        }

        refresh = reinterpret_cast<RenderDocRefreshHooksFn>(
            GetProcAddress(g_module, "RENDERDOC_UEVR_RefreshHooks"));
    }

    if (refresh == nullptr) {
        spdlog::warn("[RenderDoc] hook refresh export missing; rebuild E:\\Github\\renderdoc with the UEVR refresh export");
        return false;
    }

    refresh();
    spdlog::info("[RenderDoc] refreshed RenderDoc hook tables for newly loaded modules");
    return true;
}

void configure_default_options() {
    if (g_api == nullptr || g_api->SetCaptureOptionU32 == nullptr) {
        return;
    }

    g_api->SetCaptureOptionU32(eRENDERDOC_Option_CaptureAllCmdLists, 1);
    g_api->SetCaptureOptionU32(eRENDERDOC_Option_DebugOutputMute, 1);

    // Allow NVIDIA (PCI vendor 0x10DE) vendor extensions through capture so
    // NVAPI-using titles don't crash or lose state under RenderDoc. The header
    // documents no values; 0x10DE is the vendor-ID byte-value RenderDoc's NV
    // path expects. Replays of captures using these extensions may be
    // corrupted on non-NVIDIA replay machines - acceptable for our use.
    g_api->SetCaptureOptionU32(eRENDERDOC_Option_AllowUnsupportedVendorExtensions, 0x10DE);

    // Keep all resources (including transient/aliased ones that RDG recycles
    // per-frame) alive inside the capture so that froxel/scene-color buffers
    // are inspectable. Without this flag, RenderDoc may drop references to
    // transient D3D12 resources before EndFrameCapture and they appear as
    // empty or garbage in the replay - which is exactly what causes SN2's
    // fog volumes to read as garbage in a capture.
    g_api->SetCaptureOptionU32(eRENDERDOC_Option_RefAllResources, 1);

    // #16 Defensive soft memory limit (MBs). With RefAllResources keeping every
    // live resource in the capture, the in-memory footprint can balloon; a soft
    // limit asks RenderDoc to spill above this to disk instead of risking OOM.
    // 800MB sits inside the header's suggested 200-1000MB range. Unconditional.
    g_api->SetCaptureOptionU32(eRENDERDOC_Option_SoftMemoryLimit, 800);

    // #15 CPU callstack capture for API events. Heavy on both perf and capture
    // size, so only enabled when the readable-capture opt-in env is set/"1".
    // CaptureCallstacksOnlyActions restricts the (still costly) callstacks to
    // actions/draws only, which is the useful subset for tracing producers.
    {
        const std::string readable = env_string_a("UEVR_SN2_CAPTURE_READABLE");
        if (!readable.empty() && readable != "0") {
            g_api->SetCaptureOptionU32(eRENDERDOC_Option_CaptureCallstacks, 1);
            g_api->SetCaptureOptionU32(eRENDERDOC_Option_CaptureCallstacksOnlyActions, 1);
            spdlog::info("[RenderDoc] readable-capture opt-in: CaptureCallstacks(OnlyActions) enabled");
        }
    }
}

void set_default_capture_template_if_empty() {
    if (g_api == nullptr || g_api->GetCaptureFilePathTemplate == nullptr ||
        g_api->SetCaptureFilePathTemplate == nullptr) {
        return;
    }

    const char* existing = g_api->GetCaptureFilePathTemplate();
    if (existing != nullptr && *existing != '\0') {
        return;
    }

    wchar_t temp[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temp) == 0) {
        return;
    }

    wchar_t pathw[MAX_PATH + 64]{};
    SYSTEMTIME st{};
    GetLocalTime(&st);
    swprintf_s(pathw, L"%suevr_renderdoc_%04d%02d%02d_%02d%02d%02d",
                temp, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    char patha[MAX_PATH + 64]{};
    WideCharToMultiByte(CP_UTF8, 0, pathw, -1, patha, static_cast<int>(std::size(patha)), nullptr, nullptr);
    g_api->SetCaptureFilePathTemplate(patha);
    spdlog::info("[RenderDoc] capture template set to: {}", patha);
}

void write_capture_file_comments(const std::wstring& rdc_path, const std::string& comments) {
    std::lock_guard lock{g_mutex};
    if (g_api == nullptr || g_api->SetCaptureFileComments == nullptr) {
        spdlog::debug("[RenderDoc] write_capture_file_comments skipped (API/SetCaptureFileComments unavailable)");
        return;
    }

    // SetCaptureFileComments takes a UTF-8 filePath; pass nullptr (most-recent
    // capture) only when the caller gave no path. RenderDoc treats "" the same.
    std::string path_utf8{};
    if (!rdc_path.empty()) {
        const int needed = WideCharToMultiByte(CP_UTF8, 0, rdc_path.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (needed > 0) {
            path_utf8.resize(static_cast<size_t>(needed));
            WideCharToMultiByte(CP_UTF8, 0, rdc_path.c_str(), -1, path_utf8.data(), needed, nullptr, nullptr);
            if (!path_utf8.empty() && path_utf8.back() == '\0') {
                path_utf8.pop_back();
            }
        }
    }

    g_api->SetCaptureFileComments(path_utf8.empty() ? nullptr : path_utf8.c_str(), comments.c_str());
}

void set_capture_title(const std::string& title) {
    std::lock_guard lock{g_mutex};
    if (g_api == nullptr || g_api->SetCaptureTitle == nullptr) {
        spdlog::debug("[RenderDoc] set_capture_title skipped (API/SetCaptureTitle unavailable)");
        return;
    }

    g_api->SetCaptureTitle(title.c_str());
}

void set_capture_template(const std::string& path_template) {
    auto* rdoc = api();
    if (rdoc == nullptr || rdoc->SetCaptureFilePathTemplate == nullptr || path_template.empty()) {
        return;
    }
    rdoc->SetCaptureFilePathTemplate(path_template.c_str());
}

std::string capture_template() {
    auto* rdoc = api();
    if (rdoc == nullptr || rdoc->GetCaptureFilePathTemplate == nullptr) {
        return {};
    }
    const char* value = rdoc->GetCaptureFilePathTemplate();
    return value != nullptr ? std::string{value} : std::string{};
}

std::vector<CaptureInfo> captures() {
    std::vector<CaptureInfo> result{};
    auto* rdoc = api();
    if (rdoc == nullptr || rdoc->GetNumCaptures == nullptr || rdoc->GetCapture == nullptr) {
        return result;
    }

    const uint32_t count = rdoc->GetNumCaptures();
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t path_len{};
        uint64_t timestamp{};
        if (!rdoc->GetCapture(i, nullptr, &path_len, &timestamp) || path_len == 0) {
            continue;
        }

        std::string path(path_len, '\0');
        if (rdoc->GetCapture(i, path.data(), &path_len, &timestamp)) {
            if (!path.empty() && path.back() == '\0') {
                path.pop_back();
            }
            result.push_back(CaptureInfo{i, std::move(path), timestamp});
        }
    }
    return result;
}

std::string newest_capture_path() {
    auto values = captures();
    if (values.empty()) {
        return {};
    }
    return values.back().path;
}

void set_active_window(CapturePair pair) {
    auto* rdoc = api();
    pair = sanitise_pair(pair);
    {
        std::lock_guard lock{g_mutex};
        g_active_pair = pair;
    }
    if (rdoc == nullptr || rdoc->SetActiveWindow == nullptr || pair.device == nullptr) {
        return;
    }
    rdoc->SetActiveWindow(pair.device, pair.window);
}

CapturePair active_window() {
    std::lock_guard lock{g_mutex};
    return g_active_pair;
}

const char* object_kind_name(ObjectKind kind) {
    switch (kind) {
    case ObjectKind::UevrDummyD3D12Device: return "uevr_dummy_d3d12_device";
    case ObjectKind::UevrDummyDXGIFactory: return "uevr_dummy_dxgi_factory";
    case ObjectKind::UevrDummyDXGISwapChain: return "uevr_dummy_dxgi_swapchain";
    case ObjectKind::UevrDummyD3D12CommandQueue: return "uevr_dummy_d3d12_command_queue";
    case ObjectKind::UevrDummyD3D12CommandList: return "uevr_dummy_d3d12_command_list";
    case ObjectKind::DxgiFactoryCreateResult: return "dxgi_factory_create_result";
    case ObjectKind::ObservedDXGIFactory: return "observed_dxgi_factory";
    case ObjectKind::PresentD3D12Device: return "present_d3d12_device";
    case ObjectKind::PresentDXGISwapChain: return "present_dxgi_swapchain";
    case ObjectKind::PresentD3D12CommandQueue: return "present_d3d12_command_queue";
    case ObjectKind::ExecuteD3D12CommandQueue: return "execute_d3d12_command_queue";
    case ObjectKind::CreatedD3D12CommandList: return "created_d3d12_command_list";
    case ObjectKind::ExecuteD3D12CommandList: return "execute_d3d12_command_list";
    case ObjectKind::CreatedD3D12Resource: return "created_d3d12_resource";
    case ObjectKind::ObservedD3D12Resource: return "observed_d3d12_resource";
    case ObjectKind::CreatedD3D12DescriptorHeap: return "created_d3d12_descriptor_heap";
    case ObjectKind::BoundD3D12DescriptorHeap: return "bound_d3d12_descriptor_heap";
    case ObjectKind::CreatedD3D12RootSignature: return "created_d3d12_root_signature";
    case ObjectKind::BoundD3D12RootSignature: return "bound_d3d12_root_signature";
    case ObjectKind::CreatedD3D12PipelineState: return "created_d3d12_pipeline_state";
    case ObjectKind::BoundD3D12PipelineState: return "bound_d3d12_pipeline_state";
    case ObjectKind::Count: break;
    }
    return "unknown";
}

void note_object(ObjectKind kind, void* object, const char* source) {
    if (object == nullptr || kind == ObjectKind::Count) {
        return;
    }

    const auto index = object_kind_index(kind);
    if (index >= g_object_ownership.size()) {
        return;
    }

    {
        std::lock_guard lock{g_mutex};
        const auto& current = g_object_ownership[index].current;
        if (current.seen && current.pointer == object) {
            return;
        }
    }

    auto snapshot = make_object_snapshot(object, source);

    bool first_seen = false;
    {
        std::lock_guard lock{g_mutex};
        auto& ownership = g_object_ownership[index];
        snapshot.sequence = ++g_object_sequence;
        if (!ownership.first.seen) {
            ownership.first = snapshot;
            first_seen = true;
        }
        ownership.current = snapshot;
    }

    if (first_seen) {
        spdlog::info("[RenderDoc] first observed {}: ptr=0x{:x} wrapped={} vtable_module='{}' source='{}'",
                     object_kind_name(kind), reinterpret_cast<uintptr_t>(object),
                     snapshot.renderdoc_wrapped, snapshot.vtable_module, snapshot.source);
    }
}

std::vector<ObjectOwnershipInfo> object_ownership() {
    std::lock_guard lock{g_mutex};

    std::vector<ObjectOwnershipInfo> result{};
    result.reserve(g_object_ownership.size());
    for (size_t i = 0; i < g_object_ownership.size(); ++i) {
        const auto kind = static_cast<ObjectKind>(i);
        const auto& stored = g_object_ownership[i];
        result.push_back(ObjectOwnershipInfo{
            kind,
            object_kind_name(kind),
            stored.first,
            stored.current,
        });
    }
    return result;
}

std::string module_path_for_address(const void* address) {
    return module_path_for_address_impl(address);
}

std::string com_object_vtable_module(void* com_object) {
    return module_path_for_address(first_com_method(com_object));
}

bool com_object_looks_renderdoc_wrapped(void* com_object) {
    return path_is_renderdoc(com_object_vtable_module(com_object)) || com_object_has_renderdoc_method(com_object);
}

bool start_capture(CapturePair pair) {
    auto* rdoc = api();
    if (rdoc == nullptr || rdoc->StartFrameCapture == nullptr) {
        return false;
    }

    pair = sanitise_pair(pair);
    rdoc->StartFrameCapture(pair.device, pair.window);
    return true;
}

bool end_capture(CapturePair pair) {
    auto* rdoc = api();
    if (rdoc == nullptr || rdoc->EndFrameCapture == nullptr) {
        return false;
    }

    pair = sanitise_pair(pair);
    return rdoc->EndFrameCapture(pair.device, pair.window) != 0;
}

bool capture_blocking(CapturePair pair, std::chrono::milliseconds duration) {
    if (!start_capture(pair)) {
        return false;
    }
    std::this_thread::sleep_for(duration);
    return end_capture(pair);
}

} // namespace uevr::renderdoc_capture
