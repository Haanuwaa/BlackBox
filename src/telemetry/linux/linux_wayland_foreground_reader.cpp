#include "telemetry/linux/linux_wayland_foreground_reader.hpp"

#include "telemetry/foreground_application_tracker.hpp"

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <poll.h>
#include <stop_token>
#include <string_view>
#include <sys/random.h>
#include <thread>

namespace blackbox::telemetry::linux {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t maximum_toplevels = 256U;
constexpr std::size_t maximum_app_id_bytes = 512U;
constexpr std::size_t maximum_toplevel_states = 16U;
constexpr std::uint32_t activated_state = 2U;

extern const wl_interface foreign_toplevel_handle_interface;

const wl_interface* manager_event_types[] = {&foreign_toplevel_handle_interface};
const wl_message manager_requests[] = {
    {"stop", "", nullptr},
};
const wl_message manager_events[] = {
    {"toplevel", "n", manager_event_types},
    {"finished", "", nullptr},
};
const wl_interface foreign_toplevel_manager_interface = {
    "zwlr_foreign_toplevel_manager_v1", 3, 1, manager_requests, 2, manager_events};

const wl_interface* output_event_types[] = {&wl_output_interface};
const wl_interface* parent_event_types[] = {&foreign_toplevel_handle_interface};
const wl_message handle_requests[] = {
    {"destroy", "", nullptr},
};
const wl_message handle_events[] = {
    {"title", "s", nullptr},
    {"app_id", "s", nullptr},
    {"output_enter", "o", output_event_types},
    {"output_leave", "o", output_event_types},
    {"state", "a", nullptr},
    {"done", "", nullptr},
    {"closed", "", nullptr},
    {"parent", "3?o", parent_event_types},
};
const wl_interface foreign_toplevel_handle_interface = {
    "zwlr_foreign_toplevel_handle_v1", 3, 1, handle_requests, 8, handle_events};

void destroy_protocol_proxy(wl_proxy* proxy) noexcept {
    if (proxy == nullptr) return;
    static_cast<void>(wl_proxy_marshal_flags(proxy, 0U, nullptr, wl_proxy_get_version(proxy),
                                             WL_MARSHAL_FLAG_DESTROY));
}

struct ManagerListener {
    void (*toplevel)(void*, wl_proxy*, wl_proxy*);
    void (*finished)(void*, wl_proxy*);
};

struct HandleListener {
    void (*title)(void*, wl_proxy*, const char*);
    void (*app_id)(void*, wl_proxy*, const char*);
    void (*output_enter)(void*, wl_proxy*, wl_output*);
    void (*output_leave)(void*, wl_proxy*, wl_output*);
    void (*state)(void*, wl_proxy*, wl_array*);
    void (*done)(void*, wl_proxy*);
    void (*closed)(void*, wl_proxy*);
    void (*parent)(void*, wl_proxy*, wl_proxy*);
};

[[nodiscard]] bool wayland_session() noexcept {
    const auto* session = std::getenv("XDG_SESSION_TYPE");
    if (session != nullptr && std::string_view{session} == "wayland") return true;
    const auto* display = std::getenv("WAYLAND_DISPLAY");
    return display != nullptr && *display != '\0';
}

void sip_round(std::uint64_t& v0, std::uint64_t& v1, std::uint64_t& v2,
               std::uint64_t& v3) noexcept {
    v0 += v1;
    v1 = std::rotl(v1, 13);
    v1 ^= v0;
    v0 = std::rotl(v0, 32);
    v2 += v3;
    v3 = std::rotl(v3, 16);
    v3 ^= v2;
    v0 += v3;
    v3 = std::rotl(v3, 21);
    v3 ^= v0;
    v2 += v1;
    v1 = std::rotl(v1, 17);
    v1 ^= v2;
    v2 = std::rotl(v2, 32);
}

[[nodiscard]] std::uint64_t load_little_endian(const unsigned char* bytes) noexcept {
    std::uint64_t value{};
    std::memcpy(&value, bytes, sizeof(value));
    if constexpr (std::endian::native == std::endian::big) value = std::byteswap(value);
    return value;
}

[[nodiscard]] std::uint64_t siphash24(const std::string_view value,
                                      const std::uint64_t key0,
                                      const std::uint64_t key1) noexcept {
    std::uint64_t v0 = 0x736f6d6570736575ULL ^ key0;
    std::uint64_t v1 = 0x646f72616e646f6dULL ^ key1;
    std::uint64_t v2 = 0x6c7967656e657261ULL ^ key0;
    std::uint64_t v3 = 0x7465646279746573ULL ^ key1;
    const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
    std::size_t offset{};
    while (value.size() - offset >= sizeof(std::uint64_t)) {
        const auto message = load_little_endian(bytes + offset);
        v3 ^= message;
        sip_round(v0, v1, v2, v3);
        sip_round(v0, v1, v2, v3);
        v0 ^= message;
        offset += sizeof(std::uint64_t);
    }
    std::uint64_t tail = static_cast<std::uint64_t>(value.size()) << 56U;
    for (std::size_t index = 0U; index < value.size() - offset; ++index) {
        tail |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    v3 ^= tail;
    sip_round(v0, v1, v2, v3);
    sip_round(v0, v1, v2, v3);
    v0 ^= tail;
    v2 ^= 0xffU;
    for (int round = 0; round < 4; ++round) sip_round(v0, v1, v2, v3);
    const auto result = v0 ^ v1 ^ v2 ^ v3;
    return result == 0U ? 1U : result;
}

struct RandomIdentity {
    std::uint64_t key0{};
    std::uint64_t key1{};
    std::uint64_t session{};
};

[[nodiscard]] bool random_identity(RandomIdentity& destination) noexcept {
    auto* bytes = reinterpret_cast<unsigned char*>(&destination);
    std::size_t offset{};
    while (offset < sizeof(destination)) {
        const auto count = getrandom(bytes + offset, sizeof(destination) - offset, GRND_NONBLOCK);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        destination = {};
        return false;
    }
    return destination.key0 != 0U && destination.key1 != 0U && destination.session != 0U;
}

[[nodiscard]] std::uint64_t proxy_key(const wl_proxy* proxy) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(proxy));
}

} // namespace

