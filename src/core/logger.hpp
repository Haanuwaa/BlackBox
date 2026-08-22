#pragma once

#include <functional>
#include <string_view>

namespace blackbox::core {

enum class LogLevel {
    debug,
    info,
    warning,
    error,
};

using LogSink = std::function<void(LogLevel, std::string_view)>;

class Logger final {
public:
    static void set_sink(LogSink sink);
    static void write(LogLevel level, std::string_view message);
};

} // namespace blackbox::core
