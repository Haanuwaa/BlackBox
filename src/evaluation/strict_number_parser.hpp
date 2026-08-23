#pragma once

#include <cmath>
#include <cstddef>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace blackbox::evaluation {

// libc++ exposes floating-point from_chars only on newer Apple deployment
// targets. Artifact parsing remains locale-independent and fail-closed on the
// macOS 15 engineering boundary through this deliberately narrow grammar.
[[nodiscard]] inline std::optional<double> parse_finite_decimal(
    const std::string_view text) {
    if (text.empty() || text.size() > 128U) return std::nullopt;

    std::size_t position{};
    if (text[position] == '-') {
        ++position;
        if (position == text.size()) return std::nullopt;
    }

    bool mantissa_digit{};
    while (position < text.size() && text[position] >= '0' &&
           text[position] <= '9') {
        mantissa_digit = true;
        ++position;
    }
    if (position < text.size() && text[position] == '.') {
        ++position;
        while (position < text.size() && text[position] >= '0' &&
               text[position] <= '9') {
            mantissa_digit = true;
            ++position;
        }
    }
    if (!mantissa_digit) return std::nullopt;

    if (position < text.size() &&
        (text[position] == 'e' || text[position] == 'E')) {
        ++position;
        if (position < text.size() &&
            (text[position] == '+' || text[position] == '-')) {
            ++position;
        }
        const auto exponent_start = position;
        while (position < text.size() && text[position] >= '0' &&
               text[position] <= '9') {
            ++position;
        }
        if (position == exponent_start) return std::nullopt;
    }
    if (position != text.size()) return std::nullopt;

    std::istringstream input{std::string{text}};
    input.imbue(std::locale::classic());
    double value{};
    input >> std::noskipws >> value;
    if (!input || !input.eof() || !std::isfinite(value)) return std::nullopt;
    return value;
}

} // namespace blackbox::evaluation