struct LinuxWaylandForegroundReader::State {
    struct Toplevel {
        wl_proxy* proxy{};
    };

    State() noexcept {
        session_candidate = wayland_session();
        if (!session_candidate) return;
        if (!random_identity(identity)) {
            source_status.store(MetricStatus::inaccessible, std::memory_order_relaxed);
            value_status.store(MetricStatus::inaccessible, std::memory_order_relaxed);
            return;
        }
        source_status.store(MetricStatus::temporarily_unavailable, std::memory_order_relaxed);
        value_status.store(MetricStatus::temporarily_unavailable, std::memory_order_relaxed);
        try {
            worker = std::jthread{[this](const std::stop_token stop) { run(stop); }};
        } catch (...) {
            source_status.store(MetricStatus::temporarily_unavailable,
                                std::memory_order_relaxed);
        }
    }

    ~State() {
        worker.request_stop();
        wait.notify_all();
    }

    void publish() noexcept {
        const auto value = tracker.current(identity.session,
                                           source_status.load(std::memory_order_acquire));
        application_token.store(value.has_value() ? value.value.application_token : 0U,
                                std::memory_order_relaxed);
        value_status.store(value.status, std::memory_order_release);
    }

    [[nodiscard]] Toplevel* find_toplevel(wl_proxy* proxy) noexcept {
        for (auto& toplevel : toplevels) {
            if (toplevel.proxy == proxy) return &toplevel;
        }
        return nullptr;
    }

    [[nodiscard]] Toplevel* add_toplevel(wl_proxy* proxy) noexcept {
        for (auto& toplevel : toplevels) {
            if (toplevel.proxy == nullptr) {
                toplevel.proxy = proxy;
                return &toplevel;
            }
        }
        return nullptr;
    }

