#include "telemetry/macos/macos_system_event_provider.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOMedia.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>

namespace blackbox::telemetry::macos {

core::SystemEvent normalized_macos_media_event(const bool added) noexcept {
    core::SystemEvent event{};
    event.source = core::SystemEventSource::device;
    event.kind = added ? core::SystemEventKind::device_enumerated
                       : core::SystemEventKind::device_removed;
    event.level = core::SystemEventLevel::informational;
    // Broad normalized class only; no BSD name, registry path, serial, or UUID.
    event.detail = 1U;
    return event;
}

struct MacosSystemEventProvider::NativeState {
    static constexpr std::size_t queue_capacity = 1'024U;

    static void added_callback(void* context, const io_iterator_t iterator) noexcept {
        auto* state = static_cast<NativeState*>(context);
        if (state != nullptr) state->drain_iterator(iterator, true, state->armed);
    }

    static void removed_callback(void* context, const io_iterator_t iterator) noexcept {
        auto* state = static_cast<NativeState*>(context);
        if (state != nullptr) state->drain_iterator(iterator, false, state->armed);
    }

    void drain_iterator(const io_iterator_t iterator,
                        const bool added,
                        const bool emit) noexcept {
        while (const auto service = IOIteratorNext(iterator)) {
            IOObjectRelease(service);
            if (emit) push(normalized_macos_media_event(added));
        }
    }

    void push(const core::SystemEvent& event) noexcept {
        if (size == queue.size()) {
            head = (head + 1U) % queue.size();
            --size;
            ++dropped;
        }
        queue[(head + size) % queue.size()] = event;
        ++size;
    }

    [[nodiscard]] std::size_t drain(
        const core::MonotonicTimePoint observed_at,
        const std::span<core::SystemEvent> destination) noexcept {
        const auto count = std::min(size, destination.size());
        for (std::size_t index = 0U; index < count; ++index) {
            auto event = queue[(head + index) % queue.size()];
            event.observed_at = observed_at;
            destination[index] = event;
        }
        head = (head + count) % queue.size();
        size -= count;
        return count;
    }

    IONotificationPortRef notification_port{};
    CFRunLoopRef run_loop{};
    io_iterator_t added_iterator{IO_OBJECT_NULL};
    io_iterator_t removed_iterator{IO_OBJECT_NULL};
    EventProviderConfiguration configuration{};
    std::array<core::SystemEvent, queue_capacity> queue{};
    std::size_t head{};
    std::size_t size{};
    std::uint64_t dropped{};
    bool armed{};
};

MacosSystemEventProvider::MacosSystemEventProvider() noexcept
    : state_{new (std::nothrow) NativeState{}} {}

MacosSystemEventProvider::~MacosSystemEventProvider() {
    stop();
}

EventProviderStatus MacosSystemEventProvider::start(
    const EventProviderConfiguration& configuration) noexcept {
    stop();
    if (state_ == nullptr) return EventProviderStatus::temporarily_failed;
    state_->configuration = configuration;
    if (!configuration.device_events) return EventProviderStatus::complete;

    state_->notification_port = IONotificationPortCreate(kIOMainPortDefault);
    if (state_->notification_port == nullptr) {
        return EventProviderStatus::temporarily_failed;
    }
    state_->run_loop = CFRunLoopGetCurrent();
    const auto source = IONotificationPortGetRunLoopSource(state_->notification_port);
    if (source == nullptr || state_->run_loop == nullptr) {
        stop();
        return EventProviderStatus::temporarily_failed;
    }
    CFRunLoopAddSource(state_->run_loop, source, kCFRunLoopDefaultMode);

    auto* added_match = IOServiceMatching(kIOMediaClass);
    auto* removed_match = IOServiceMatching(kIOMediaClass);
    if (added_match == nullptr || removed_match == nullptr) {
        if (added_match != nullptr) CFRelease(added_match);
        if (removed_match != nullptr) CFRelease(removed_match);
        stop();
        return EventProviderStatus::temporarily_failed;
    }
    if (IOServiceAddMatchingNotification(
            state_->notification_port, kIOFirstMatchNotification, added_match,
            &NativeState::added_callback, state_.get(),
            &state_->added_iterator) != KERN_SUCCESS) {
        // IOKit consumes the matching dictionary for this call.
        CFRelease(removed_match);
        stop();
        return EventProviderStatus::temporarily_failed;
    }
    if (IOServiceAddMatchingNotification(
            state_->notification_port, kIOTerminatedNotification, removed_match,
            &NativeState::removed_callback, state_.get(),
            &state_->removed_iterator) != KERN_SUCCESS) {
        // IOKit consumes the matching dictionary for this call.
        stop();
        return EventProviderStatus::temporarily_failed;
    }

    // Registration iterators initially contain inventory. Drain it without
    // publishing events so startup cannot masquerade as device churn.
    state_->drain_iterator(state_->added_iterator, true, false);
    state_->drain_iterator(state_->removed_iterator, false, false);
    state_->armed = true;
    return EventProviderStatus::complete;
}

EventProviderPollResult MacosSystemEventProvider::poll(
    const core::MonotonicTimePoint observed_at,
    const std::span<core::SystemEvent> destination) noexcept {
    if (state_ == nullptr) {
        return {EventProviderStatus::temporarily_failed, 0U, 0U};
    }
    if (!state_->configuration.device_events) {
        return {EventProviderStatus::complete, 0U, state_->dropped};
    }
    if (state_->notification_port == nullptr) {
        return {EventProviderStatus::temporarily_failed, 0U, state_->dropped};
    }
    static_cast<void>(CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true));
    const auto count = state_->drain(observed_at, destination);
    return {EventProviderStatus::complete, count, state_->dropped};
}

void MacosSystemEventProvider::stop() noexcept {
    if (state_ == nullptr) return;
    state_->armed = false;
    if (state_->added_iterator != IO_OBJECT_NULL) {
        IOObjectRelease(state_->added_iterator);
        state_->added_iterator = IO_OBJECT_NULL;
    }
    if (state_->removed_iterator != IO_OBJECT_NULL) {
        IOObjectRelease(state_->removed_iterator);
        state_->removed_iterator = IO_OBJECT_NULL;
    }
    if (state_->notification_port != nullptr) {
        const auto source = IONotificationPortGetRunLoopSource(
            state_->notification_port);
        if (source != nullptr && state_->run_loop != nullptr) {
            CFRunLoopRemoveSource(state_->run_loop, source, kCFRunLoopDefaultMode);
        }
        IONotificationPortDestroy(state_->notification_port);
        state_->notification_port = nullptr;
    }
    state_->run_loop = nullptr;
    state_->head = 0U;
    state_->size = 0U;
}

EventProviderCapabilities MacosSystemEventProvider::capabilities() const noexcept {
    EventProviderCapabilities result{};
    result.device_events = true;
    return result;
}

} // namespace blackbox::telemetry::macos
