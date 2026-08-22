#include "core/logger.hpp"

#include <iostream>
#include <mutex>
#include <utility>

namespace blackbox::core {
namespace {

std::mutex sink_mutex;
LogSink active_sink;

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

void Logger::write(const LogLevel level, const std::string_view message) {
    const std::scoped_lock lock{sink_mutex};
    if (active_sink) {
        active_sink(level, message);
        return;
    }
    std::clog << "[blackbox] [" << level_name(level) << "] " << message << '\n';
}

} // namespace blackbox::core
