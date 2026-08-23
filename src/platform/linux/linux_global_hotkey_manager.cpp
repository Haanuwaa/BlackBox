#include "platform/linux/linux_global_hotkey_manager.hpp"

#include <dbus/dbus.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <unistd.h>

namespace blackbox::platform::linux {
namespace {

constexpr char portal_service[] = "org.freedesktop.portal.Desktop";
constexpr char portal_path[] = "/org/freedesktop/portal/desktop";
constexpr char shortcuts_interface[] = "org.freedesktop.portal.GlobalShortcuts";
constexpr char request_interface[] = "org.freedesktop.portal.Request";
constexpr char session_interface[] = "org.freedesktop.portal.Session";
constexpr char shortcut_id[] = "capture-incident";
constexpr int portal_timeout_milliseconds = 2'000;

[[nodiscard]] bool valid_combination(const HotkeyCombination combination) noexcept {
    const auto key = static_cast<std::uint8_t>(combination.key);
    return key >= static_cast<std::uint8_t>(HotkeyKey::f1) &&
           key <= static_cast<std::uint8_t>(HotkeyKey::f12);
}

[[nodiscard]] bool append_basic(DBusMessageIter& iterator,
                                const int type,
                                const void* value) noexcept {
    return dbus_message_iter_append_basic(&iterator, type, value) != FALSE;
}

[[nodiscard]] bool append_string_option(DBusMessageIter& options,
                                        const char* key,
                                        const char* value) noexcept {
    DBusMessageIter entry{};
    DBusMessageIter variant{};
    return dbus_message_iter_open_container(
               &options, DBUS_TYPE_DICT_ENTRY, nullptr, &entry) != FALSE &&
           append_basic(entry, DBUS_TYPE_STRING, &key) &&
           dbus_message_iter_open_container(
               &entry, DBUS_TYPE_VARIANT, DBUS_TYPE_STRING_AS_STRING,
               &variant) != FALSE &&
           append_basic(variant, DBUS_TYPE_STRING, &value) &&
           dbus_message_iter_close_container(&entry, &variant) != FALSE &&
           dbus_message_iter_close_container(&options, &entry) != FALSE;
}

[[nodiscard]] DBusMessage* call(DBusConnection* connection,
                                DBusMessage* request) noexcept {
    if (connection == nullptr || request == nullptr) return nullptr;
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        connection, request, portal_timeout_milliseconds, nullptr);
    dbus_message_unref(request);
    if (reply != nullptr && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        dbus_message_unref(reply);
        return nullptr;
    }
    return reply;
}

[[nodiscard]] std::string reply_object_path(DBusMessage* reply) {
    if (reply == nullptr) return {};
    DBusMessageIter iterator{};
    if (!dbus_message_iter_init(reply, &iterator) ||
        dbus_message_iter_get_arg_type(&iterator) != DBUS_TYPE_OBJECT_PATH) {
        return {};
    }
    const char* value{};
    dbus_message_iter_get_basic(&iterator, &value);
    return value != nullptr ? std::string{value} : std::string{};
}

[[nodiscard]] bool response_session_handle(DBusMessage* message,
                                           std::string& session_handle) {
    DBusMessageIter iterator{};
    if (!dbus_message_iter_init(message, &iterator) ||
        dbus_message_iter_get_arg_type(&iterator) != DBUS_TYPE_UINT32) {
        return false;
    }
    dbus_uint32_t response{};
    dbus_message_iter_get_basic(&iterator, &response);
    if (response != 0U || !dbus_message_iter_next(&iterator) ||
        dbus_message_iter_get_arg_type(&iterator) != DBUS_TYPE_ARRAY) {
        return false;
    }
    DBusMessageIter entries{};
    dbus_message_iter_recurse(&iterator, &entries);
    while (dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry{};
        dbus_message_iter_recurse(&entries, &entry);
        const char* key{};
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&entry, &key);
        }
        if (key != nullptr && std::string_view{key} == "session_handle" &&
            dbus_message_iter_next(&entry) &&
            dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
            DBusMessageIter value{};
            dbus_message_iter_recurse(&entry, &value);
            // The portal specification retains this object path in a string
            // variant for backwards compatibility.
            if (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_STRING) {
                const char* path{};
                dbus_message_iter_get_basic(&value, &path);
                if (path != nullptr) session_handle = path;
            }
        }
        dbus_message_iter_next(&entries);
    }
    return !session_handle.empty();
}

