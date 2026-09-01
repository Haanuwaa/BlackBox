#include "telemetry/linux/linux_psi_parser.hpp"

#include <charconv>
#include <cmath>
#include <limits>

namespace blackbox::telemetry::linux {
namespace {

[[nodiscard]] bool parse_unsigned(const std::string_view text, std::uint64_t& value) noexcept {
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_average(const std::string_view text) noexcept {
    if (text.empty()) return false;
    double value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
           std::isfinite(value) && value >= 0.0 && value <= 100.0;
}

[[nodiscard]] bool parse_record(const std::string_view line, std::string_view& name,
                                std::uint64_t& total) noexcept {
    const auto first_space = line.find(' ');
    if (first_space == std::string_view::npos || first_space == 0U) return false;
    name = line.substr(0U, first_space);
    auto remaining = line.substr(first_space + 1U);
    bool avg10{};
    bool avg60{};
    bool avg300{};
    bool found_total{};
    while (!remaining.empty()) {
        const auto separator = remaining.find(' ');
        const auto token = remaining.substr(0U, separator);
        if (token.empty()) return false;
        const auto equals = token.find('=');
        if (equals == std::string_view::npos || equals == 0U || equals + 1U == token.size()) {
            return false;
        }
        const auto key = token.substr(0U, equals);
        const auto value = token.substr(equals + 1U);
        if (key == "avg10") {
            if (avg10 || !parse_average(value)) return false;
            avg10 = true;
        } else if (key == "avg60") {
            if (avg60 || !parse_average(value)) return false;
            avg60 = true;
        } else if (key == "avg300") {
            if (avg300 || !parse_average(value)) return false;
            avg300 = true;
        } else if (key == "total") {
            if (found_total || !parse_unsigned(value, total)) return false;
            found_total = true;
        } else {
            return false;
        }
        if (separator == std::string_view::npos) break;
        remaining.remove_prefix(separator + 1U);
    }
    return avg10 && avg60 && avg300 && found_total;
}

} // namespace

std::expected<LinuxPsiCounters, LinuxPsiParseError>
parse_linux_psi(std::string_view contents) noexcept {
    if (contents.empty() || contents.size() > 4'096U) {
        return std::unexpected{LinuxPsiParseError::empty};
    }
    LinuxPsiCounters result{};
    bool some{};
    bool full{};
    while (!contents.empty()) {
        const auto newline = contents.find('\n');
        auto line = contents.substr(0U, newline);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);
        if (line.empty()) return std::unexpected{LinuxPsiParseError::malformed};
        std::string_view name;
        std::uint64_t total{};
        if (!parse_record(line, name, total)) {
            return std::unexpected{LinuxPsiParseError::malformed};
        }
        if (name == "some") {
            if (some) return std::unexpected{LinuxPsiParseError::duplicate_record};
            some = true;
            result.some_total_microseconds = total;
        } else if (name == "full") {
            if (full) return std::unexpected{LinuxPsiParseError::duplicate_record};
            full = true;
            result.full_total_microseconds = total;
        } else {
            return std::unexpected{LinuxPsiParseError::unexpected_record};
        }
        if (newline == std::string_view::npos) break;
        contents.remove_prefix(newline + 1U);
        if (contents.empty()) break;
    }
    if (!some) return std::unexpected{LinuxPsiParseError::missing_some};
    return result;
}

} // namespace blackbox::telemetry::linux
