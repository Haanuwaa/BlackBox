#include "platform/linux/linux_accessibility.hpp"

#include <dbus/dbus.h>

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <stop_token>
#include <string_view>
#include <thread>

namespace blackbox::platform::linux {
namespace {

constexpr auto portal_service = "org.freedesktop.portal.Desktop";
constexpr auto portal_path = "/org/freedesktop/portal/desktop";
constexpr auto settings_interface = "org.freedesktop.portal.Settings";
constexpr auto appearance_namespace = "org.freedesktop.appearance";

struct SettingReadResult {
  std::optional<std::uint32_t> value{};
  bool unknown_method{};
};

[[nodiscard]] bool append_string(DBusMessageIter &destination,
                                 const char *value) noexcept {
  return dbus_message_iter_append_basic(&destination, DBUS_TYPE_STRING,
                                        &value) != FALSE;
}

[[nodiscard]] SettingReadResult
read_setting_with_method(DBusConnection *connection, const char *method,
                         const char *key) noexcept {
  if (connection == nullptr)
    return {};
  DBusMessage *request = dbus_message_new_method_call(
      portal_service, portal_path, settings_interface, method);
  if (request == nullptr)
    return {};

  DBusMessageIter arguments{};
  dbus_message_iter_init_append(request, &arguments);
  const char *setting_namespace = appearance_namespace;
  const bool valid = append_string(arguments, setting_namespace) &&
                     append_string(arguments, key);
  DBusMessage *reply = valid ? dbus_connection_send_with_reply_and_block(
                                   connection, request, 500, nullptr)
                             : nullptr;
  dbus_message_unref(request);
  if (reply == nullptr)
    return {};

  SettingReadResult result{};
  if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
    const char *error_name = dbus_message_get_error_name(reply);
    result.unknown_method =
        error_name != nullptr &&
        std::strcmp(error_name, DBUS_ERROR_UNKNOWN_METHOD) == 0;
    dbus_message_unref(reply);
    return result;
  }

  DBusMessageIter value{};
  if (dbus_message_iter_init(reply, &value) != FALSE) {
    for (unsigned depth = 0U; depth < 2U && dbus_message_iter_get_arg_type(
                                                &value) == DBUS_TYPE_VARIANT;
         ++depth) {
      DBusMessageIter nested{};
      dbus_message_iter_recurse(&value, &nested);
      value = nested;
    }
    if (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_UINT32) {
      dbus_uint32_t raw{};
      dbus_message_iter_get_basic(&value, &raw);
      result.value = static_cast<std::uint32_t>(raw);
    }
  }
  dbus_message_unref(reply);
  return result;
}

[[nodiscard]] std::optional<std::uint32_t>
read_setting(DBusConnection *connection, const char *key) noexcept {
  const auto current = read_setting_with_method(connection, "ReadOne", key);
  if (current.value.has_value() || !current.unknown_method)
    return current.value;
  return read_setting_with_method(connection, "Read", key).value;
}

[[nodiscard]] DBusConnection *open_portal_connection() noexcept {
  DBusError error{};
  dbus_error_init(&error);
  DBusConnection *connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
  if (dbus_error_is_set(&error))
    dbus_error_free(&error);
  if (connection == nullptr)
    return nullptr;
  dbus_connection_set_exit_on_disconnect(connection, FALSE);

  dbus_error_init(&error);
  const bool available =
      dbus_bus_name_has_owner(connection, portal_service, &error) != FALSE;
  if (dbus_error_is_set(&error))
    dbus_error_free(&error);
  if (!available) {
    dbus_connection_close(connection);
    dbus_connection_unref(connection);
    return nullptr;
  }
  return connection;
}

void close_connection(DBusConnection *&connection) noexcept {
  if (connection == nullptr)
    return;
  dbus_connection_close(connection);
  dbus_connection_unref(connection);
  connection = nullptr;
}

} // namespace

