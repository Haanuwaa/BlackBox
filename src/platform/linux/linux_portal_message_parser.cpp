#include "platform/linux/linux_portal_message_parser.hpp"

#include <dbus/dbus.h>

#include <string_view>

namespace blackbox::platform::linux {
namespace {

constexpr char shortcut_id[] = "capture-incident";

} // namespace

bool response_session_handle(DBusMessage* message, std::string& session_handle) {
    if (message == nullptr) return false;
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

bool response_bound_shortcut(DBusMessage* message) noexcept {
    if (message == nullptr) return false;
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
                if (identifier != nullptr && std::string_view{identifier} == shortcut_id) {
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

} // namespace blackbox::platform::linux
