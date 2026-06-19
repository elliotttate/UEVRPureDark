// FrameResourceTracker.hpp
//
// Provider-neutral frame resource tracker core. Owns the latest depth/velocity/etc. resource
// per (kind, provider, eye), resolves get_latest() by configurable provider priority + staleness,
// and produces a human-readable describe_state() dump.
//
// No UEVR/PureDark/UESDK dependency (only D3D12 pointer types) so it builds into the self-test exe.
#pragma once

#include "FrameResourceLog.hpp"
#include "FrameResourceTypes.hpp"

#include <array>
#include <atomic>
#include <mutex>
#include <string>

namespace afw_fr {

struct TrackerOptions {
    bool enable_render_target_pool{true};
    bool enable_d3d12_bind{true};
    bool enable_dlss_observer{false}; // never required
    LogLevel log_level{LogLevel::Info};

    // A resource observed more than this many frames ago is reported as Stale (ptr still returned).
    uint32_t max_stale_frames{2};

    // Provider resolution order, highest priority first. Default favours the engine-truth
    // RenderTargetPool, then D3D12 bind discovery, then internal copies, then the optional DLSS observer.
    std::array<FrameResourceProvider, 4> priority{{
        FrameResourceProvider::RenderTargetPool,
        FrameResourceProvider::D3D12Bind,
        FrameResourceProvider::InternalCopy,
        FrameResourceProvider::DlssNgx,
    }};
};

class FrameResourceTracker {
public:
    static FrameResourceTracker& get();

    void initialize(ID3D12Device* device, ID3D12CommandQueue* queue);
    void reset();
    bool is_available() const;

    void set_options(const TrackerOptions& opts);
    TrackerOptions options() const;

    void begin_frame(uint32_t render_frame);
    void set_current_eye(FrameResourceEye eye);
    uint32_t current_frame() const;
    FrameResourceEye current_eye() const;

    // Provider entry points.
    void observe_resource(const ObservedFrameResource& resource);
    void observe_missing(FrameResourceKind kind, FrameResourceProvider provider, const char* reason);

    // Resolution. Always returns a fully-populated view; check view.validity.
    FrameResourceView get_latest(FrameResourceKind kind, FrameResourceEye eye = FrameResourceEye::Unknown) const;

    // Diagnostics. The returned pointer is valid until the next describe_state() call.
    const char* describe_state_cstr() const;
    std::string describe_state() const;

    // Per-name diagnostic counters (accepts/rejects/hop failures, etc.).
    void bump(const char* counter, uint64_t n = 1);

    // Offline self-test: exercises the pure state machine with sentinel resources (never
    // dereferenced). Returns true on full pass; fills *report with a one-line PASS/FAIL summary.
    static bool run_self_test(std::string* report);

private:
    FrameResourceTracker() = default;

    struct Entry {
        bool used{false};
        bool is_missing{false};
        bool owns_reference{false};
        ID3D12Resource* resource{nullptr};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        uint32_t width{0};
        uint32_t height{0};
        D3D12_RESOURCE_STATES expected_state{D3D12_RESOURCE_STATE_COMMON};
        float mv_scale_x{1.0f};
        float mv_scale_y{1.0f};
        uint32_t render_frame{0};
        const char* debug_name{nullptr};
        const char* reason{nullptr};
    };

    static constexpr uint32_t kKinds = static_cast<uint32_t>(FrameResourceKind::Count);
    static constexpr uint32_t kProviders = static_cast<uint32_t>(FrameResourceProvider::Count);
    static constexpr uint32_t kEyes = static_cast<uint32_t>(FrameResourceEye::Count);
    static constexpr uint32_t kSlots = kKinds * kProviders * kEyes;

    static uint32_t slot_index(FrameResourceKind kind, FrameResourceProvider provider, FrameResourceEye eye);
    static void release_entry(Entry& e);

    // Returns validity for an entry given the current frame; null entry => MissingProvider.
    FrameResourceValidity entry_validity(const Entry& e) const;
    FrameResourceView make_view(FrameResourceKind kind, FrameResourceProvider provider,
                                FrameResourceEye eye, const Entry& e) const;

    mutable std::mutex m_mutex;
    bool m_initialized{false};
    ID3D12Device* m_device{nullptr};
    ID3D12CommandQueue* m_queue{nullptr};
    std::atomic<uint32_t> m_frame{0};
    FrameResourceEye m_eye{FrameResourceEye::Unknown};
    TrackerOptions m_options{};
    std::array<Entry, kSlots> m_slots{};

    // counters
    struct Counter { const char* name{nullptr}; uint64_t value{0}; };
    static constexpr uint32_t kMaxCounters = 32;
    mutable std::array<Counter, kMaxCounters> m_counters{};
};

} // namespace afw_fr
