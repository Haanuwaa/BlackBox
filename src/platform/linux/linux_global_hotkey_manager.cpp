#include "platform/linux/linux_global_hotkey_manager.hpp"

#include <dbus/dbus.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
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
constexpr char properties_interface[] = "org.freedesktop.DBus.Properties";
constexpr char bus_interface[] = "org.freedesktop.DBus";
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
                                DBusMessage* request,
                                const int timeout_milliseconds =
                                    portal_timeout_milliseconds) noexcept {
    if (connection == nullptr || request == nullptr) return nullptr;
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        connection, request, timeout_milliseconds, nullptr);
    dbus_message_unref(request);
    if (reply != nullptr && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        dbus_message_unref(reply);
        return nullptr;
    }
    return reply;
}

[[nodiscard]] std::uint32_t portal_version(DBusConnection* connection) noexcept {
    DBusMessage* request = dbus_message_new_method_call(
        portal_service, portal_path, properties_interface, "Get");
    if (request == nullptr) return 0U;
    DBusMessageIter arguments{};
    dbus_message_iter_init_append(request, &arguments);
    const char* interface_name = shortcuts_interface;
    const char* property_name = "version";
    if (!append_basic(arguments, DBUS_TYPE_STRING, &interface_name) ||
        !append_basic(arguments, DBUS_TYPE_STRING, &property_name)) {
        dbus_message_unref(request);
        return 0U;
    }
    DBusMessage* reply = call(connection, request, 500);
    if (reply == nullptr) return 0U;
    DBusMessageIter value{};
    bool valid = dbus_message_iter_init(reply, &value) != FALSE;
    if (valid && dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_VARIANT) {
        DBusMessageIter nested{};
        dbus_message_iter_recurse(&value, &nested);
        value = nested;
    }
    dbus_uint32_t result{};
    valid = valid && dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_UINT32;
    if (valid) dbus_message_iter_get_basic(&value, &result);
    dbus_message_unref(reply);
    return valid ? static_cast<std::uint32_t>(result) : 0U;
}

[[nodiscard]] bool add_match(DBusConnection* connection,
                             const char* rule) noexcept {
    DBusError error{};
    dbus_error_init(&error);
    dbus_bus_add_match(connection, rule, &error);
    const bool valid = !dbus_error_is_set(&error);
    if (!valid) dbus_error_free(&error);
    return valid;
}

[[nodiscard]] bool service_owner_changed(DBusMessage* message) noexcept {
    if (dbus_message_is_signal(message, bus_interface, "NameOwnerChanged") == FALSE) {
        return false;
    }
    DBusMessageIter iterator{};
    if (!dbus_message_iter_init(message, &iterator) ||
        dbus_message_iter_get_arg_type(&iterator) != DBUS_TYPE_STRING) {
        return false;
    }
    const char* name{};
    dbus_message_iter_get_basic(&iterator, &name);
    return name != nullptr && std::strcmp(name, portal_service) == 0;
}

[[nodiscard]] bool matching_session_signal(DBusMessage* message,
                                           const char* interface_name,
                                           const char* member,
                                           const std::string& session_handle) noexcept {
    if (dbus_message_is_signal(message, interface_name, member) == FALSE) return false;
    const char* path = dbus_message_get_path(message);
    if (std::string_view{interface_name} == session_interface) {
        return path != nullptr && session_handle == path;
    }
    DBusMessageIter iterator{};
    if (!dbus_message_iter_init(message, &iterator) ||
        dbus_message_iter_get_arg_type(&iterator) != DBUS_TYPE_OBJECT_PATH) {
        return false;
    }
    const char* session{};
    dbus_message_iter_get_basic(&iterator, &session);
    return session != nullptr && session_handle == session;
}

