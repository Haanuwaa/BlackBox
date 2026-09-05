#pragma once

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace blackbox::core {

// Environment variable names used here are ASCII. Values are native paths;
// never round-trip a Windows profile/settings path through the ANSI code page.
[[nodiscard]] inline std::optional<std::filesystem::path>
environment_path(const std::string_view name) {
#if defined(_WIN32)
    const std::wstring native_name{name.begin(), name.end()};
    std::size_t required{};
    if (_wgetenv_s(&required, nullptr, 0U, native_name.c_str()) != 0 || required <= 1U)
        return std::nullopt;
    std::wstring value(required, L'\0');
    if (_wgetenv_s(&required, value.data(), value.size(), native_name.c_str()) != 0)
        return std::nullopt;
    value.resize(required - 1U);
    return std::filesystem::path{value};
#else
    const std::string terminated{name};
    const auto* value = std::getenv(terminated.c_str());
    if (value == nullptr || *value == '\0') return std::nullopt;
    return std::filesystem::path{value};
#endif
}

} // namespace blackbox::core
