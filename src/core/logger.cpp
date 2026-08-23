#include "core/logger.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <mutex>
#include <utility>

namespace blackbox::core {
namespace {

std::mutex sink_mutex;
std::mutex output_mutex;
LogSink active_sink;
const auto process_log_epoch = std::chrono::steady_clock::now();

constexpr std::size_t maximum_component_bytes = 32U;
constexpr std::size_t maximum_message_bytes = 2'048U;

template <std::size_t Capacity>
struct BoundedText final {
    std::array<char, Capacity + 1U> bytes{};
    std::size_t size{};

    [[nodiscard]] std::string_view view() const noexcept {
        return {bytes.data(), size};
    }
};

template <std::size_t Capacity>
[[nodiscard]] BoundedText<Capacity> clean_text(
    const std::string_view input, const std::string_view fallback) noexcept {
    BoundedText<Capacity> result{};
    bool pending_space{};
    bool truncated{};
    for (const unsigned char byte : input) {
        const bool whitespace = byte == ' ' || byte == '\t' || byte == '\r' ||
                                byte == '\n';
        if (whitespace) {
            pending_space = result.size != 0U;
            continue;
        }
        if (pending_space) {
            if (result.size == Capacity) {
                truncated = true;
                break;
            }
            result.bytes[result.size++] = ' ';
            pending_space = false;
        }
        if (result.size == Capacity) {
            truncated = true;
            break;
        }
        result.bytes[result.size++] = byte < 0x20U || byte == 0x7FU
                                          ? '?'
                                          : static_cast<char>(byte);
    }
    if (result.size == 0U) {
        for (const char byte : fallback) {
            if (result.size == Capacity) break;
            result.bytes[result.size++] = byte;
        }
    }
    if (truncated && Capacity >= 3U) {
        result.size = Capacity;
        result.bytes[Capacity - 3U] = '.';
        result.bytes[Capacity - 2U] = '.';
        result.bytes[Capacity - 1U] = '.';
    }
    result.bytes[result.size] = '\0';
    return result;
}

constexpr std::string_view level_name(const LogLevel level) noexcept {
    switch (level) {
    case LogLevel::debug:
        return "debug";
    case LogLevel::info:
        return "info";
    case LogLevel::warning:
        return "warning";
    case LogLevel::error:
        return "error";
    }
    return "unknown";
}

} // namespace

void Logger::set_sink(LogSink sink) {
    const std::scoped_lock lock{sink_mutex};
    active_sink = std::move(sink);
}

void Logger::write(const LogLevel level, const std::string_view component,
                   const std::string_view message) {
    const auto clean_component = clean_text<maximum_component_bytes>(
        component, "general");
    const auto clean_message = clean_text<maximum_message_bytes>(
        message, "(empty)");
    LogSink sink;
    {
        const std::scoped_lock lock{sink_mutex};
        sink = active_sink;
    }
    if (sink) {
        sink(level, clean_component.view(), clean_message.view());
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - process_log_epoch).count();
    const std::scoped_lock lock{output_mutex};
    std::clog << "[blackbox +" << elapsed << "ms] [" << level_name(level)
              << "] [" << clean_component.view() << "] "
              << clean_message.view() << '\n';
    if (level == LogLevel::warning || level == LogLevel::error) {
        std::clog.flush();
    }
}

void Logger::write(const LogLevel level, const std::string_view message) {
    write(level, "application", message);
}

} // namespace blackbox::core
