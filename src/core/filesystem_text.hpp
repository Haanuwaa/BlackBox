#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace blackbox::core {

[[nodiscard]] inline std::string path_to_utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

[[nodiscard]] inline std::filesystem::path path_from_utf8(const std::string_view text) {
    return std::filesystem::path{
        std::u8string{reinterpret_cast<const char8_t*>(text.data()), text.size()}};
}

} // namespace blackbox::core
