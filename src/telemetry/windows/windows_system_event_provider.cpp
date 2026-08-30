#include "telemetry/windows/windows_system_event_provider.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cfgmgr32.h>
#include <mmdeviceapi.h>
#include <powerbase.h>
#include <powrprof.h>
#include <dbt.h>
#include <winevt.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>

namespace blackbox::telemetry::windows {
namespace {

[[nodiscard]] constexpr std::int64_t file_time_to_unix_milliseconds(
    const std::uint64_t file_time) noexcept {
    constexpr std::uint64_t windows_to_unix_100ns = 116'444'736'000'000'000ULL;
    return file_time >= windows_to_unix_100ns
               ? static_cast<std::int64_t>((file_time - windows_to_unix_100ns) / 10'000ULL)
               : 0;
}

[[nodiscard]] core::SystemEventLevel level_from_native(const std::uint8_t level) noexcept {
    if (level == 2U || level == 1U) {
        return core::SystemEventLevel::error;
    }
    if (level == 3U) {
        return core::SystemEventLevel::warning;
    }
    return core::SystemEventLevel::informational;
}

} // namespace

std::optional<core::SystemEventKind> normalized_windows_event_kind(
    const core::SystemEventSource source,
    const std::uint32_t native_event_id) noexcept {
    switch (source) {
    case core::SystemEventSource::service_control_manager:
        if (native_event_id == 7031U || native_event_id == 7034U)
            return core::SystemEventKind::service_unexpected_stop;
        if (native_event_id == 7036U || native_event_id == 7040U)
            return core::SystemEventKind::service_state_changed;
        return std::nullopt;
    case core::SystemEventSource::defender:
        if (native_event_id == 1000U)
            return core::SystemEventKind::defender_scan_started;
        if (native_event_id == 1001U)
            return core::SystemEventKind::defender_scan_completed;
        if (native_event_id == 1116U)
            return core::SystemEventKind::defender_threat_detected;
        if (native_event_id >= 1117U && native_event_id <= 1119U)
            return core::SystemEventKind::defender_action;
        if (native_event_id == 5007U)
            return core::SystemEventKind::defender_configuration_changed;
        return std::nullopt;
    case core::SystemEventSource::windows_update:
        if (native_event_id == 19U)
            return core::SystemEventKind::update_succeeded;
        if (native_event_id == 20U || native_event_id == 25U ||
            native_event_id == 31U || native_event_id == 34U)
            return core::SystemEventKind::update_failed;
        if (native_event_id == 41U || native_event_id == 43U ||
            native_event_id == 44U)
            return core::SystemEventKind::update_activity_started;
        return std::nullopt;
    case core::SystemEventSource::application:
        if (native_event_id == 1000U)
            return core::SystemEventKind::application_crash;
        if (native_event_id == 1002U)
            return core::SystemEventKind::application_hang;
        return std::nullopt;
    case core::SystemEventSource::network:
        return native_event_id == 1014U
                   ? std::optional{core::SystemEventKind::dns_resolution_timeout}
                   : std::nullopt;
    case core::SystemEventSource::graphics:
        return native_event_id == 4101U
                   ? std::optional{core::SystemEventKind::display_driver_recovery}
                   : std::nullopt;
    case core::SystemEventSource::storage:
        return native_event_id == 153U
                   ? std::optional{core::SystemEventKind::storage_io_retry}
                   : std::nullopt;
    case core::SystemEventSource::power:
    case core::SystemEventSource::device:
    case core::SystemEventSource::audio:
    case core::SystemEventSource::process:
        return std::nullopt;
    }
    return std::nullopt;
}

struct WindowsSystemEventProvider::NativeState {
    static constexpr std::size_t queue_capacity = 1'024U;

    struct SubscriptionContext {
        NativeState* owner{};
        core::SystemEventSource source{core::SystemEventSource::device};
    };

