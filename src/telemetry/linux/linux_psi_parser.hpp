#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace blackbox::telemetry::linux {

struct LinuxPsiCounters {
    std::uint64_t some_total_microseconds{};
    std::optional<std::uint64_t> full_total_microseconds{};
    friend constexpr bool operator==(const LinuxPsiCounters&, const LinuxPsiCounters&) = default;
};

enum class LinuxPsiParseError : std::uint8_t {
    empty,
    malformed,
    duplicate_record,
    unexpected_record,
    missing_some,
};

[[nodiscard]] std::expected<LinuxPsiCounters, LinuxPsiParseError>
parse_linux_psi(std::string_view contents) noexcept;

} // namespace blackbox::telemetry::linux
