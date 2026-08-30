#include "telemetry/macos/macos_system_event_provider.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
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
    event.kind =
        added ? core::SystemEventKind::device_enumerated : core::SystemEventKind::device_removed;
    event.level = core::SystemEventLevel::informational;
    // Broad normalized class only; no BSD name, registry path, serial, or UUID.
    event.detail = 1U;
    return event;
}

core::SystemEvent normalized_macos_sleep_event(const bool sleeping) noexcept {
    core::SystemEvent event{};
    event.source = core::SystemEventSource::power;
    event.kind =
        sleeping ? core::SystemEventKind::suspend : core::SystemEventKind::resume_automatic;
    event.level = core::SystemEventLevel::informational;
    return event;
}

struct MacosSystemEventProvider::NativeState {
    static constexpr std::size_t queue_capacity = 1'024U;

    static void added_callback(void* context, const io_iterator_t iterator) noexcept {
        auto* state = static_cast<NativeState*>(context);
        if (state != nullptr)
            state->drain_iterator(iterator, true, state->armed);
    }

    static void removed_callback(void* context, const io_iterator_t iterator) noexcept {
        auto* state = static_cast<NativeState*>(context);
        if (state != nullptr)
            state->drain_iterator(iterator, false, state->armed);
    }

    static void power_callback(void* context, const io_service_t, const natural_t message_type,
                               void* message_argument) noexcept {
        auto* state = static_cast<NativeState*>(context);
        if (state == nullptr)
            return;
        const auto notification_id =
            static_cast<long>(reinterpret_cast<std::intptr_t>(message_argument));
        switch (message_type) {
        case kIOMessageCanSystemSleep:
            static_cast<void>(IOAllowPowerChange(state->power_connection, notification_id));
            break;
        case kIOMessageSystemWillSleep:
            state->push(normalized_macos_sleep_event(true));
            static_cast<void>(IOAllowPowerChange(state->power_connection, notification_id));
            break;
        case kIOMessageSystemHasPoweredOn:
            state->push(normalized_macos_sleep_event(false));
            break;
        default:
            break;
        }
    }

