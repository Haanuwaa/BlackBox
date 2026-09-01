#pragma once

#include <string>

struct DBusMessage;

namespace blackbox::platform::linux {

// Bounded typed-message decoders shared by the portal adapter, deterministic
// tests, and the native structured-message fuzzer. Native DBus objects never
// cross the platform implementation boundary.
[[nodiscard]] bool response_session_handle(DBusMessage* message,
                                           std::string& session_handle);
[[nodiscard]] bool response_bound_shortcut(DBusMessage* message) noexcept;

} // namespace blackbox::platform::linux