[[nodiscard]] bool response_bound_shortcut(DBusMessage* message) noexcept {
    DBusMessageIter iterator{};
    if (!dbus_message_iter_init(message, &iterator) ||
        dbus_message_iter_get_arg_type(&iterator) != DBUS_TYPE_UINT32) {
        return false;
    }
    dbus_uint32_t response{};
    dbus_message_iter_get_basic(&iterator, &response);
    if (response != 0U || !dbus_message_iter_next(&iterator) ||
        dbus_message_iter_get_arg_type(&iterator) != DBUS_TYPE_ARRAY) {
        return false;
    }
    DBusMessageIter entries{};
    dbus_message_iter_recurse(&iterator, &entries);
    while (dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry{};
        dbus_message_iter_recurse(&entries, &entry);
        const char* key{};
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&entry, &key);
        }
        if (key != nullptr && std::string_view{key} == "shortcuts" &&
            dbus_message_iter_next(&entry) &&
            dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
            DBusMessageIter value{};
            dbus_message_iter_recurse(&entry, &value);
            if (dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_ARRAY) return false;
            DBusMessageIter shortcuts{};
            dbus_message_iter_recurse(&value, &shortcuts);
            while (dbus_message_iter_get_arg_type(&shortcuts) == DBUS_TYPE_STRUCT) {
                DBusMessageIter shortcut{};
                dbus_message_iter_recurse(&shortcuts, &shortcut);
                const char* identifier{};
                if (dbus_message_iter_get_arg_type(&shortcut) == DBUS_TYPE_STRING) {
                    dbus_message_iter_get_basic(&shortcut, &identifier);
                }
                if (identifier != nullptr &&
                    std::string_view{identifier} == shortcut_id) {
                    return true;
                }
                dbus_message_iter_next(&shortcuts);
            }
            return false;
        }
        dbus_message_iter_next(&entries);
    }
    return false;
}

template <typename Response>
[[nodiscard]] bool wait_for_response(DBusConnection* connection,
                                     const std::string& request_path,
                                     Response response) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds{portal_timeout_milliseconds};
    while (std::chrono::steady_clock::now() < deadline) {
        if (dbus_connection_read_write(connection, 50) == FALSE) return false;
        while (DBusMessage* message = dbus_connection_pop_message(connection)) {
            const char* path = dbus_message_get_path(message);
            const bool matching = path != nullptr && request_path == path &&
                dbus_message_is_signal(message, request_interface, "Response") != FALSE;
            const bool accepted = matching && response(message);
            dbus_message_unref(message);
            if (matching) return accepted;
        }
    }
    return false;
}

[[nodiscard]] std::string unique_token(const char* prefix) {
    static std::atomic<std::uint64_t> sequence{};
    return std::string{prefix} + '_' + std::to_string(::getpid()) + '_' +
           std::to_string(++sequence);
}

[[nodiscard]] std::string create_session(DBusConnection* connection) {
    DBusMessage* request = dbus_message_new_method_call(
        portal_service, portal_path, shortcuts_interface, "CreateSession");
    if (request == nullptr) return {};
    DBusMessageIter arguments{};
    DBusMessageIter options{};
    dbus_message_iter_init_append(request, &arguments);
    bool valid = dbus_message_iter_open_container(
                     &arguments, DBUS_TYPE_ARRAY, "{sv}", &options) != FALSE;
    const auto handle_token = unique_token("blackbox_request");
    const auto session_token = unique_token("blackbox_session");
    const char* handle_value = handle_token.c_str();
    const char* session_value = session_token.c_str();
    valid = valid && append_string_option(options, "handle_token", handle_value) &&
            append_string_option(options, "session_handle_token", session_value) &&
            dbus_message_iter_close_container(&arguments, &options) != FALSE;
    if (!valid) {
        dbus_message_unref(request);
        return {};
    }
    DBusMessage* reply = call(connection, request);
    const auto request_path = reply_object_path(reply);
    if (reply != nullptr) dbus_message_unref(reply);
    if (request_path.empty()) return {};
    std::string session_handle{};
    const bool accepted = wait_for_response(
        connection, request_path,
        [&session_handle](DBusMessage* response) {
            return response_session_handle(response, session_handle);
        });
    return accepted ? session_handle : std::string{};
}