    static void manager_toplevel(void* data, wl_proxy*, wl_proxy* handle) noexcept;
    static void manager_finished(void* data, wl_proxy* manager_proxy) noexcept;
    static void handle_title(void*, wl_proxy*, const char*) noexcept {}
    static void handle_app_id(void* data, wl_proxy* handle, const char* app_id) noexcept;
    static void handle_output_enter(void*, wl_proxy*, wl_output*) noexcept {}
    static void handle_output_leave(void*, wl_proxy*, wl_output*) noexcept {}
    static void handle_state(void* data, wl_proxy* handle, wl_array* states) noexcept;
    static void handle_done(void* data, wl_proxy*) noexcept {
        static_cast<State*>(data)->publish();
    }
    static void handle_closed(void* data, wl_proxy* handle) noexcept;
    static void handle_parent(void*, wl_proxy*, wl_proxy*) noexcept {}
    static void registry_global(void* data, wl_registry* registry, std::uint32_t name,
                                const char* interface, std::uint32_t version) noexcept;
    static void registry_global_remove(void* data, wl_registry*, std::uint32_t name) noexcept;
    static void registry_complete(void* data, wl_callback* callback, std::uint32_t) noexcept;

    void run(const std::stop_token stop) noexcept {
        while (!stop.stop_requested()) {
            source_status.store(MetricStatus::temporarily_unavailable,
                                std::memory_order_release);
            publish();
            static_cast<void>(run_connection(stop));
            if (stop.stop_requested()) break;
            std::unique_lock lock{wait_mutex};
            wait.wait_for(lock, stop, 1s, [] { return false; });
        }
    }