    void drain_iterator(const io_iterator_t iterator, const bool added, const bool emit) noexcept {
        while (const auto service = IOIteratorNext(iterator)) {
            IOObjectRelease(service);
            if (emit)
                push(normalized_macos_media_event(added));
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

    [[nodiscard]] std::size_t drain(const core::MonotonicTimePoint observed_at,
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

    IONotificationPortRef device_notification_port{};
    IONotificationPortRef power_notification_port{};
    CFRunLoopRef run_loop{};
    io_iterator_t added_iterator{IO_OBJECT_NULL};
    io_iterator_t removed_iterator{IO_OBJECT_NULL};
    io_connect_t power_connection{IO_OBJECT_NULL};
    io_object_t power_notifier{IO_OBJECT_NULL};
    EventProviderConfiguration configuration{};
    std::array<core::SystemEvent, queue_capacity> queue{};
    std::size_t head{};
    std::size_t size{};
    std::uint64_t dropped{};
    bool armed{};
    bool device_active{};
    bool power_active{};
};

namespace {

[[nodiscard]] EventProviderStatus source_status(const bool device_requested,
                                                const bool device_active,
                                                const bool power_requested,
                                                const bool power_active) noexcept {
    const auto requested =
        static_cast<unsigned>(device_requested) + static_cast<unsigned>(power_requested);
    const auto active = static_cast<unsigned>(device_requested && device_active) +
                        static_cast<unsigned>(power_requested && power_active);
    if (requested == 0U || active == requested)
        return EventProviderStatus::complete;
    return active == 0U ? EventProviderStatus::temporarily_failed : EventProviderStatus::partial;
}

} // namespace

MacosSystemEventProvider::MacosSystemEventProvider() noexcept
    : state_{new (std::nothrow) NativeState{}} {}

MacosSystemEventProvider::~MacosSystemEventProvider() { stop(); }

EventProviderStatus
MacosSystemEventProvider::start(const EventProviderConfiguration& configuration) noexcept {
    stop();
    if (state_ == nullptr)
        return EventProviderStatus::temporarily_failed;
    state_->configuration = configuration;
    state_->dropped = 0U;
    state_->run_loop = CFRunLoopGetCurrent();
    if (state_->run_loop == nullptr)
        return EventProviderStatus::temporarily_failed;

    if (configuration.device_events) {
        state_->device_notification_port = IONotificationPortCreate(kIOMainPortDefault);
        if (state_->device_notification_port != nullptr) {
            const auto source =
                IONotificationPortGetRunLoopSource(state_->device_notification_port);
            if (source != nullptr) {
                CFRunLoopAddSource(state_->run_loop, source, kCFRunLoopDefaultMode);
                auto* added_match = IOServiceMatching(kIOMediaClass);
                auto* removed_match = IOServiceMatching(kIOMediaClass);
                if (added_match != nullptr && removed_match != nullptr) {
                    const auto added_result = IOServiceAddMatchingNotification(
                        state_->device_notification_port, kIOFirstMatchNotification, added_match,
                        &NativeState::added_callback, state_.get(), &state_->added_iterator);
                    // IOKit consumes every matching dictionary passed to a
                    // registration call, including when registration fails.
                    added_match = nullptr;
                    if (added_result == KERN_SUCCESS) {
                        const auto removed_result = IOServiceAddMatchingNotification(
                            state_->device_notification_port, kIOTerminatedNotification,
                            removed_match, &NativeState::removed_callback, state_.get(),
                            &state_->removed_iterator);
                        removed_match = nullptr;
                        if (removed_result == KERN_SUCCESS) {
                            // Registration iterators initially contain inventory.
                            // Drain it without publishing startup device churn.
                            state_->drain_iterator(state_->added_iterator, true, false);
                            state_->drain_iterator(state_->removed_iterator, false, false);
                            state_->device_active = true;
                        }
                    }
                }
                if (added_match != nullptr)
                    CFRelease(added_match);
                if (removed_match != nullptr)
                    CFRelease(removed_match);
            }
        }
        if (!state_->device_active && state_->device_notification_port != nullptr) {
            if (state_->added_iterator != IO_OBJECT_NULL) {
                IOObjectRelease(state_->added_iterator);
                state_->added_iterator = IO_OBJECT_NULL;
            }
            if (state_->removed_iterator != IO_OBJECT_NULL) {
                IOObjectRelease(state_->removed_iterator);
                state_->removed_iterator = IO_OBJECT_NULL;
            }
            const auto source =
                IONotificationPortGetRunLoopSource(state_->device_notification_port);
            if (source != nullptr) {
                CFRunLoopRemoveSource(state_->run_loop, source, kCFRunLoopDefaultMode);
            }
            IONotificationPortDestroy(state_->device_notification_port);
            state_->device_notification_port = nullptr;
        }
    }

    if (configuration.power_events) {
        state_->power_connection =
            IORegisterForSystemPower(state_.get(), &state_->power_notification_port,
                                     &NativeState::power_callback, &state_->power_notifier);
        if (state_->power_connection != IO_OBJECT_NULL &&
            state_->power_notification_port != nullptr) {
            const auto source = IONotificationPortGetRunLoopSource(state_->power_notification_port);
            if (source != nullptr) {
                CFRunLoopAddSource(state_->run_loop, source, kCFRunLoopDefaultMode);
                state_->power_active = true;
            }
        }
    }

    state_->armed = true;
    return source_status(configuration.device_events, state_->device_active,
                         configuration.power_events, state_->power_active);
}

EventProviderPollResult
MacosSystemEventProvider::poll(const core::MonotonicTimePoint observed_at,
                               const std::span<core::SystemEvent> destination) noexcept {
    if (state_ == nullptr) {
        return {EventProviderStatus::temporarily_failed, 0U, 0U};
    }
    static_cast<void>(CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true));
    const auto count = state_->drain(observed_at, destination);
    return {source_status(state_->configuration.device_events, state_->device_active,
                          state_->configuration.power_events, state_->power_active),
            count, state_->dropped};
}

void MacosSystemEventProvider::stop() noexcept {
    if (state_ == nullptr)
        return;
    state_->armed = false;
    if (state_->added_iterator != IO_OBJECT_NULL) {
        IOObjectRelease(state_->added_iterator);
        state_->added_iterator = IO_OBJECT_NULL;
    }
    if (state_->removed_iterator != IO_OBJECT_NULL) {
        IOObjectRelease(state_->removed_iterator);
        state_->removed_iterator = IO_OBJECT_NULL;
    }
    if (state_->device_notification_port != nullptr) {
        const auto source = IONotificationPortGetRunLoopSource(state_->device_notification_port);
        if (source != nullptr && state_->run_loop != nullptr) {
            CFRunLoopRemoveSource(state_->run_loop, source, kCFRunLoopDefaultMode);
        }
        IONotificationPortDestroy(state_->device_notification_port);
        state_->device_notification_port = nullptr;
    }
    if (state_->power_notifier != IO_OBJECT_NULL) {
        IOObjectRelease(state_->power_notifier);
        state_->power_notifier = IO_OBJECT_NULL;
    }
    if (state_->power_connection != IO_OBJECT_NULL) {
        static_cast<void>(IOServiceClose(state_->power_connection));
        state_->power_connection = IO_OBJECT_NULL;
    }
    if (state_->power_notification_port != nullptr) {
        const auto source = IONotificationPortGetRunLoopSource(state_->power_notification_port);
        if (source != nullptr && state_->run_loop != nullptr) {
            CFRunLoopRemoveSource(state_->run_loop, source, kCFRunLoopDefaultMode);
        }
        IONotificationPortDestroy(state_->power_notification_port);
        state_->power_notification_port = nullptr;
    }
    state_->run_loop = nullptr;
    state_->head = 0U;
    state_->size = 0U;
    state_->device_active = false;
    state_->power_active = false;
}

EventProviderCapabilities MacosSystemEventProvider::capabilities() const noexcept {
    EventProviderCapabilities result{};
    result.device_events = true;
    result.power_events = true;
    return result;
}

} // namespace blackbox::telemetry::macos
