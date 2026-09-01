#include "platform/linux/linux_portal_message_parser.hpp"

#include <dbus/dbus.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace {

[[nodiscard]] DBusMessage* session_response(const std::uint8_t* data, const std::size_t size) {
    DBusMessage* message = dbus_message_new(DBUS_MESSAGE_TYPE_SIGNAL);
    if (message == nullptr) return nullptr;
    DBusMessageIter arguments{};
    DBusMessageIter entries{};
    DBusMessageIter entry{};
    DBusMessageIter variant{};
    dbus_message_iter_init_append(message, &arguments);
    dbus_uint32_t response = size == 0U ? 0U : static_cast<dbus_uint32_t>(data[0] % 3U);
    const char* key = size > 1U && (data[1] & 1U) != 0U ? "other" : "session_handle";
    const char* value = size > 2U && (data[2] & 1U) != 0U
                            ? "not-an-object-path"
                            : "/org/freedesktop/portal/desktop/session/blackbox";
    const bool valid =
        dbus_message_iter_append_basic(&arguments, DBUS_TYPE_UINT32, &response) != FALSE &&
        dbus_message_iter_open_container(&arguments, DBUS_TYPE_ARRAY, "{sv}", &entries) != FALSE &&
        dbus_message_iter_open_container(&entries, DBUS_TYPE_DICT_ENTRY, nullptr, &entry) != FALSE &&
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) != FALSE &&
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant) != FALSE &&
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value) != FALSE &&
        dbus_message_iter_close_container(&entry, &variant) != FALSE &&
        dbus_message_iter_close_container(&entries, &entry) != FALSE &&
        dbus_message_iter_close_container(&arguments, &entries) != FALSE;
    if (!valid) {
        dbus_message_unref(message);
        return nullptr;
    }
    return message;
}

[[nodiscard]] DBusMessage* shortcut_response(const std::uint8_t* data, const std::size_t size) {
    DBusMessage* message = dbus_message_new(DBUS_MESSAGE_TYPE_SIGNAL);
    if (message == nullptr) return nullptr;
    DBusMessageIter arguments{};
    DBusMessageIter entries{};
    DBusMessageIter entry{};
    DBusMessageIter variant{};
    DBusMessageIter shortcuts{};
    DBusMessageIter shortcut{};
    DBusMessageIter options{};
    dbus_message_iter_init_append(message, &arguments);
    dbus_uint32_t response = size == 0U ? 0U : static_cast<dbus_uint32_t>(data[0] % 3U);
    const char* key = size > 1U && (data[1] & 1U) != 0U ? "other" : "shortcuts";
    const char* identifier = size > 2U && (data[2] & 1U) != 0U
                                 ? "other-shortcut"
                                 : "capture-incident";
    const bool valid =
        dbus_message_iter_append_basic(&arguments, DBUS_TYPE_UINT32, &response) != FALSE &&
        dbus_message_iter_open_container(&arguments, DBUS_TYPE_ARRAY, "{sv}", &entries) != FALSE &&
        dbus_message_iter_open_container(&entries, DBUS_TYPE_DICT_ENTRY, nullptr, &entry) != FALSE &&
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) != FALSE &&
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "a(sa{sv})", &variant) != FALSE &&
        dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "(sa{sv})", &shortcuts) != FALSE &&
        dbus_message_iter_open_container(&shortcuts, DBUS_TYPE_STRUCT, nullptr, &shortcut) != FALSE &&
        dbus_message_iter_append_basic(&shortcut, DBUS_TYPE_STRING, &identifier) != FALSE &&
        dbus_message_iter_open_container(&shortcut, DBUS_TYPE_ARRAY, "{sv}", &options) != FALSE &&
        dbus_message_iter_close_container(&shortcut, &options) != FALSE &&
        dbus_message_iter_close_container(&shortcuts, &shortcut) != FALSE &&
        dbus_message_iter_close_container(&variant, &shortcuts) != FALSE &&
        dbus_message_iter_close_container(&entry, &variant) != FALSE &&
        dbus_message_iter_close_container(&entries, &entry) != FALSE &&
        dbus_message_iter_close_container(&arguments, &entries) != FALSE;
    if (!valid) {
        dbus_message_unref(message);
        return nullptr;
    }
    return message;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    constexpr std::size_t maximum_input_size = 4U * 1024U;
    if (data == nullptr || size > maximum_input_size) return 0;

    if (auto* message = session_response(data, size); message != nullptr) {
        std::string handle{};
        static_cast<void>(blackbox::platform::linux::response_session_handle(message, handle));
        dbus_message_unref(message);
    }
    if (auto* message = shortcut_response(data, size); message != nullptr) {
        static_cast<void>(blackbox::platform::linux::response_bound_shortcut(message));
        dbus_message_unref(message);
    }
    return 0;
}
