#include "telemetry/linux/linux_x11_foreground_reader.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <cstdlib>
#include <array>
#include <cstdint>
#include <limits>
#include <new>
#include <string_view>
#include <unistd.h>

namespace blackbox::telemetry::linux {
namespace {

[[nodiscard]] bool wayland_session() noexcept {
    const auto* session = std::getenv("XDG_SESSION_TYPE");
    if (session != nullptr && std::string_view{session} == "wayland") return true;
    const auto* display = std::getenv("WAYLAND_DISPLAY");
    return display != nullptr && *display != '\0';
}

[[nodiscard]] bool property_value(Display* display,
                                  const Window window,
                                  const Atom property,
                                  const Atom expected_type,
                                  unsigned long& destination) noexcept {
    Atom actual_type{};
    int actual_format{};
    unsigned long item_count{};
    unsigned long remaining{};
    unsigned char* data{};
    const auto status = XGetWindowProperty(
        display, window, property, 0L, 1L, False, expected_type,
        &actual_type, &actual_format, &item_count, &remaining, &data);
    if (status != Success || actual_type != expected_type || actual_format != 32 ||
        item_count != 1U || data == nullptr) {
        if (data != nullptr) XFree(data);
        return false;
    }
    destination = *reinterpret_cast<unsigned long*>(data);
    XFree(data);
    return true;
}

[[nodiscard]] bool local_client(Display* display,
                                const Window window,
                                const Atom client_machine,
                                const std::string_view local_hostname) noexcept {
    Atom actual_type{};
    int actual_format{};
    unsigned long item_count{};
    unsigned long remaining{};
    unsigned char* data{};
    constexpr long maximum_longs = 64L;
    const auto status = XGetWindowProperty(
        display, window, client_machine, 0L, maximum_longs, False, XA_STRING,
        &actual_type, &actual_format, &item_count, &remaining, &data);
    const bool valid = status == Success && actual_type == XA_STRING &&
                       actual_format == 8 && item_count != 0U &&
                       item_count <= 255U && remaining == 0U && data != nullptr;
    const auto machine = valid
        ? std::string_view{reinterpret_cast<const char*>(data), item_count}
        : std::string_view{};
    const bool matches = valid && machine == local_hostname;
    if (data != nullptr) XFree(data);
    return matches;
}

} // namespace

struct LinuxX11ForegroundReader::State {
    Display* display{};
    Window root{};
    Atom active_window{None};
    Atom process_id{None};
    Atom client_machine{None};
    std::array<char, 256U> hostname{};
    std::size_t hostname_size{};
    MetricStatus unavailable_status{MetricStatus::temporarily_unavailable};
};

LinuxX11ForegroundReader::LinuxX11ForegroundReader() noexcept
    : state_{new (std::nothrow) State{}} {
    if (state_ == nullptr) return;
    if (wayland_session()) {
        state_->unavailable_status = MetricStatus::unsupported;
        return;
    }
    state_->display = XOpenDisplay(nullptr);
    if (state_->display == nullptr) {
        state_->unavailable_status = MetricStatus::inaccessible;
        return;
    }
    state_->root = DefaultRootWindow(state_->display);
    state_->active_window = XInternAtom(state_->display, "_NET_ACTIVE_WINDOW", True);
    state_->process_id = XInternAtom(state_->display, "_NET_WM_PID", True);
    state_->client_machine = XInternAtom(state_->display, "WM_CLIENT_MACHINE", True);
    if (state_->active_window == None || state_->process_id == None ||
        state_->client_machine == None ||
        gethostname(state_->hostname.data(), state_->hostname.size()) != 0) {
        state_->unavailable_status = MetricStatus::unsupported;
        return;
    }
    state_->hostname.back() = '\0';
    state_->hostname_size = std::char_traits<char>::length(state_->hostname.data());
    if (state_->hostname_size == 0U) state_->unavailable_status = MetricStatus::unsupported;
}

LinuxX11ForegroundReader::~LinuxX11ForegroundReader() {
    if (state_ != nullptr && state_->display != nullptr) {
        XCloseDisplay(state_->display);
    }
}

MetricValue<ProcessId> LinuxX11ForegroundReader::read() noexcept {
    if (state_ == nullptr || state_->display == nullptr ||
        state_->active_window == None || state_->process_id == None ||
        state_->client_machine == None || state_->hostname_size == 0U) {
        return MetricValue<ProcessId>::unavailable(
            state_ != nullptr ? state_->unavailable_status
                              : MetricStatus::temporarily_unavailable);
    }
    unsigned long active{};
    if (!property_value(state_->display, state_->root, state_->active_window,
                        XA_WINDOW, active) || active == None) {
        return MetricValue<ProcessId>::unavailable(
            MetricStatus::temporarily_unavailable);
    }
    unsigned long pid{};
    if (!property_value(state_->display, static_cast<Window>(active),
                        state_->process_id, XA_CARDINAL, pid) || pid == 0U ||
        pid > std::numeric_limits<std::uint32_t>::max() ||
        !local_client(state_->display, static_cast<Window>(active),
                      state_->client_machine,
                      std::string_view{state_->hostname.data(), state_->hostname_size})) {
        return MetricValue<ProcessId>::unavailable(
            MetricStatus::temporarily_unavailable);
    }
    return MetricValue<ProcessId>::available(
        ProcessId{static_cast<std::uint32_t>(pid)});
}

} // namespace blackbox::telemetry::linux