    class AudioNotifications final : public IMMNotificationClient {
    public:
        explicit AudioNotifications(NativeState& owner) noexcept : owner_{owner} {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
            if (result == nullptr) return E_POINTER;
            *result = nullptr;
            if (id == __uuidof(IUnknown) || id == __uuidof(IMMNotificationClient)) {
                *result = static_cast<IMMNotificationClient*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
        ULONG STDMETHODCALLTYPE Release() override {
            const auto remaining = --references_;
            if (remaining == 0U) delete this;
            return remaining;
        }
        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD state) override {
            owner_.push(make_audio(core::SystemEventKind::audio_endpoint_state_changed,
                                   state));
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
            owner_.push(make_audio(core::SystemEventKind::audio_endpoint_added));
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {
            owner_.push(make_audio(core::SystemEventKind::audio_endpoint_removed));
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(
            EDataFlow flow, ERole role, LPCWSTR) override {
            owner_.push(make_audio(core::SystemEventKind::audio_default_changed,
                                   static_cast<std::uint32_t>(flow) |
                                       (static_cast<std::uint32_t>(role) << 8U)));
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
            // Property changes are both noisy and semantically weak; endpoint state/default
            // callbacks cover the user-visible transitions without retaining identifiers.
            return S_OK;
        }

    private:
        [[nodiscard]] static core::SystemEvent make_audio(
            const core::SystemEventKind kind,
            const std::uint32_t detail = 0U) noexcept {
            core::SystemEvent event{};
            event.source = core::SystemEventSource::audio;
            event.kind = kind;
            event.detail = detail;
            return event;
        }

        std::atomic<ULONG> references_{1U};
        NativeState& owner_;
    };

    void push(core::SystemEvent event) noexcept {
        const std::scoped_lock lock{mutex};
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
        const std::scoped_lock lock{mutex};
        const auto count = (std::min)(size, destination.size());
        for (std::size_t index = 0U; index < count; ++index) {
            auto event = queue[(head + index) % queue.size()];
            event.observed_at = observed_at;
            destination[index] = event;
        }
        head = (head + count) % queue.size();
        size -= count;
        return count;
    }

    [[nodiscard]] std::uint64_t dropped_count() const noexcept {
        const std::scoped_lock lock{mutex};
        return dropped;
    }

    [[nodiscard]] static DWORD CALLBACK device_callback(
        HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action,
        PCM_NOTIFY_EVENT_DATA, DWORD) noexcept {
        auto* owner = static_cast<NativeState*>(context);
        if (owner == nullptr) return ERROR_SUCCESS;
        core::SystemEvent event{};
        event.source = core::SystemEventSource::device;
        switch (action) {
        case CM_NOTIFY_ACTION_DEVICEINSTANCEENUMERATED:
            event.kind = core::SystemEventKind::device_enumerated;
            break;
        case CM_NOTIFY_ACTION_DEVICEINSTANCESTARTED:
            event.kind = core::SystemEventKind::device_started;
            break;
        case CM_NOTIFY_ACTION_DEVICEINSTANCEREMOVED:
            event.kind = core::SystemEventKind::device_removed;
            break;
        default:
            return ERROR_SUCCESS;
        }
        owner->push(event);
        return ERROR_SUCCESS;
    }

    [[nodiscard]] static ULONG CALLBACK power_callback(
        PVOID context, ULONG type, PVOID) noexcept {
        auto* owner = static_cast<NativeState*>(context);
        if (owner == nullptr) return ERROR_SUCCESS;
        core::SystemEvent event{};
        event.source = core::SystemEventSource::power;
        switch (type) {
        case PBT_APMSUSPEND:
            event.kind = core::SystemEventKind::suspend;
            break;
        case PBT_APMRESUMEAUTOMATIC:
            event.kind = core::SystemEventKind::resume_automatic;
            break;
        case PBT_APMRESUMESUSPEND:
        case PBT_APMRESUMECRITICAL:
            event.kind = core::SystemEventKind::resume_user;
            break;
        default:
            return ERROR_SUCCESS;
        }
        owner->push(event);
        return ERROR_SUCCESS;
    }

    [[nodiscard]] static DWORD WINAPI event_log_callback(
        EVT_SUBSCRIBE_NOTIFY_ACTION action, PVOID context, EVT_HANDLE event_handle) noexcept {
        auto* subscription = static_cast<SubscriptionContext*>(context);
        if (subscription == nullptr || subscription->owner == nullptr) return ERROR_SUCCESS;
        if (action == EvtSubscribeActionError) {
            ++subscription->owner->subscription_errors;
            return ERROR_SUCCESS;
        }
        if (action != EvtSubscribeActionDeliver || event_handle == nullptr) {
            return ERROR_SUCCESS;
        }
        LPCWSTR paths[] = {
            L"Event/System/EventID",
            L"Event/System/Level",
            L"Event/System/TimeCreated/@SystemTime",
        };
        const auto context_handle = EvtCreateRenderContext(
            static_cast<DWORD>(std::size(paths)), paths, EvtRenderContextValues);
        if (context_handle == nullptr) {
            ++subscription->owner->subscription_errors;
            return ERROR_SUCCESS;
        }
        std::array<std::byte, 512U> storage{};
        DWORD used{};
        DWORD count{};
        const auto rendered = EvtRender(
            context_handle, event_handle, EvtRenderEventValues,
            static_cast<DWORD>(storage.size()), storage.data(), &used, &count);
        EvtClose(context_handle);
        if (rendered == FALSE || count < 3U) {
            ++subscription->owner->subscription_errors;
            return ERROR_SUCCESS;
        }
        const auto* values = reinterpret_cast<const EVT_VARIANT*>(storage.data());
        const auto event_id = values[0].Type == EvtVarTypeUInt16
                                  ? static_cast<std::uint32_t>(values[0].UInt16Val)
                                  : values[0].Type == EvtVarTypeUInt32
                                        ? values[0].UInt32Val : 0U;
        const auto level = static_cast<std::uint8_t>(
            values[1].Type == EvtVarTypeByte ? values[1].ByteVal : 4U);
        core::SystemEvent event{};
        event.source = subscription->source;
        event.native_event_id = event_id;
        event.level = level_from_native(level);
        if (values[2].Type == EvtVarTypeFileTime) {
            event.source_utc_milliseconds = file_time_to_unix_milliseconds(
                values[2].FileTimeVal);
            event.has_source_utc_time = event.source_utc_milliseconds > 0;
        }
        if (!subscription->owner->map_event_kind(event)) {
            return ERROR_SUCCESS;
        }
        subscription->owner->push(event);
        return ERROR_SUCCESS;
    }

    [[nodiscard]] bool map_event_kind(core::SystemEvent& event) const noexcept {
        const auto kind = normalized_windows_event_kind(event.source,
                                                        event.native_event_id);
        if (!kind) return false;
        event.kind = *kind;
        return true;
    }

    [[nodiscard]] bool subscribe(
        const std::size_t index, LPCWSTR channel, LPCWSTR query,
        const core::SystemEventSource source) noexcept {
        contexts[index] = SubscriptionContext{this, source};
        subscriptions[index] = EvtSubscribe(
            nullptr, nullptr, channel, query, nullptr, &contexts[index],
            event_log_callback, EvtSubscribeToFutureEvents);
        return subscriptions[index] != nullptr;
    }

    [[nodiscard]] EventProviderStatus start(
        const EventProviderConfiguration& requested) noexcept {
        {
            const std::scoped_lock lock{mutex};
            head = 0U;
            size = 0U;
            dropped = 0U;
        }
        subscription_errors.store(0U);
        configuration = requested;
        capabilities = {};
        std::size_t requested_count{};
        std::size_t active_count{};

        if (configuration.device_events) {
            ++requested_count;
            CM_NOTIFY_FILTER filter{};
            filter.cbSize = sizeof(filter);
            filter.Flags = CM_NOTIFY_FILTER_FLAG_ALL_DEVICE_INSTANCES;
            filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE;
            if (CM_Register_Notification(&filter, this, device_callback,
                                         &device_registration) == CR_SUCCESS) {
                capabilities.device_events = true;
                ++active_count;
            }
        }
        if (configuration.power_events) {
            ++requested_count;
            DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS parameters{};
            parameters.Callback = power_callback;
            parameters.Context = this;
            if (PowerRegisterSuspendResumeNotification(
                    DEVICE_NOTIFY_CALLBACK, &parameters,
                    &power_registration) == ERROR_SUCCESS) {
                capabilities.power_events = true;
                ++active_count;
            }
        }
        const auto com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_initialized = SUCCEEDED(com_result);
        const bool com_available = SUCCEEDED(com_result) || com_result == RPC_E_CHANGED_MODE;
        if (configuration.audio_device_events) {
            ++requested_count;
            if (com_available && SUCCEEDED(CoCreateInstance(
                    __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                    __uuidof(IMMDeviceEnumerator),
                    reinterpret_cast<void**>(&audio_enumerator)))) {
                audio_notifications = new (std::nothrow) AudioNotifications{*this};
                if (audio_notifications != nullptr && SUCCEEDED(
                        audio_enumerator->RegisterEndpointNotificationCallback(
                            audio_notifications))) {
                    capabilities.audio_device_events = true;
                    ++active_count;
                }
            }
        }

        std::size_t subscription_index{};
        if (configuration.service_events) {
            ++requested_count;
            if (subscribe(subscription_index++, L"System",
                    L"*[System[Provider[@Name='Service Control Manager'] and "
                    L"(EventID=7031 or EventID=7034 or EventID=7036 or EventID=7040)]]",
                    core::SystemEventSource::service_control_manager)) {
                capabilities.service_events = true;
                ++active_count;
            }
        }
        if (configuration.defender_events) {
            ++requested_count;
            if (subscribe(subscription_index++,
                    L"Microsoft-Windows-Windows Defender/Operational",
                    L"*[System[EventID=1000 or EventID=1001 or EventID=1116 or "
                    L"EventID=1117 or EventID=1118 or EventID=1119 or EventID=5007]]",
                    core::SystemEventSource::defender)) {
                capabilities.defender_events = true;
                ++active_count;
            }
        }
        if (configuration.windows_update_events) {
            ++requested_count;
            if (subscribe(subscription_index++,
                    L"Microsoft-Windows-WindowsUpdateClient/Operational",
                    L"*[System[EventID=19 or EventID=20 or EventID=25 or EventID=31 or "
                    L"EventID=34 or EventID=41 or EventID=43 or EventID=44]]",
                    core::SystemEventSource::windows_update)) {
                capabilities.windows_update_events = true;
                ++active_count;
            }
        }
        if (configuration.application_events) {
            ++requested_count;
            if (subscribe(subscription_index++, L"Application",
                    L"*[System[(Provider[@Name='Application Error'] and EventID=1000) or "
                    L"(Provider[@Name='Application Hang'] and EventID=1002)]]",
                    core::SystemEventSource::application)) {
                capabilities.application_events = true;
                ++active_count;
            }
        }
        if (configuration.network_events) {
            ++requested_count;
            if (subscribe(subscription_index++, L"System",
                    L"*[System[Provider[@Name='Microsoft-Windows-DNS-Client'] and "
                    L"EventID=1014]]",
                    core::SystemEventSource::network)) {
                capabilities.network_events = true;
                ++active_count;
            }
        }
        if (configuration.graphics_events) {
            ++requested_count;
            if (subscribe(subscription_index++, L"System",
                    L"*[System[Provider[@Name='Display'] and EventID=4101]]",
                    core::SystemEventSource::graphics)) {
                capabilities.graphics_events = true;
                ++active_count;
            }
        }
        if (configuration.storage_events) {
            ++requested_count;
            if (subscribe(subscription_index++, L"System",
                    L"*[System[Provider[@Name='disk'] and EventID=153]]",
                    core::SystemEventSource::storage)) {
                capabilities.storage_events = true;
                ++active_count;
            }
        }
        if (requested_count == 0U || active_count == requested_count) {
            return EventProviderStatus::complete;
        }
        return active_count == 0U ? EventProviderStatus::temporarily_failed
                                  : EventProviderStatus::partial;
    }

    void stop() noexcept {
        for (auto& subscription : subscriptions) {
            if (subscription != nullptr) {
                EvtClose(subscription);
                subscription = nullptr;
            }
        }
        if (audio_enumerator != nullptr && audio_notifications != nullptr) {
            static_cast<void>(audio_enumerator->UnregisterEndpointNotificationCallback(
                audio_notifications));
        }
        if (audio_notifications != nullptr) {
            audio_notifications->Release();
            audio_notifications = nullptr;
        }
        if (audio_enumerator != nullptr) {
            audio_enumerator->Release();
            audio_enumerator = nullptr;
        }
        if (power_registration != nullptr) {
            static_cast<void>(PowerUnregisterSuspendResumeNotification(power_registration));
            power_registration = nullptr;
        }
        if (device_registration != nullptr) {
            static_cast<void>(CM_Unregister_Notification(device_registration));
            device_registration = nullptr;
        }
        if (com_initialized) {
            CoUninitialize();
            com_initialized = false;
        }
        capabilities = {};
    }

    mutable std::mutex mutex{};
    std::array<core::SystemEvent, queue_capacity> queue{};
    std::size_t head{};
    std::size_t size{};
    std::uint64_t dropped{};
    std::atomic<std::uint64_t> subscription_errors{};
    EventProviderConfiguration configuration{};
    EventProviderCapabilities capabilities{};
    HCMNOTIFICATION device_registration{};
    HPOWERNOTIFY power_registration{};
    IMMDeviceEnumerator* audio_enumerator{};
    AudioNotifications* audio_notifications{};
    bool com_initialized{};
    std::array<EVT_HANDLE, 7U> subscriptions{};
    std::array<SubscriptionContext, 7U> contexts{};
};

WindowsSystemEventProvider::WindowsSystemEventProvider() noexcept {
    try {
        state_ = std::make_unique<NativeState>();
    } catch (...) {
        // A provider without native state degrades to temporarily_failed. The
        // recorder remains usable and exposes that status through its normal
        // capability/health surfaces.
    }
}

WindowsSystemEventProvider::~WindowsSystemEventProvider() {
    stop();
}

EventProviderStatus WindowsSystemEventProvider::start(
    const EventProviderConfiguration& configuration) noexcept {
    return state_ != nullptr ? state_->start(configuration)
                             : EventProviderStatus::temporarily_failed;
}

EventProviderPollResult WindowsSystemEventProvider::poll(
    const core::MonotonicTimePoint observed_at,
    const std::span<core::SystemEvent> destination) noexcept {
    if (state_ == nullptr) {
        return {EventProviderStatus::temporarily_failed, 0U, 0U};
    }
    const auto count = state_->drain(observed_at, destination);
    const auto capabilities = state_->capabilities;
    const auto configuration = state_->configuration;
    const auto requested = static_cast<std::size_t>(configuration.power_events) +
                           static_cast<std::size_t>(configuration.device_events) +
                           static_cast<std::size_t>(configuration.audio_device_events) +
                           static_cast<std::size_t>(configuration.service_events) +
                           static_cast<std::size_t>(configuration.defender_events) +
                           static_cast<std::size_t>(configuration.windows_update_events) +
                           static_cast<std::size_t>(configuration.application_events) +
                           static_cast<std::size_t>(configuration.network_events) +
                           static_cast<std::size_t>(configuration.graphics_events) +
                           static_cast<std::size_t>(configuration.storage_events);
    const auto active = static_cast<std::size_t>(capabilities.power_events) +
                        static_cast<std::size_t>(capabilities.device_events) +
                        static_cast<std::size_t>(capabilities.audio_device_events) +
                        static_cast<std::size_t>(capabilities.service_events) +
                        static_cast<std::size_t>(capabilities.defender_events) +
                        static_cast<std::size_t>(capabilities.windows_update_events) +
                        static_cast<std::size_t>(capabilities.application_events) +
                        static_cast<std::size_t>(capabilities.network_events) +
                        static_cast<std::size_t>(capabilities.graphics_events) +
                        static_cast<std::size_t>(capabilities.storage_events);
    const auto status = requested == 0U || active == requested
                            ? EventProviderStatus::complete
                            : active == 0U ? EventProviderStatus::temporarily_failed
                                           : EventProviderStatus::partial;
    return {status, count, state_->dropped_count()};
}

void WindowsSystemEventProvider::stop() noexcept {
    if (state_ != nullptr) state_->stop();
}

EventProviderCapabilities WindowsSystemEventProvider::capabilities() const noexcept {
    // Report the sources this implementation can provide. Per-registration
    // availability is reflected by start()/poll() status, so callers can query
    // capabilities safely before the provider thread owns its native state.
    return state_ == nullptr
               ? EventProviderCapabilities{}
               : EventProviderCapabilities{true, true, true, true, true, true, true,
                                           true, true, true};
}

} // namespace blackbox::telemetry::windows
