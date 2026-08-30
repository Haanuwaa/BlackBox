#include "telemetry/macos/macos_system_event_provider.hpp"

#import <AppKit/AppKit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/storage/IOMedia.h>
#include <SystemConfiguration/SystemConfiguration.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <utility>

namespace blackbox::telemetry::macos {

core::SystemEvent normalized_macos_media_event(const bool added) noexcept {
    core::SystemEvent event{};
    event.source = core::SystemEventSource::storage;
    event.kind =
        added ? core::SystemEventKind::storage_device_added
              : core::SystemEventKind::storage_device_removed;
    event.level = core::SystemEventLevel::informational;
    // Broad normalized class only; no BSD name, registry path, serial, or UUID.
    event.detail = 1U;
    return event;
}

core::SystemEvent normalized_macos_application_event(const bool started) noexcept {
    core::SystemEvent event{};
    event.source = core::SystemEventSource::application;
    event.kind = started ? core::SystemEventKind::application_started
                         : core::SystemEventKind::application_terminated;
    event.level = core::SystemEventLevel::informational;
    return event;
}

core::SystemEvent normalized_macos_audio_event(const bool default_changed) noexcept {
    core::SystemEvent event{};
    event.source = core::SystemEventSource::audio;
    event.kind = default_changed ? core::SystemEventKind::audio_default_changed
                                 : core::SystemEventKind::audio_endpoint_state_changed;
    event.level = core::SystemEventLevel::informational;
    return event;
}

core::SystemEvent normalized_macos_display_event() noexcept {
    core::SystemEvent event{};
    event.source = core::SystemEventSource::graphics;
    event.kind = core::SystemEventKind::display_configuration_changed;
    event.level = core::SystemEventLevel::informational;
    return event;
}

core::SystemEvent normalized_macos_network_event() noexcept {
    core::SystemEvent event{};
    event.source = core::SystemEventSource::network;
    event.kind = core::SystemEventKind::network_connectivity_changed;
    event.level = core::SystemEventLevel::informational;
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

    static OSStatus audio_callback(const AudioObjectID, const UInt32 address_count,
                                   const AudioObjectPropertyAddress* addresses,
                                   void* context) noexcept {
        auto* state = static_cast<NativeState*>(context);
        if (state == nullptr || !state->armed || addresses == nullptr) return noErr;
        for (UInt32 index = 0U; index < address_count; ++index) {
            const auto selector = addresses[index].mSelector;
            const auto is_default = selector == kAudioHardwarePropertyDefaultInputDevice ||
                                    selector == kAudioHardwarePropertyDefaultOutputDevice;
            state->push(normalized_macos_audio_event(is_default));
        }
        return noErr;
    }

    static void display_callback(const CGDirectDisplayID,
                                 const CGDisplayChangeSummaryFlags flags,
                                 void* context) noexcept {
        auto* state = static_cast<NativeState*>(context);
        if (state != nullptr && state->armed &&
            (flags & kCGDisplayBeginConfigurationFlag) == 0U) {
            state->push(normalized_macos_display_event());
        }
    }

    static void network_callback(SCDynamicStoreRef, CFArrayRef, void* context) noexcept {
        auto* state = static_cast<NativeState*>(context);
        if (state != nullptr && state->armed) state->push(normalized_macos_network_event());
    }

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
        if (state == nullptr || !state->armed)
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
        const std::scoped_lock lock{queue_mutex};
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
        const std::scoped_lock lock{queue_mutex};
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
    SCDynamicStoreRef network_store{};
    CFRunLoopSourceRef network_source{};
    id application_started_observer{};
    id application_terminated_observer{};
    std::array<AudioObjectPropertyAddress, 3U> audio_addresses{
        AudioObjectPropertyAddress{kAudioHardwarePropertyDevices,
                                   kAudioObjectPropertyScopeGlobal,
                                   kAudioObjectPropertyElementMain},
        AudioObjectPropertyAddress{kAudioHardwarePropertyDefaultInputDevice,
                                   kAudioObjectPropertyScopeGlobal,
                                   kAudioObjectPropertyElementMain},
        AudioObjectPropertyAddress{kAudioHardwarePropertyDefaultOutputDevice,
                                   kAudioObjectPropertyScopeGlobal,
                                   kAudioObjectPropertyElementMain},
    };
    std::array<bool, 3U> audio_registered{};
    EventProviderConfiguration configuration{};
    std::array<core::SystemEvent, queue_capacity> queue{};
    std::mutex queue_mutex{};
    std::size_t head{};
    std::size_t size{};
    std::atomic_uint64_t dropped{};
    std::atomic_bool armed{};
    bool device_active{};
    bool power_active{};
    bool audio_active{};
    bool application_active{};
    bool network_active{};
    bool display_active{};
};

namespace {

template <typename State>
[[nodiscard]] EventProviderStatus source_status(
    const EventProviderConfiguration& configuration, const State& state) noexcept {
    const std::array pairs{
        std::pair{configuration.storage_events, state.device_active},
        std::pair{configuration.power_events, state.power_active},
        std::pair{configuration.audio_device_events, state.audio_active},
        std::pair{configuration.application_events, state.application_active},
        std::pair{configuration.network_events, state.network_active},
        std::pair{configuration.graphics_events, state.display_active},
    };
    unsigned requested{};
    unsigned active{};
    for (const auto& [wanted, available] : pairs) {
        requested += static_cast<unsigned>(wanted);
        active += static_cast<unsigned>(wanted && available);
    }
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
    state_->dropped.store(0U, std::memory_order_relaxed);
    state_->run_loop = CFRunLoopGetCurrent();
    if (state_->run_loop == nullptr)
        return EventProviderStatus::temporarily_failed;
    // Arm before power registration so even an immediate sleep query is
    // acknowledged. Startup media inventory is still explicitly drained with
    // emit=false below.
    state_->armed.store(true, std::memory_order_release);

    if (configuration.storage_events) {
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

    if (configuration.audio_device_events) {
        state_->audio_active = true;
        for (std::size_t index = 0U; index < state_->audio_addresses.size(); ++index) {
            if (AudioObjectAddPropertyListener(kAudioObjectSystemObject,
                                               &state_->audio_addresses[index],
                                               &NativeState::audio_callback,
                                               state_.get()) == noErr) {
                state_->audio_registered[index] = true;
            } else {
                state_->audio_active = false;
            }
        }
        if (!state_->audio_active) {
            for (std::size_t index = 0U; index < state_->audio_addresses.size(); ++index) {
                if (state_->audio_registered[index]) {
                    static_cast<void>(AudioObjectRemovePropertyListener(
                        kAudioObjectSystemObject, &state_->audio_addresses[index],
                        &NativeState::audio_callback, state_.get()));
                    state_->audio_registered[index] = false;
                }
            }
        }
    }

    if (configuration.application_events) {
        auto* center = [[NSWorkspace sharedWorkspace] notificationCenter];
        auto* native_state = state_.get();
        state_->application_started_observer =
            [center addObserverForName:NSWorkspaceDidLaunchApplicationNotification
                                object:nil
                                 queue:nil
                            usingBlock:^(NSNotification*) {
                                if (native_state->armed) {
                                    native_state->push(normalized_macos_application_event(true));
                                }
                            }];
        state_->application_terminated_observer =
            [center addObserverForName:NSWorkspaceDidTerminateApplicationNotification
                                object:nil
                                 queue:nil
                            usingBlock:^(NSNotification*) {
                                if (native_state->armed) {
                                    native_state->push(normalized_macos_application_event(false));
                                }
                            }];
        state_->application_active = state_->application_started_observer != nil &&
                                     state_->application_terminated_observer != nil;
    }

    if (configuration.network_events) {
        SCDynamicStoreContext context{0, state_.get(), nullptr, nullptr, nullptr};
        state_->network_store = SCDynamicStoreCreate(
            nullptr, CFSTR("io.github.Haanuwaa.BlackBox.system-events"),
            &NativeState::network_callback, &context);
        if (state_->network_store != nullptr) {
            const void* values[]{CFSTR("State:/Network/Global/IPv4"),
                                 CFSTR("State:/Network/Global/IPv6")};
            auto* keys = CFArrayCreate(nullptr, values, 2, &kCFTypeArrayCallBacks);
            if (keys != nullptr &&
                SCDynamicStoreSetNotificationKeys(state_->network_store, keys, nullptr)) {
                state_->network_source =
                    SCDynamicStoreCreateRunLoopSource(nullptr, state_->network_store, 0);
                if (state_->network_source != nullptr) {
                    CFRunLoopAddSource(state_->run_loop, state_->network_source,
                                       kCFRunLoopDefaultMode);
                    state_->network_active = true;
                }
            }
            if (keys != nullptr) CFRelease(keys);
        }
    }

    if (configuration.graphics_events &&
        CGDisplayRegisterReconfigurationCallback(&NativeState::display_callback,
                                                 state_.get()) == kCGErrorSuccess) {
        state_->display_active = true;
    }

    return source_status(configuration, *state_);
}

EventProviderPollResult
MacosSystemEventProvider::poll(const core::MonotonicTimePoint observed_at,
                               const std::span<core::SystemEvent> destination) noexcept {
    if (state_ == nullptr) {
        return {EventProviderStatus::temporarily_failed, 0U, 0U};
    }
    static_cast<void>(CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true));
    const auto count = state_->drain(observed_at, destination);
    return {source_status(state_->configuration, *state_), count,
            state_->dropped.load(std::memory_order_relaxed)};
}

void MacosSystemEventProvider::stop() noexcept {
    if (state_ == nullptr)
        return;
    state_->armed.store(false, std::memory_order_release);
    if (state_->display_active) {
        static_cast<void>(CGDisplayRemoveReconfigurationCallback(
            &NativeState::display_callback, state_.get()));
        state_->display_active = false;
    }
    if (state_->application_started_observer != nil ||
        state_->application_terminated_observer != nil) {
        auto* center = [[NSWorkspace sharedWorkspace] notificationCenter];
        if (state_->application_started_observer != nil) {
            [center removeObserver:state_->application_started_observer];
            state_->application_started_observer = nil;
        }
        if (state_->application_terminated_observer != nil) {
            [center removeObserver:state_->application_terminated_observer];
            state_->application_terminated_observer = nil;
        }
    }
    for (std::size_t index = 0U; index < state_->audio_addresses.size(); ++index) {
        if (state_->audio_registered[index]) {
            static_cast<void>(AudioObjectRemovePropertyListener(
                kAudioObjectSystemObject, &state_->audio_addresses[index],
                &NativeState::audio_callback, state_.get()));
            state_->audio_registered[index] = false;
        }
    }
    if (state_->network_source != nullptr) {
        if (state_->run_loop != nullptr) {
            CFRunLoopRemoveSource(state_->run_loop, state_->network_source,
                                  kCFRunLoopDefaultMode);
        }
        CFRelease(state_->network_source);
        state_->network_source = nullptr;
    }
    if (state_->network_store != nullptr) {
        CFRelease(state_->network_store);
        state_->network_store = nullptr;
    }
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
    {
        const std::scoped_lock lock{state_->queue_mutex};
        state_->head = 0U;
        state_->size = 0U;
    }
    state_->device_active = false;
    state_->power_active = false;
    state_->audio_active = false;
    state_->application_active = false;
    state_->network_active = false;
    state_->display_active = false;
}

EventProviderCapabilities MacosSystemEventProvider::capabilities() const noexcept {
    EventProviderCapabilities result{};
    result.power_events = true;
    result.audio_device_events = true;
    result.application_events = true;
    result.network_events = true;
    result.graphics_events = true;
    result.storage_events = true;
    return result;
}

} // namespace blackbox::telemetry::macos
