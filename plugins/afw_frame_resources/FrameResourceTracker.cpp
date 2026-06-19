// FrameResourceTracker.cpp
#include "FrameResourceTracker.hpp"
#include "FrameResourceLog.hpp"

#include <cstdio>
#include <cstring>

namespace afw_fr {

// ---- enum -> string -------------------------------------------------------
const char* to_string(FrameResourceKind kind) {
    switch (kind) {
    case FrameResourceKind::Depth: return "Depth";
    case FrameResourceKind::Velocity: return "Velocity";
    case FrameResourceKind::CorrectedVelocity: return "CorrectedVelocity";
    case FrameResourceKind::Color: return "Color";
    case FrameResourceKind::Output: return "Output";
    default: return "?";
    }
}
const char* to_string(FrameResourceProvider provider) {
    switch (provider) {
    case FrameResourceProvider::None: return "None";
    case FrameResourceProvider::RenderTargetPool: return "RenderTargetPool";
    case FrameResourceProvider::D3D12Bind: return "D3D12Bind";
    case FrameResourceProvider::InternalCopy: return "InternalCopy";
    case FrameResourceProvider::DlssNgx: return "DlssNgx";
    default: return "?";
    }
}
const char* to_string(FrameResourceEye eye) {
    switch (eye) {
    case FrameResourceEye::Unknown: return "Unknown";
    case FrameResourceEye::Left: return "Left";
    case FrameResourceEye::Right: return "Right";
    case FrameResourceEye::Both: return "Both";
    default: return "?";
    }
}
const char* to_string(FrameResourceValidity validity) {
    switch (validity) {
    case FrameResourceValidity::Invalid: return "Invalid";
    case FrameResourceValidity::Valid: return "Valid";
    case FrameResourceValidity::MissingProvider: return "MissingProvider";
    case FrameResourceValidity::MissingResource: return "MissingResource";
    case FrameResourceValidity::WrongRenderer: return "WrongRenderer";
    case FrameResourceValidity::WrongFormat: return "WrongFormat";
    case FrameResourceValidity::WrongSize: return "WrongSize";
    case FrameResourceValidity::NotReady: return "NotReady";
    case FrameResourceValidity::Stale: return "Stale";
    default: return "?";
    }
}

namespace {
int validity_rank(FrameResourceValidity v) {
    switch (v) {
    case FrameResourceValidity::Valid: return 4;
    case FrameResourceValidity::Stale: return 3;
    case FrameResourceValidity::MissingResource: return 2;
    case FrameResourceValidity::WrongFormat:
    case FrameResourceValidity::WrongSize:
    case FrameResourceValidity::WrongRenderer:
    case FrameResourceValidity::NotReady: return 2;
    case FrameResourceValidity::MissingProvider:
    case FrameResourceValidity::Invalid:
    default: return 0;
    }
}

// eye candidate search order for a requested eye.
struct EyeList { FrameResourceEye eyes[4]; int count; };
EyeList eye_candidates(FrameResourceEye requested) {
    switch (requested) {
    case FrameResourceEye::Left:
        return {{FrameResourceEye::Left, FrameResourceEye::Both, FrameResourceEye::Unknown}, 3};
    case FrameResourceEye::Right:
        return {{FrameResourceEye::Right, FrameResourceEye::Both, FrameResourceEye::Unknown}, 3};
    case FrameResourceEye::Both:
        return {{FrameResourceEye::Both, FrameResourceEye::Unknown, FrameResourceEye::Left, FrameResourceEye::Right}, 4};
    case FrameResourceEye::Unknown:
    default:
        return {{FrameResourceEye::Unknown, FrameResourceEye::Both, FrameResourceEye::Left, FrameResourceEye::Right}, 4};
    }
}
} // namespace

FrameResourceTracker& FrameResourceTracker::get() {
    static FrameResourceTracker s_instance;
    return s_instance;
}

uint32_t FrameResourceTracker::slot_index(FrameResourceKind kind, FrameResourceProvider provider, FrameResourceEye eye) {
    return ((static_cast<uint32_t>(kind) * kProviders) + static_cast<uint32_t>(provider)) * kEyes
           + static_cast<uint32_t>(eye);
}

void FrameResourceTracker::release_entry(Entry& e) {
    if (e.owns_reference && e.resource != nullptr) {
        e.resource->Release();
    }
    e = {};
}

void FrameResourceTracker::initialize(ID3D12Device* device, ID3D12CommandQueue* queue) {
    std::scoped_lock lock(m_mutex);
    m_device = device;
    m_queue = queue;
    m_initialized = (device != nullptr);
}

void FrameResourceTracker::reset() {
    std::scoped_lock lock(m_mutex);
    for (auto& e : m_slots) {
        release_entry(e);
    }
    m_frame = 0;
    m_eye = FrameResourceEye::Unknown;
    for (auto& c : m_counters) c = {};
}

bool FrameResourceTracker::is_available() const {
    std::scoped_lock lock(m_mutex);
    return m_initialized;
}

void FrameResourceTracker::set_options(const TrackerOptions& opts) {
    std::scoped_lock lock(m_mutex);
    m_options = opts;
}

TrackerOptions FrameResourceTracker::options() const {
    std::scoped_lock lock(m_mutex);
    return m_options;
}

void FrameResourceTracker::begin_frame(uint32_t render_frame) {
    m_frame = render_frame;
}

void FrameResourceTracker::set_current_eye(FrameResourceEye eye) {
    std::scoped_lock lock(m_mutex);
    m_eye = eye;
}

uint32_t FrameResourceTracker::current_frame() const {
    return m_frame.load();
}

FrameResourceEye FrameResourceTracker::current_eye() const {
    std::scoped_lock lock(m_mutex);
    return m_eye;
}

void FrameResourceTracker::observe_resource(const ObservedFrameResource& r) {
    if (r.resource == nullptr) {
        observe_missing(r.kind, r.provider, "observe_resource called with null resource");
        return;
    }
    std::scoped_lock lock(m_mutex);
    Entry& e = m_slots[slot_index(r.kind, r.provider, r.eye)];
    const bool first = !e.used || e.is_missing || e.resource != r.resource;
    const bool same_resource = e.used && !e.is_missing && e.resource == r.resource;

    if (!same_resource) {
        release_entry(e);
        if (r.retain_reference) {
            r.resource->AddRef();
        }
    } else if (e.owns_reference != r.retain_reference) {
        if (r.retain_reference) {
            r.resource->AddRef();
        } else if (e.resource != nullptr) {
            e.resource->Release();
        }
    }

    e.used = true;
    e.is_missing = false;
    e.owns_reference = r.retain_reference;
    e.resource = r.resource;
    e.format = r.format;
    e.width = r.width;
    e.height = r.height;
    e.expected_state = r.expected_state;
    e.mv_scale_x = r.mv_scale_x;
    e.mv_scale_y = r.mv_scale_y;
    e.render_frame = r.render_frame;
    e.debug_name = r.debug_name;
    e.reason = nullptr;

    if (first) {
        char key[160];
        std::snprintf(key, sizeof(key), "valid:%s:%s", to_string(r.provider), to_string(r.kind));
        if (log_once_key(key)) {
            log_info("%s provider=%s eye=%s ptr=0x%p %ux%u fmt=%u name=%s",
                     to_string(r.kind), to_string(r.provider), to_string(r.eye),
                     (void*)r.resource, r.width, r.height, (unsigned)r.format,
                     r.debug_name ? r.debug_name : "?");
        }
    }
}

void FrameResourceTracker::observe_missing(FrameResourceKind kind, FrameResourceProvider provider, const char* reason) {
    std::scoped_lock lock(m_mutex);
    bool was_valid = false;

    for (uint32_t eye = 0; eye < kEyes; ++eye) {
        Entry& e = m_slots[slot_index(kind, provider, static_cast<FrameResourceEye>(eye))];
        was_valid = was_valid || (e.used && !e.is_missing && e.resource != nullptr);
        release_entry(e);
    }

    Entry& e = m_slots[slot_index(kind, provider, FrameResourceEye::Unknown)];
    e.used = true;
    e.is_missing = true;
    e.reason = reason;
    e.render_frame = m_frame.load();

    char key[160];
    std::snprintf(key, sizeof(key), "missing:%s:%s", to_string(provider), to_string(kind));
    if (was_valid || log_once_key(key)) {
        log_info("%s missing provider=%s reason=%s", to_string(kind), to_string(provider),
                 reason ? reason : "?");
    }
}

FrameResourceValidity FrameResourceTracker::entry_validity(const Entry& e) const {
    if (!e.used) return FrameResourceValidity::MissingProvider;
    if (e.is_missing || e.resource == nullptr) return FrameResourceValidity::MissingResource;
    const uint32_t now = m_frame.load();
    const uint32_t age = (now >= e.render_frame) ? (now - e.render_frame) : 0u;
    if (age > m_options.max_stale_frames) return FrameResourceValidity::Stale;
    return FrameResourceValidity::Valid;
}

FrameResourceView FrameResourceTracker::make_view(FrameResourceKind kind, FrameResourceProvider provider,
                                                  FrameResourceEye eye, const Entry& e) const {
    FrameResourceView v;
    v.kind = kind;
    v.provider = provider;
    v.eye = eye;
    v.resource = e.resource;
    v.format = e.format;
    v.width = e.width;
    v.height = e.height;
    v.expected_state = e.expected_state;
    v.motion_scale_x = e.mv_scale_x;
    v.motion_scale_y = e.mv_scale_y;
    v.render_frame = e.render_frame;
    v.debug_name = e.debug_name;
    v.validity = entry_validity(e);
    v.reason = (v.validity == FrameResourceValidity::Stale) ? "stale" : e.reason;
    return v;
}

FrameResourceView FrameResourceTracker::get_latest(FrameResourceKind kind, FrameResourceEye eye) const {
    std::scoped_lock lock(m_mutex);
    const EyeList el = eye_candidates(eye);

    // Pass 1: first provider (by priority) that has a Valid entry wins.
    for (auto provider : m_options.priority) {
        bool have = false;
        Entry best{};
        FrameResourceEye best_eye = FrameResourceEye::Unknown;
        for (int i = 0; i < el.count; ++i) {
            const Entry& e = m_slots[slot_index(kind, provider, el.eyes[i])];
            if (!e.used) continue;
            if (entry_validity(e) != FrameResourceValidity::Valid) continue;
            // Strictly-newer wins; on a tie keep the earlier (more-specific) eye candidate, since
            // eye_candidates() lists the requested eye before the Both/Unknown fallbacks.
            if (!have || e.render_frame > best.render_frame) {
                best = e;
                best_eye = el.eyes[i];
                have = true;
            }
        }
        if (have) return make_view(kind, provider, best_eye, best);
    }

    // Pass 2: best non-valid by rank, earlier provider priority breaks ties.
    int best_rank = -1;
    FrameResourceView best_view;
    best_view.kind = kind;
    best_view.eye = eye;
    best_view.provider = FrameResourceProvider::None;
    best_view.validity = FrameResourceValidity::MissingProvider;
    best_view.reason = "no provider has observed this kind";

    for (auto provider : m_options.priority) {
        for (int i = 0; i < el.count; ++i) {
            const Entry& e = m_slots[slot_index(kind, provider, el.eyes[i])];
            if (!e.used) continue;
            const FrameResourceValidity v = entry_validity(e);
            const int r = validity_rank(v);
            if (r > best_rank) {
                best_rank = r;
                best_view = make_view(kind, provider, el.eyes[i], e);
            }
        }
    }
    return best_view;
}

void FrameResourceTracker::bump(const char* counter, uint64_t n) {
    std::scoped_lock lock(m_mutex);
    for (auto& c : m_counters) {
        if (c.name != nullptr && std::strcmp(c.name, counter) == 0) {
            c.value += n;
            return;
        }
    }
    for (auto& c : m_counters) {
        if (c.name == nullptr) {
            c.name = counter;
            c.value = n;
            return;
        }
    }
}

std::string FrameResourceTracker::describe_state() const {
    std::scoped_lock lock(m_mutex);
    std::string out;
    char line[512];

    std::snprintf(line, sizeof(line), "state: frame=%u eye=%s init=%d providers[rtpool=%d d3d12bind=%d dlss=%d] log=%d\n",
                  m_frame.load(), to_string(m_eye), (int)m_initialized,
                  (int)m_options.enable_render_target_pool, (int)m_options.enable_d3d12_bind,
                  (int)m_options.enable_dlss_observer, (int)m_options.log_level);
    out += line;

    const FrameResourceKind kinds[] = {FrameResourceKind::Depth, FrameResourceKind::Velocity,
                                       FrameResourceKind::CorrectedVelocity};
    for (auto kind : kinds) {
        // resolve inline (we already hold the lock; replicate get_latest pass logic minimally
        // by scanning providers for the best view).
        FrameResourceView best;
        best.kind = kind;
        best.provider = FrameResourceProvider::None;
        best.validity = FrameResourceValidity::MissingProvider;
        best.reason = "none";
        int best_rank = -1;
        const EyeList el = eye_candidates(FrameResourceEye::Unknown);
        for (auto provider : m_options.priority) {
            for (int i = 0; i < el.count; ++i) {
                const Entry& e = m_slots[slot_index(kind, provider, el.eyes[i])];
                if (!e.used) continue;
                const int r = validity_rank(entry_validity(e));
                if (r > best_rank) {
                    best_rank = r;
                    best = make_view(kind, provider, el.eyes[i], e);
                }
            }
        }
        const uint32_t now = m_frame.load();
        const uint32_t age = (now >= best.render_frame) ? (now - best.render_frame) : 0u;
        std::snprintf(line, sizeof(line),
                      "  %-17s provider=%-16s validity=%-15s ptr=0x%p %ux%u fmt=%u age=%u reason=%s\n",
                      to_string(kind), to_string(best.provider), to_string(best.validity),
                      (void*)best.resource, best.width, best.height, (unsigned)best.format, age,
                      best.reason ? best.reason : "");
        out += line;
    }

    bool any_counter = false;
    for (const auto& c : m_counters) {
        if (c.name != nullptr) {
            if (!any_counter) { out += "  counters:"; any_counter = true; }
            std::snprintf(line, sizeof(line), " %s=%llu", c.name, (unsigned long long)c.value);
            out += line;
        }
    }
    if (any_counter) out += "\n";

    return out;
}

const char* FrameResourceTracker::describe_state_cstr() const {
    // Copy into a per-thread buffer so the returned pointer is stable for THIS caller until it next
    // calls describe_state_cstr() (the C ABI contract), and can't be reallocated out from under it
    // by another thread querying concurrently. describe_state() does its own locking.
    static thread_local std::string tls;
    tls = describe_state();
    return tls.c_str();
}

// ---- offline self-test ----------------------------------------------------
bool FrameResourceTracker::run_self_test(std::string* report) {
    int passed = 0;
    int total = 0;
    std::string fails;

    auto check = [&](bool ok, const char* name) {
        ++total;
        if (ok) {
            ++passed;
        } else {
            if (!fails.empty()) fails += ", ";
            fails += name;
        }
    };

    // Sentinel resources: non-null pointers that are NEVER dereferenced by the core.
    auto* r_pool = reinterpret_cast<ID3D12Resource*>(static_cast<uintptr_t>(0x1000));
    auto* r_bind = reinterpret_cast<ID3D12Resource*>(static_cast<uintptr_t>(0x2000));
    auto* r_left = reinterpret_cast<ID3D12Resource*>(static_cast<uintptr_t>(0x3000));
    auto* r_right = reinterpret_cast<ID3D12Resource*>(static_cast<uintptr_t>(0x4000));

    auto make_obs = [](FrameResourceKind k, FrameResourceProvider p, FrameResourceEye eye,
                       ID3D12Resource* res, uint32_t frame, uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
        ObservedFrameResource o;
        o.kind = k; o.provider = p; o.eye = eye; o.resource = res; o.render_frame = frame;
        o.width = w; o.height = h; o.format = fmt; o.debug_name = "selftest";
        o.retain_reference = false;
        return o;
    };

    // C1: fresh tracker -> MissingProvider.
    {
        FrameResourceTracker t;
        auto d = t.get_latest(FrameResourceKind::Depth);
        auto v = t.get_latest(FrameResourceKind::Velocity);
        check(d.validity == FrameResourceValidity::MissingProvider && d.resource == nullptr &&
                  v.validity == FrameResourceValidity::MissingProvider,
              "C1_fresh_missing_provider");
    }

    // C2: observe_missing -> MissingResource, ptr null.
    {
        FrameResourceTracker t;
        t.begin_frame(1);
        t.observe_missing(FrameResourceKind::Velocity, FrameResourceProvider::RenderTargetPool, "no pool name");
        auto v = t.get_latest(FrameResourceKind::Velocity);
        check(v.validity == FrameResourceValidity::MissingResource && v.resource == nullptr,
              "C2_missing_resource");
    }

    // C3: observe a valid depth.
    {
        FrameResourceTracker t;
        t.begin_frame(10);
        t.observe_resource(make_obs(FrameResourceKind::Depth, FrameResourceProvider::RenderTargetPool,
                                    FrameResourceEye::Unknown, r_pool, 10, 1920, 1080, DXGI_FORMAT_R32_TYPELESS));
        auto d = t.get_latest(FrameResourceKind::Depth);
        check(d.validity == FrameResourceValidity::Valid && d.resource == r_pool && d.width == 1920 &&
                  d.height == 1080 && d.provider == FrameResourceProvider::RenderTargetPool,
              "C3_valid_depth");
    }

    // C4: staleness.
    {
        FrameResourceTracker t;
        TrackerOptions opts; opts.max_stale_frames = 2; t.set_options(opts);
        t.begin_frame(10);
        t.observe_resource(make_obs(FrameResourceKind::Depth, FrameResourceProvider::RenderTargetPool,
                                    FrameResourceEye::Unknown, r_pool, 10, 1920, 1080, DXGI_FORMAT_R32_TYPELESS));
        t.begin_frame(13); // age 3 > 2
        auto d = t.get_latest(FrameResourceKind::Depth);
        check(d.validity == FrameResourceValidity::Stale && d.resource == r_pool, "C4_stale");
    }

    // C5: provider priority (both valid same frame -> higher priority wins; flip flips winner).
    {
        FrameResourceTracker t;
        t.begin_frame(5);
        t.observe_resource(make_obs(FrameResourceKind::Depth, FrameResourceProvider::RenderTargetPool,
                                    FrameResourceEye::Unknown, r_pool, 5, 100, 100, DXGI_FORMAT_R32_TYPELESS));
        t.observe_resource(make_obs(FrameResourceKind::Depth, FrameResourceProvider::D3D12Bind,
                                    FrameResourceEye::Unknown, r_bind, 5, 100, 100, DXGI_FORMAT_R32_TYPELESS));
        auto d1 = t.get_latest(FrameResourceKind::Depth);
        bool default_wins = d1.provider == FrameResourceProvider::RenderTargetPool && d1.resource == r_pool;

        TrackerOptions flipped;
        flipped.priority = {{FrameResourceProvider::D3D12Bind, FrameResourceProvider::RenderTargetPool,
                             FrameResourceProvider::InternalCopy, FrameResourceProvider::DlssNgx}};
        t.set_options(flipped);
        auto d2 = t.get_latest(FrameResourceKind::Depth);
        bool flip_wins = d2.provider == FrameResourceProvider::D3D12Bind && d2.resource == r_bind;
        check(default_wins && flip_wins, "C5_provider_priority");
    }

    // C6: eye routing.
    {
        FrameResourceTracker t;
        t.begin_frame(7);
        t.observe_resource(make_obs(FrameResourceKind::Velocity, FrameResourceProvider::D3D12Bind,
                                    FrameResourceEye::Left, r_left, 7, 64, 64, DXGI_FORMAT_R16G16_FLOAT));
        t.observe_resource(make_obs(FrameResourceKind::Velocity, FrameResourceProvider::D3D12Bind,
                                    FrameResourceEye::Right, r_right, 7, 64, 64, DXGI_FORMAT_R16G16_FLOAT));
        auto l = t.get_latest(FrameResourceKind::Velocity, FrameResourceEye::Left);
        auto rr = t.get_latest(FrameResourceKind::Velocity, FrameResourceEye::Right);
        check(l.resource == r_left && l.eye == FrameResourceEye::Left && rr.resource == r_right &&
                  rr.eye == FrameResourceEye::Right,
              "C6_eye_routing");
    }

    // C7: describe_state non-empty + contains tokens.
    {
        FrameResourceTracker t;
        t.begin_frame(3);
        t.observe_resource(make_obs(FrameResourceKind::Depth, FrameResourceProvider::RenderTargetPool,
                                    FrameResourceEye::Unknown, r_pool, 3, 800, 600, DXGI_FORMAT_R32_TYPELESS));
        std::string s = t.describe_state();
        check(!s.empty() && s.find("Depth") != std::string::npos &&
                  s.find("RenderTargetPool") != std::string::npos,
              "C7_describe_state");
    }

    char summary[256];
    if (passed == total) {
        std::snprintf(summary, sizeof(summary), "core selftest: %d/%d PASS", passed, total);
    } else {
        std::snprintf(summary, sizeof(summary), "core selftest: %d/%d FAIL [%s]", passed, total, fails.c_str());
    }
    if (report) *report = summary;
    return passed == total;
}

} // namespace afw_fr