[[nodiscard]] bool bind_shortcut(DBusConnection* connection,
                                 const std::string& session_handle,
                                 const std::string& accelerator) {
    DBusMessage* request = dbus_message_new_method_call(
        portal_service, portal_path, shortcuts_interface, "BindShortcuts");
    if (request == nullptr) return false;
    DBusMessageIter arguments{};
    dbus_message_iter_init_append(request, &arguments);
    const char* session = session_handle.c_str();
    bool valid = append_basic(arguments, DBUS_TYPE_OBJECT_PATH, &session);

    DBusMessageIter shortcuts{};
    DBusMessageIter shortcut{};
    DBusMessageIter shortcut_options{};
    valid = valid && dbus_message_iter_open_container(
                         &arguments, DBUS_TYPE_ARRAY, "(sa{sv})", &shortcuts) != FALSE &&
            dbus_message_iter_open_container(
                &shortcuts, DBUS_TYPE_STRUCT, nullptr, &shortcut) != FALSE;
    const char* identifier = shortcut_id;
    const char* description = "Capture the recent BlackBox incident window";
    const char* trigger = accelerator.c_str();
    valid = valid && append_basic(shortcut, DBUS_TYPE_STRING, &identifier) &&
            dbus_message_iter_open_container(
                &shortcut, DBUS_TYPE_ARRAY, "{sv}", &shortcut_options) != FALSE &&
            append_string_option(shortcut_options, "description", description) &&
            append_string_option(shortcut_options, "preferred_trigger", trigger) &&
            dbus_message_iter_close_container(&shortcut, &shortcut_options) != FALSE &&
            dbus_message_iter_close_container(&shortcuts, &shortcut) != FALSE &&
            dbus_message_iter_close_container(&arguments, &shortcuts) != FALSE;

    const char* parent_window = "";
    valid = valid && append_basic(arguments, DBUS_TYPE_STRING, &parent_window);
    DBusMessageIter options{};
    valid = valid && dbus_message_iter_open_container(
                         &arguments, DBUS_TYPE_ARRAY, "{sv}", &options) != FALSE;
    const auto token = unique_token("blackbox_bind");
    const char* token_value = token.c_str();
    valid = valid && append_string_option(options, "handle_token", token_value) &&
            dbus_message_iter_close_container(&arguments, &options) != FALSE;
    if (!valid) {
        dbus_message_unref(request);
        return false;
    }
    DBusMessage* reply = call(connection, request);
    const auto request_path = reply_object_path(reply);
    if (reply != nullptr) dbus_message_unref(reply);
    return !request_path.empty() && wait_for_response(
        connection, request_path,
        [](DBusMessage* response) { return response_bound_shortcut(response); });
}

void close_session(DBusConnection* connection,
                   const std::string& session_handle) noexcept {
    if (connection == nullptr || session_handle.empty()) return;
    DBusMessage* request = dbus_message_new_method_call(
        portal_service, session_handle.c_str(), session_interface, "Close");
    if (request == nullptr) return;
    static_cast<void>(dbus_connection_send(connection, request, nullptr));
    dbus_connection_flush(connection);
    dbus_message_unref(request);
}

} // namespace

std::string portal_accelerator(const HotkeyCombination combination) {
    if (!valid_combination(combination)) return {};
    std::string result{};
    if (combination.control) result += "CTRL+";
    if (combination.shift) result += "SHIFT+";
    if (combination.alt) result += "ALT+";
    if (combination.windows) result += "LOGO+";
    result += 'F';
    result += std::to_string(static_cast<unsigned>(combination.key));
    return result;
}

struct LinuxGlobalHotkeyManager::NativeState {
    DBusConnection* connection{};
    std::jthread worker{};
    mutable std::mutex mutex{};
    HotkeyCallback callback{};
    std::string session_handle{};
    std::atomic<bool> registered{};

