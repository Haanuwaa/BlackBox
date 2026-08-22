#include "platform/windows/windows_global_hotkey_manager.hpp"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace blackbox::platform::windows {
namespace {

constexpr int incident_hotkey_id = 0x4242;

[[nodiscard]] constexpr UINT native_key(const HotkeyKey key) noexcept {
    const auto number = static_cast<UINT>(key);
    if (number < 1U || number > 12U) {
        return 0U;
    }
    return VK_F1 + number - 1U;
}

[[nodiscard]] constexpr UINT native_modifiers(
    const HotkeyCombination combination) noexcept {
    UINT result = MOD_NOREPEAT;
    if (combination.control) {
        result |= MOD_CONTROL;
    }
    if (combination.shift) {
        result |= MOD_SHIFT;
    }
    if (combination.alt) {
        result |= MOD_ALT;
    }
    if (combination.windows) {
        result |= MOD_WIN;
    }
    return result;
}

} // namespace

struct WindowsGlobalHotkeyManager::NativeState {
    std::jthread worker{};
    mutable std::mutex mutex{};
    std::condition_variable ready_condition{};
    std::atomic<bool> registered{};
    bool initialized{};
    DWORD thread_id{};
    DWORD registration_error{};
};

WindowsGlobalHotkeyManager::WindowsGlobalHotkeyManager()
    : native_{std::make_unique<NativeState>()} {}

WindowsGlobalHotkeyManager::~WindowsGlobalHotkeyManager() {
    unregister_hotkey();
}

HotkeyRegistrationResult WindowsGlobalHotkeyManager::register_hotkey(
    const HotkeyCombination combination,
    HotkeyCallback callback) {
    unregister_hotkey();
    const auto key = native_key(combination.key);
    if (key == 0U || !callback) {
        return HotkeyRegistrationResult::invalid_combination;
    }

    {
        const std::scoped_lock lock{native_->mutex};
        native_->initialized = false;
        native_->registration_error = ERROR_SUCCESS;
        native_->thread_id = 0U;
    }
    native_->worker = std::jthread{
        [state = native_.get(), combination, callback = std::move(callback), key] {
            MSG queue_initializer{};
            static_cast<void>(PeekMessageW(
                &queue_initializer, nullptr, WM_USER, WM_USER, PM_NOREMOVE));
            const auto thread_id = GetCurrentThreadId();
            const auto succeeded = RegisterHotKey(
                nullptr, incident_hotkey_id, native_modifiers(combination), key) != FALSE;
            const auto error = succeeded ? ERROR_SUCCESS : GetLastError();
            {
                const std::scoped_lock lock{state->mutex};
                state->thread_id = thread_id;
                state->registration_error = error;
                state->registered.store(succeeded);
                state->initialized = true;
            }
            state->ready_condition.notify_all();
            if (!succeeded) {
                return;
            }

            MSG message{};
            while (true) {
                const auto result = GetMessageW(&message, nullptr, 0U, 0U);
                if (result <= 0) {
                    break;
                }
                if (message.message == WM_HOTKEY &&
                    message.wParam == static_cast<WPARAM>(incident_hotkey_id)) {
                    try {
                        callback();
                    } catch (...) {
                        // Native message handling must not propagate application
                        // callback failures across the platform thread boundary.
                    }
                }
            }
            static_cast<void>(UnregisterHotKey(nullptr, incident_hotkey_id));
            state->registered.store(false);
        }};

    DWORD error = ERROR_SUCCESS;
    {
        std::unique_lock lock{native_->mutex};
        native_->ready_condition.wait(lock, [this] { return native_->initialized; });
        error = native_->registration_error;
    }
    if (error == ERROR_SUCCESS) {
        return HotkeyRegistrationResult::registered;
    }
    if (native_->worker.joinable()) {
        native_->worker.join();
    }
    return error == ERROR_HOTKEY_ALREADY_REGISTERED
               ? HotkeyRegistrationResult::conflict
               : HotkeyRegistrationResult::unavailable;
}

void WindowsGlobalHotkeyManager::unregister_hotkey() noexcept {
    if (!native_->worker.joinable()) {
        return;
    }

    DWORD thread_id = 0U;
    {
        const std::scoped_lock lock{native_->mutex};
        thread_id = native_->thread_id;
    }
    if (thread_id != 0U) {
        static_cast<void>(PostThreadMessageW(thread_id, WM_QUIT, 0U, 0));
    }
    try {
        native_->worker.join();
    } catch (...) {
        // Destruction and shutdown remain non-throwing.
    }
    native_->registered.store(false);
}

bool WindowsGlobalHotkeyManager::registered() const noexcept {
    return native_->registered.load();
}

bool WindowsGlobalHotkeyManager::post_activation_for_testing() noexcept {
    DWORD thread_id = 0U;
    {
        const std::scoped_lock lock{native_->mutex};
        thread_id = native_->thread_id;
    }
    return thread_id != 0U &&
           PostThreadMessageW(thread_id, WM_HOTKEY,
                              static_cast<WPARAM>(incident_hotkey_id), 0) != FALSE;
}

} // namespace blackbox::platform::windows
