#include "render/RenderDocDxgiProofHooks.hpp"

#include <Windows.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstdint>
#include <mutex>

#include <safetyhook.hpp>
#include <spdlog/spdlog.h>

#include "render/RenderDocCaptureService.hpp"

namespace uevr::renderdoc_dxgi_proof {
namespace {

using CreateDXGIFactoryFn = HRESULT(WINAPI*)(REFIID riid, void** factory);
using CreateDXGIFactory2Fn = HRESULT(WINAPI*)(UINT flags, REFIID riid, void** factory);

std::mutex g_mutex{};
bool g_attempted{};
thread_local uint32_t g_internal_factory_proof_depth{};
safetyhook::InlineHook g_create_factory{};
safetyhook::InlineHook g_create_factory1{};
safetyhook::InlineHook g_create_factory2{};

void note_factory(const char* source, HRESULT result, REFIID riid, void** factory) {
    if (g_internal_factory_proof_depth != 0) {
        return;
    }

    if (SUCCEEDED(result) && factory != nullptr && *factory != nullptr) {
        renderdoc_capture::note_object(
            renderdoc_capture::ObjectKind::DxgiFactoryCreateResult,
            *factory,
            source);
    } else if (FAILED(result)) {
        spdlog::warn("[RenderDoc] DXGI factory proof hook {} failed hr=0x{:08x}",
                     source, static_cast<uint32_t>(result));
    }
    (void)riid;
}

HRESULT WINAPI create_dxgi_factory_hook(REFIID riid, void** factory) {
    auto original = g_create_factory.original<CreateDXGIFactoryFn>();
    if (original == nullptr) {
        return E_FAIL;
    }

    const HRESULT result = original(riid, factory);
    note_factory("CreateDXGIFactory", result, riid, factory);
    return result;
}

HRESULT WINAPI create_dxgi_factory1_hook(REFIID riid, void** factory) {
    auto original = g_create_factory1.original<CreateDXGIFactoryFn>();
    if (original == nullptr) {
        return E_FAIL;
    }

    const HRESULT result = original(riid, factory);
    note_factory("CreateDXGIFactory1", result, riid, factory);
    return result;
}

HRESULT WINAPI create_dxgi_factory2_hook(UINT flags, REFIID riid, void** factory) {
    auto original = g_create_factory2.original<CreateDXGIFactory2Fn>();
    if (original == nullptr) {
        return E_FAIL;
    }

    const HRESULT result = original(flags, riid, factory);
    note_factory("CreateDXGIFactory2", result, riid, factory);
    return result;
}

template <typename Detour>
void install_export(HMODULE dxgi, const char* name, safetyhook::InlineHook& hook, Detour detour) {
    if (hook) {
        return;
    }

    auto* target = reinterpret_cast<void*>(GetProcAddress(dxgi, name));
    if (target == nullptr) {
        return;
    }

    hook = safetyhook::create_inline(target, detour);
    if (!hook) {
        spdlog::warn("[RenderDoc] DXGI factory proof hook failed for {}", name);
        return;
    }

    spdlog::info("[RenderDoc] DXGI factory proof hook installed for {} at 0x{:x}",
                 name, reinterpret_cast<uintptr_t>(target));
}

} // namespace

bool install() {
    if (!renderdoc_capture::env_truthy_w(L"UEVR_RENDERDOC_DXGI_FACTORY_PROOF")) {
        return false;
    }
    if (!renderdoc_capture::is_api_loaded()) {
        return false;
    }

    std::scoped_lock lock{g_mutex};
    if (g_attempted) {
        return static_cast<bool>(g_create_factory) ||
               static_cast<bool>(g_create_factory1) ||
               static_cast<bool>(g_create_factory2);
    }
    g_attempted = true;

    auto* dxgi = LoadLibraryW(L"dxgi.dll");
    if (dxgi == nullptr) {
        spdlog::warn("[RenderDoc] DXGI factory proof hook could not load dxgi.dll");
        return false;
    }

    install_export(dxgi, "CreateDXGIFactory", g_create_factory, create_dxgi_factory_hook);
    install_export(dxgi, "CreateDXGIFactory1", g_create_factory1, create_dxgi_factory1_hook);
    install_export(dxgi, "CreateDXGIFactory2", g_create_factory2, create_dxgi_factory2_hook);

    return static_cast<bool>(g_create_factory) ||
           static_cast<bool>(g_create_factory1) ||
           static_cast<bool>(g_create_factory2);
}

ScopedInternalFactoryProof::ScopedInternalFactoryProof() {
    ++g_internal_factory_proof_depth;
}

ScopedInternalFactoryProof::~ScopedInternalFactoryProof() {
    if (g_internal_factory_proof_depth != 0) {
        --g_internal_factory_proof_depth;
    }
}

} // namespace uevr::renderdoc_dxgi_proof