[[nodiscard]] bool shortcut_signal_contains_capture(DBusMessage* message) noexcept {
    DBusMessageIter iterator{};
    if (!dbus_message_iter_init(message, &iterator) ||
        dbus_message_iter_get_arg_type(&iterator) != DBUS_TYPE_OBJECT_PATH ||
        !dbus_message_iter_next(&iterator) ||
        dbus_message_iter_get_arg_type(&iterator) != DBUS_TYPE_ARRAY) {
        return false;
    }
    DBusMessageIter shortcuts{};
    dbus_message_iter_recurse(&iterator, &shortcuts);
    while (dbus_message_iter_get_arg_type(&shortcuts) == DBUS_TYPE_STRUCT) {
        DBusMessageIter shortcut{};
        dbus_message_iter_recurse(&shortcuts, &shortcut);
        const char* identifier{};
        if (dbus_message_iter_get_arg_type(&shortcut) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&shortcut, &identifier);
        }
        if (identifier != nullptr && std::string_view{identifier} == shortcut_id) {
            return true;
        }
        dbus_message_iter_next(&shortcuts);
    }
    return false;
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
    if (connection == nullptr || session_handle.empty() ||
        dbus_connection_get_is_connected(connection) == FALSE) {
        return;
    }
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
    std::condition_variable_any retry_condition{};
    HotkeyCallback callback{};
    std::string accelerator{};
    std::string session_handle{};
    std::atomic<bool> registered{};
    LinuxGlobalHotkeyDiagnostics diagnostics{};

    void transition(const PortalShortcutEvent event) noexcept {
        const std::scoped_lock lock{mutex};
        diagnostics.state = portal_shortcut_transition(diagnostics.state, event);
        registered.store(diagnostics.state == PortalShortcutState::active,
                         std::memory_order_release);
    }

    void close_connection() noexcept {
        close_session(connection, session_handle);
        session_handle.clear();
        if (connection != nullptr) {
            dbus_connection_close(connection);
            dbus_connection_unref(connection);
            connection = nullptr;
        }
    }

    [[nodiscard]] bool establish_session() noexcept {
        close_connection();
        DBusError error{};
        dbus_error_init(&error);
        connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
        if (dbus_error_is_set(&error)) dbus_error_free(&error);
        if (connection == nullptr) return false;
        dbus_connection_set_exit_on_disconnect(connection, FALSE);

        dbus_error_init(&error);
        const bool owner = dbus_bus_name_has_owner(
            connection, portal_service, &error) != FALSE;
        if (dbus_error_is_set(&error)) dbus_error_free(&error);
        const auto version = owner ? portal_version(connection) : 0U;
        const bool matches = owner && version >= 1U &&
            add_match(connection,
                "type='signal',interface='org.freedesktop.portal.Request',member='Response'") &&
            add_match(connection,
                "type='signal',interface='org.freedesktop.portal.GlobalShortcuts'") &&
            add_match(connection,
                "type='signal',interface='org.freedesktop.portal.Session',member='Closed'") &&
            add_match(connection,
                "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged',"
                "arg0='org.freedesktop.portal.Desktop'");
        if (!matches) {
            close_connection();
            return false;
        }
        dbus_connection_flush(connection);
        session_handle = create_session(connection);
        if (session_handle.empty() ||
            !bind_shortcut(connection, session_handle, accelerator)) {
            close_connection();
            return false;
        }
        {
            const std::scoped_lock lock{mutex};
            diagnostics.portal_version = version;
        }
        transition(PortalShortcutEvent::session_established);
        return true;
    }

    void mark_session_lost() noexcept {
        {
            const std::scoped_lock lock{mutex};
            ++diagnostics.session_losses;
        }
        transition(PortalShortcutEvent::session_lost);
        close_connection();
    }

    [[nodiscard]] bool capture_activated(DBusMessage* message) noexcept {
        if (!matching_session_signal(message, shortcuts_interface, "Activated",
                                     session_handle)) {
            return false;
        }
        DBusMessageIter iterator{};
        if (!dbus_message_iter_init(message, &iterator) ||
            !dbus_message_iter_next(&iterator) ||
            dbus_message_iter_get_arg_type(&iterator) != DBUS_TYPE_STRING) {
            return false;
        }
        const char* identifier{};
        dbus_message_iter_get_basic(&iterator, &identifier);
        return identifier != nullptr && std::string_view{identifier} == shortcut_id;
    }

    void dispatch_activation() noexcept {
        try {
            HotkeyCallback activation{};
            {
                const std::scoped_lock lock{mutex};
                activation = callback;
                ++diagnostics.activations;
            }
            if (activation) activation();
        } catch (...) {
            // Portal dispatch cannot unwind into libdbus.
        }
    }

    [[nodiscard]] bool process_messages() noexcept {
        while (DBusMessage* message = dbus_connection_pop_message(connection)) {
            bool session_lost = service_owner_changed(message) ||
                matching_session_signal(message, session_interface, "Closed",
                                        session_handle);
            if (!session_lost && capture_activated(message) && registered.load(
                    std::memory_order_acquire)) {
                dispatch_activation();
            }
            if (!session_lost && matching_session_signal(
                    message, shortcuts_interface, "ShortcutsChanged", session_handle)) {
                const bool retained = shortcut_signal_contains_capture(message);
                {
                    const std::scoped_lock lock{mutex};
                    ++diagnostics.shortcut_changes;
                }
                transition(retained ? PortalShortcutEvent::shortcut_restored
                                    : PortalShortcutEvent::shortcut_removed);
            }
            dbus_message_unref(message);
            if (session_lost) return false;
        }
        return true;
    }

    void listen(const std::stop_token stop_token) noexcept {
        std::uint32_t retry_attempt{};
        while (!stop_token.stop_requested()) {
            if (connection != nullptr &&
                dbus_connection_read_write(connection, 100) != FALSE &&
                process_messages()) {
                retry_attempt = 0U;
                continue;
            }

            if (connection != nullptr) mark_session_lost();
            const auto delay = portal_reconnect_delay(retry_attempt++);
            std::unique_lock lock{mutex};
            const bool stopped = retry_condition.wait_for(
                lock, stop_token, delay, [] { return false; });
            if (stopped || stop_token.stop_requested()) break;
            ++diagnostics.reconnect_attempts;
            lock.unlock();
            if (establish_session()) {
                const std::scoped_lock success_lock{mutex};
                ++diagnostics.reconnect_successes;
                retry_attempt = 0U;
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
    {
        const std::scoped_lock lock{native_->mutex};
        native_->callback = std::move(callback);
        native_->accelerator = accelerator;
    }
    if (!native_->establish_session()) {
        unregister_hotkey();
        return HotkeyRegistrationResult::unavailable;
    }
    native_->worker = std::jthread{
        [state = native_.get()](const std::stop_token stop_token) {
            state->listen(stop_token);
        }};
    return HotkeyRegistrationResult::registered;
}

void LinuxGlobalHotkeyManager::unregister_hotkey() noexcept {
    native_->transition(PortalShortcutEvent::stop);
    if (native_->worker.joinable()) {
        native_->worker.request_stop();
        native_->retry_condition.notify_all();
        native_->worker.join();
    }
    native_->close_connection();
    {
        const std::scoped_lock lock{native_->mutex};
        native_->callback = {};
        native_->accelerator.clear();
        native_->diagnostics.portal_version = 0U;
    }
}

bool LinuxGlobalHotkeyManager::registered() const noexcept {
    return native_->registered.load(std::memory_order_acquire);
}

LinuxGlobalHotkeyDiagnostics LinuxGlobalHotkeyManager::diagnostics() const noexcept {
    const std::scoped_lock lock{native_->mutex};
    return native_->diagnostics;
}

} // namespace blackbox::platform::linux
