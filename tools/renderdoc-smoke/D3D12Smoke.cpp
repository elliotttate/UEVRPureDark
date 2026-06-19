#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kFrameCount = 2;
constexpr UINT kWidth = 640;
constexpr UINT kHeight = 360;

HWND g_hwnd{};
ComPtr<IDXGIFactory4> g_factory;
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<IDXGISwapChain3> g_swapchain;
ComPtr<ID3D12DescriptorHeap> g_rtv_heap;
std::array<ComPtr<ID3D12Resource>, kFrameCount> g_render_targets;
ComPtr<ID3D12CommandAllocator> g_allocator;
ComPtr<ID3D12GraphicsCommandList> g_command_list;
ComPtr<ID3D12Fence> g_fence;
HANDLE g_fence_event{};
UINT g_rtv_size{};
UINT g_frame_index{};
uint64_t g_fence_value{};

void check(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string{message} + " hr=0x" + std::to_string(static_cast<uint32_t>(hr)));
    }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

HWND create_window(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"UEVRRenderDocSmokeWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    RECT rect{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"UEVR RenderDoc D3D12 Smoke",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (hwnd == nullptr) {
        throw std::runtime_error("CreateWindowExW failed");
    }
    ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}

ComPtr<IDXGIAdapter1> choose_warp_adapter() {
    ComPtr<IDXGIAdapter1> adapter;
    check(g_factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter");
    return adapter;
}

void init_d3d12() {
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&g_factory)), "CreateDXGIFactory2");

    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device));
    if (FAILED(hr)) {
        auto warp = choose_warp_adapter();
        check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device)),
              "D3D12CreateDevice WARP");
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(g_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&g_queue)), "CreateCommandQueue");

    DXGI_SWAP_CHAIN_DESC1 swap_desc{};
    swap_desc.BufferCount = kFrameCount;
    swap_desc.Width = kWidth;
    swap_desc.Height = kHeight;
    swap_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_desc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapchain1;
    check(g_factory->CreateSwapChainForHwnd(g_queue.Get(), g_hwnd, &swap_desc, nullptr, nullptr, &swapchain1),
          "CreateSwapChainForHwnd");
    check(g_factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER), "MakeWindowAssociation");
    check(swapchain1.As(&g_swapchain), "IDXGISwapChain3");
    g_frame_index = g_swapchain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.NumDescriptors = kFrameCount;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    check(g_device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&g_rtv_heap)), "CreateDescriptorHeap RTV");
    g_rtv_size = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        check(g_swapchain->GetBuffer(i, IID_PPV_ARGS(&g_render_targets[i])), "GetBuffer");
        g_device->CreateRenderTargetView(g_render_targets[i].Get(), nullptr, rtv);
        rtv.ptr += g_rtv_size;
    }

    check(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocator)),
          "CreateCommandAllocator");
    check(g_device->CreateCommandList(
              0,
              D3D12_COMMAND_LIST_TYPE_DIRECT,
              g_allocator.Get(),
              nullptr,
              IID_PPV_ARGS(&g_command_list)),
          "CreateCommandList");
    check(g_command_list->Close(), "Close initial command list");

    check(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)), "CreateFence");
    g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_fence_event == nullptr) {
        throw std::runtime_error("CreateEventW fence failed");
    }
}

void wait_for_gpu() {
    const uint64_t fence_to_wait = ++g_fence_value;
    check(g_queue->Signal(g_fence.Get(), fence_to_wait), "Signal");
    if (g_fence->GetCompletedValue() < fence_to_wait) {
        check(g_fence->SetEventOnCompletion(fence_to_wait, g_fence_event), "SetEventOnCompletion");
        WaitForSingleObject(g_fence_event, INFINITE);
    }
    g_frame_index = g_swapchain->GetCurrentBackBufferIndex();
}

void render_frame(uint32_t frame) {
    check(g_allocator->Reset(), "Reset allocator");
    check(g_command_list->Reset(g_allocator.Get(), nullptr), "Reset command list");

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_render_targets[g_frame_index].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_command_list->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += g_frame_index * g_rtv_size;
    const float t = static_cast<float>(frame % 240) / 240.0f;
    const float color[4] = {0.08f + t * 0.7f, 0.18f, 0.34f + (1.0f - t) * 0.4f, 1.0f};
    g_command_list->ClearRenderTargetView(rtv, color, 0, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_command_list->ResourceBarrier(1, &barrier);

    check(g_command_list->Close(), "Close command list");
    ID3D12CommandList* lists[] = {g_command_list.Get()};
    g_queue->ExecuteCommandLists(1, lists);
    check(g_swapchain->Present(1, 0), "Present");
    wait_for_gpu();
}

int parse_seconds(int argc, wchar_t** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wstring{argv[i]} == L"--seconds") {
            return std::max(1, _wtoi(argv[i + 1]));
        }
    }
    return 20;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const int seconds = parse_seconds(argc, argv);

        std::wcout << L"UEVR RenderDoc D3D12 smoke running for " << seconds << L" seconds\n";

        g_hwnd = create_window(GetModuleHandleW(nullptr));
        init_d3d12();

        const auto end_time = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        MSG msg{};
        uint32_t frame = 0;
        while (std::chrono::steady_clock::now() < end_time) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    wait_for_gpu();
                    return 0;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            render_frame(frame++);
        }

        wait_for_gpu();
        return 0;
    } catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "UEVR RenderDoc D3D12 Smoke", MB_ICONERROR | MB_OK);
        return 1;
    }
}