struct LinuxAccessibilityMonitor::NativeState {
  void loop(const std::stop_token stop_token) noexcept {
    DBusConnection *connection{};
    while (!stop_token.stop_requested()) {
      {
        std::unique_lock lock{mutex};
        condition.wait(lock, [this, stop_token] {
          return stop_token.stop_requested() || stopping || refresh_pending;
        });
        if (stop_token.stop_requested() || stopping)
          break;
        refresh_pending = false;
      }

      if (connection == nullptr)
        connection = open_portal_connection();
      const auto contrast = read_setting(connection, "contrast");
      const auto reduced_motion = read_setting(connection, "reduced-motion");
      const bool connected = connection != nullptr;
      if (connection != nullptr &&
          (!dbus_connection_get_is_connected(connection) ||
           (!contrast.has_value() && !reduced_motion.has_value()))) {
        close_connection(connection);
      }

      const std::scoped_lock lock{mutex};
      state.portal_available = connected;
      state.contrast_available = contrast.has_value();
      state.reduced_motion_available = reduced_motion.has_value();
      const auto decoded =
          portal_accessibility_preferences(contrast, reduced_motion);
      if (contrast.has_value()) {
        state.preferences.high_contrast = decoded.high_contrast;
      }
      if (reduced_motion.has_value()) {
        state.preferences.animations_enabled = decoded.animations_enabled;
      }
      ++state.refreshes_completed;
      if (!contrast.has_value() || !reduced_motion.has_value()) {
        ++state.refresh_failures;
      }
    }
    close_connection(connection);
  }

  mutable std::mutex mutex{};
  std::condition_variable condition{};
  std::jthread worker{};
  LinuxAccessibilitySnapshot state{};
  bool started{};
  bool stopping{};
  bool refresh_pending{};
};

LinuxAccessibilityMonitor::LinuxAccessibilityMonitor() noexcept
    : native_state_{new (std::nothrow) NativeState{}} {}

LinuxAccessibilityMonitor::~LinuxAccessibilityMonitor() { stop(); }

bool LinuxAccessibilityMonitor::start() noexcept {
  if (native_state_ == nullptr)
    return false;
  static_cast<void>(dbus_threads_init_default());
  {
    const std::scoped_lock lock{native_state_->mutex};
    if (native_state_->started)
      return true;
    native_state_->stopping = false;
    native_state_->refresh_pending = false;
  }
  try {
    native_state_->worker =
        std::jthread{[state = native_state_.get()](
                         const std::stop_token token) { state->loop(token); }};
  } catch (...) {
    return false;
  }
  const std::scoped_lock lock{native_state_->mutex};
  native_state_->started = true;
  return true;
}

void LinuxAccessibilityMonitor::request_refresh() noexcept {
  if (native_state_ == nullptr)
    return;
  {
    const std::scoped_lock lock{native_state_->mutex};
    if (!native_state_->started || native_state_->stopping)
      return;
    native_state_->refresh_pending = true;
  }
  native_state_->condition.notify_one();
}

void LinuxAccessibilityMonitor::stop() noexcept {
  if (native_state_ == nullptr)
    return;
  {
    const std::scoped_lock lock{native_state_->mutex};
    if (!native_state_->started)
      return;
    native_state_->stopping = true;
  }
  native_state_->worker.request_stop();
  native_state_->condition.notify_all();
  if (native_state_->worker.joinable())
    native_state_->worker.join();
  const std::scoped_lock lock{native_state_->mutex};
  native_state_->started = false;
  native_state_->stopping = false;
  native_state_->refresh_pending = false;
}

LinuxAccessibilitySnapshot
LinuxAccessibilityMonitor::snapshot() const noexcept {
  if (native_state_ == nullptr)
    return {};
  const std::scoped_lock lock{native_state_->mutex};
  return native_state_->state;
}

} // namespace blackbox::platform::linux
