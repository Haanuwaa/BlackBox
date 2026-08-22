#pragma once

#include <string_view>

namespace blackbox::core {

inline constexpr std::string_view version = BLACKBOX_VERSION;
inline constexpr std::string_view source_revision = BLACKBOX_SOURCE_REVISION;

} // namespace blackbox::core