    [[nodiscard]] bool run_connection(const std::stop_token stop) noexcept {
        display = wl_display_connect(nullptr);
        if (display == nullptr) {
            source_status.store(MetricStatus::inaccessible, std::memory_order_release);
            publish();
            return false;
        }
        registry = wl_display_get_registry(display);
        static constexpr wl_registry_listener registry_listener{
            &State::registry_global, &State::registry_global_remove};
        if (registry == nullptr || wl_registry_add_listener(registry, &registry_listener, this) != 0) {
            cleanup();
            return false;
        }
        initial_sync = wl_display_sync(display);
        static constexpr wl_callback_listener sync_listener{&State::registry_complete};
        if (initial_sync == nullptr ||
            wl_callback_add_listener(initial_sync, &sync_listener, this) != 0) {
            cleanup();
            return false;
        }
        reconnect_requested = false;
        const auto descriptor = wl_display_get_fd(display);
        bool healthy = descriptor >= 0;
        while (healthy && !stop.stop_requested() && !reconnect_requested) {
            while (wl_display_prepare_read(display) != 0) {
                if (wl_display_dispatch_pending(display) < 0) {
                    healthy = false;
                    break;
                }
            }
            if (!healthy) break;
            const auto flushed = wl_display_flush(display);
            if (flushed < 0 && errno != EAGAIN) {
                wl_display_cancel_read(display);
                healthy = false;
                break;
            }
            pollfd descriptor_state{descriptor, POLLIN, 0};
            const auto ready = ::poll(&descriptor_state, 1U, 100);
            if (ready < 0 && errno == EINTR) {
                wl_display_cancel_read(display);
                continue;
            }
            if (ready <= 0) {
                wl_display_cancel_read(display);
                if (ready < 0) healthy = false;
                continue;
            }
            if ((descriptor_state.revents & POLLIN) != 0) {
                if (wl_display_read_events(display) < 0 ||
                    wl_display_dispatch_pending(display) < 0) {
                    healthy = false;
                }
            } else {
                wl_display_cancel_read(display);
            }
            if ((descriptor_state.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) healthy = false;
        }
        cleanup();
        return healthy;
    }

    void cleanup() noexcept {
        tracker.reset();
        for (auto& toplevel : toplevels) {
            destroy_protocol_proxy(toplevel.proxy);
            toplevel = {};
        }
        destroy_protocol_proxy(manager);
        manager = nullptr;
        manager_global_name = 0U;
        if (initial_sync != nullptr) wl_callback_destroy(initial_sync);
        initial_sync = nullptr;
        if (registry != nullptr) wl_registry_destroy(registry);
        registry = nullptr;
        if (display != nullptr) wl_display_disconnect(display);
        display = nullptr;
        source_status.store(MetricStatus::temporarily_unavailable, std::memory_order_release);
        publish();
    }

    RandomIdentity identity{};
    ForegroundApplicationTracker<maximum_toplevels> tracker{};
    std::array<Toplevel, maximum_toplevels> toplevels{};
    wl_display* display{};
    wl_registry* registry{};
    wl_proxy* manager{};
    wl_callback* initial_sync{};
    std::uint32_t manager_global_name{};
    bool reconnect_requested{};
    bool session_candidate{};
    std::atomic<MetricStatus> source_status{MetricStatus::unsupported};
    std::atomic<MetricStatus> value_status{MetricStatus::unsupported};
    std::atomic<std::uint64_t> application_token{};
    std::mutex wait_mutex{};
    std::condition_variable_any wait{};
    std::jthread worker{};
};

void LinuxWaylandForegroundReader::State::manager_toplevel(void* data, wl_proxy*,
                                                           wl_proxy* handle) noexcept {
    auto& state = *static_cast<State*>(data);
    static const HandleListener listener{
        &State::handle_title,       &State::handle_app_id, &State::handle_output_enter,
        &State::handle_output_leave, &State::handle_state,  &State::handle_done,
        &State::handle_closed,      &State::handle_parent};
    if (handle == nullptr || state.add_toplevel(handle) == nullptr ||
        !state.tracker.add(proxy_key(handle)) ||
        wl_proxy_add_listener(
            handle,
            reinterpret_cast<void (**)(void)>(const_cast<HandleListener*>(&listener)),
            &state) != 0) {
        state.tracker.remove(proxy_key(handle));
        if (auto* entry = state.find_toplevel(handle); entry != nullptr) *entry = {};
        destroy_protocol_proxy(handle);
        state.source_status.store(MetricStatus::temporarily_unavailable,
                                  std::memory_order_release);
        state.publish();
    }
}

void LinuxWaylandForegroundReader::State::manager_finished(void* data,
                                                           wl_proxy* manager_proxy) noexcept {
    auto& state = *static_cast<State*>(data);
    destroy_protocol_proxy(manager_proxy);
    state.manager = nullptr;
    state.source_status.store(MetricStatus::unsupported, std::memory_order_release);
    state.publish();
}

void LinuxWaylandForegroundReader::State::handle_app_id(void* data, wl_proxy* handle,
                                                        const char* app_id) noexcept {
    auto& state = *static_cast<State*>(data);
    if (app_id == nullptr) return;
    const auto length = ::strnlen(app_id, maximum_app_id_bytes + 1U);
    if (length == 0U || length > maximum_app_id_bytes) return;
    const auto token = siphash24(std::string_view{app_id, length}, state.identity.key0,
                                 state.identity.key1);
    static_cast<void>(state.tracker.set_application_token(proxy_key(handle), token));
}

void LinuxWaylandForegroundReader::State::handle_state(void* data, wl_proxy* handle,
                                                       wl_array* states) noexcept {
    auto& state = *static_cast<State*>(data);
    if (states == nullptr || states->size > maximum_toplevel_states * sizeof(std::uint32_t) ||
        states->size % sizeof(std::uint32_t) != 0U ||
        (states->size != 0U && states->data == nullptr)) {
        static_cast<void>(state.tracker.set_active(proxy_key(handle), false));
        return;
    }
    bool active{};
    const auto* values = static_cast<const std::uint32_t*>(states->data);
    for (std::size_t index = 0U; index < states->size / sizeof(std::uint32_t); ++index) {
        if (values[index] == activated_state) active = true;
    }
    static_cast<void>(state.tracker.set_active(proxy_key(handle), active));
}

void LinuxWaylandForegroundReader::State::handle_closed(void* data,
                                                        wl_proxy* handle) noexcept {
    auto& state = *static_cast<State*>(data);
    state.tracker.remove(proxy_key(handle));
    if (auto* entry = state.find_toplevel(handle); entry != nullptr) *entry = {};
    destroy_protocol_proxy(handle);
    state.publish();
}

void LinuxWaylandForegroundReader::State::registry_global(
    void* data, wl_registry* registry_value, const std::uint32_t name,
    const char* interface, const std::uint32_t version) noexcept {
    auto& state = *static_cast<State*>(data);
    if (interface == nullptr ||
        std::string_view{interface} != foreign_toplevel_manager_interface.name ||
        state.manager != nullptr || version == 0U) {
        return;
    }
    const auto bound_version = version < 3U ? version : 3U;
    state.manager = static_cast<wl_proxy*>(wl_registry_bind(
        registry_value, name, &foreign_toplevel_manager_interface, bound_version));
    static const ManagerListener listener{&State::manager_toplevel, &State::manager_finished};
    if (state.manager == nullptr ||
        wl_proxy_add_listener(
            state.manager,
            reinterpret_cast<void (**)(void)>(const_cast<ManagerListener*>(&listener)),
            &state) != 0) {
        destroy_protocol_proxy(state.manager);
        state.manager = nullptr;
        return;
    }
    state.manager_global_name = name;
    state.source_status.store(MetricStatus::available, std::memory_order_release);
    state.publish();
}

void LinuxWaylandForegroundReader::State::registry_global_remove(
    void* data, wl_registry*, const std::uint32_t name) noexcept {
    auto& state = *static_cast<State*>(data);
    if (name != state.manager_global_name) return;
    state.reconnect_requested = true;
    state.source_status.store(MetricStatus::temporarily_unavailable,
                              std::memory_order_release);
    state.publish();
}

void LinuxWaylandForegroundReader::State::registry_complete(void* data,
                                                            wl_callback* callback,
                                                            std::uint32_t) noexcept {
    auto& state = *static_cast<State*>(data);
    if (callback != nullptr) wl_callback_destroy(callback);
    state.initial_sync = nullptr;
    if (state.manager == nullptr) {
        state.source_status.store(MetricStatus::unsupported, std::memory_order_release);
        state.publish();
    }
}

LinuxWaylandForegroundReader::LinuxWaylandForegroundReader() noexcept
    : state_{new (std::nothrow) State{}} {}

LinuxWaylandForegroundReader::~LinuxWaylandForegroundReader() = default;

MetricStatus LinuxWaylandForegroundReader::status() const noexcept {
    return state_ != nullptr ? state_->source_status.load(std::memory_order_acquire)
                             : MetricStatus::temporarily_unavailable;
}

bool LinuxWaylandForegroundReader::candidate() const noexcept {
    return state_ != nullptr && state_->session_candidate;
}

MetricValue<OpaqueApplicationIdentity> LinuxWaylandForegroundReader::read() const noexcept {
    if (state_ == nullptr) {
        return MetricValue<OpaqueApplicationIdentity>::unavailable(
            MetricStatus::temporarily_unavailable);
    }
    const auto status = state_->value_status.load(std::memory_order_acquire);
    const auto token = state_->application_token.load(std::memory_order_relaxed);
    if (status != MetricStatus::available || token == 0U) {
        return MetricValue<OpaqueApplicationIdentity>::unavailable(status);
    }
    return MetricValue<OpaqueApplicationIdentity>::available(
        OpaqueApplicationIdentity{state_->identity.session, token});
}

} // namespace blackbox::telemetry::linux