    void listen(const std::stop_token stop_token) noexcept {
        while (!stop_token.stop_requested() && connection != nullptr &&
               dbus_connection_read_write(connection, 100) != FALSE) {
            while (DBusMessage* message = dbus_connection_pop_message(connection)) {
                if (dbus_message_is_signal(
                        message, shortcuts_interface, "Activated") != FALSE) {
                    DBusMessageIter iterator{};
                    const char* session{};
                    const char* identifier{};
                    if (dbus_message_iter_init(message, &iterator) &&
                        dbus_message_iter_get_arg_type(&iterator) == DBUS_TYPE_OBJECT_PATH) {
                        dbus_message_iter_get_basic(&iterator, &session);
                        if (dbus_message_iter_next(&iterator) &&
                            dbus_message_iter_get_arg_type(&iterator) == DBUS_TYPE_STRING) {
                            dbus_message_iter_get_basic(&iterator, &identifier);
                        }
                    }
                    if (session != nullptr && identifier != nullptr &&
                        session_handle == session &&
                        std::string_view{identifier} == shortcut_id) {
                        try {
                            HotkeyCallback activation{};
                            {
                                const std::scoped_lock lock{mutex};
                                activation = callback;
                            }
                            if (activation) activation();
                        } catch (...) {
                            // Portal dispatch cannot unwind into libdbus.
                        }
                    }
                }
                dbus_message_unref(message);
            }
        }
    }
};

LinuxGlobalHotkeyManager::LinuxGlobalHotkeyManager()
    : native_{std::make_unique<NativeState>()} {}

LinuxGlobalHotkeyManager::~LinuxGlobalHotkeyManager() {
    unregister_hotkey();
}

HotkeyRegistrationResult LinuxGlobalHotkeyManager::register_hotkey(
    const HotkeyCombination combination,
    HotkeyCallback callback) {
    unregister_hotkey();
    const auto accelerator = portal_accelerator(combination);
    if (accelerator.empty() || !callback) {
        return HotkeyRegistrationResult::invalid_combination;
    }
    static_cast<void>(dbus_threads_init_default());
    DBusError error{};
    dbus_error_init(&error);
    native_->connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (dbus_error_is_set(&error)) dbus_error_free(&error);
    if (native_->connection == nullptr) return HotkeyRegistrationResult::unavailable;
    dbus_connection_set_exit_on_disconnect(native_->connection, FALSE);
    dbus_bus_add_match(
        native_->connection,
        "type='signal',interface='org.freedesktop.portal.Request',member='Response'",
        nullptr);
    dbus_bus_add_match(
        native_->connection,
        "type='signal',interface='org.freedesktop.portal.GlobalShortcuts',member='Activated'",
        nullptr);
    dbus_connection_flush(native_->connection);

    native_->session_handle = create_session(native_->connection);
    if (native_->session_handle.empty() ||
        !bind_shortcut(native_->connection, native_->session_handle, accelerator)) {
        unregister_hotkey();
        return HotkeyRegistrationResult::unavailable;
    }
    {
        const std::scoped_lock lock{native_->mutex};
        native_->callback = std::move(callback);
    }
    native_->registered.store(true);
    native_->worker = std::jthread{
        [state = native_.get()](const std::stop_token stop_token) {
            state->listen(stop_token);
        }};
    return HotkeyRegistrationResult::registered;
}

void LinuxGlobalHotkeyManager::unregister_hotkey() noexcept {
    native_->registered.store(false);
    if (native_->worker.joinable()) {
        native_->worker.request_stop();
        native_->worker.join();
    }
    close_session(native_->connection, native_->session_handle);
    native_->session_handle.clear();
    {
        const std::scoped_lock lock{native_->mutex};
        native_->callback = {};
    }
    if (native_->connection != nullptr) {
        dbus_connection_close(native_->connection);
        dbus_connection_unref(native_->connection);
        native_->connection = nullptr;
    }
}

bool LinuxGlobalHotkeyManager::registered() const noexcept {
    return native_->registered.load();
}

} // namespace blackbox::platform::linux
