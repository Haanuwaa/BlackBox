#pragma once

#include "telemetry/types.hpp"

#include <memory>

namespace blackbox::telemetry::linux {

class LinuxX11ForegroundReader final {
public:
    LinuxX11ForegroundReader() noexcept;
    ~LinuxX11ForegroundReader();

    LinuxX11ForegroundReader(const LinuxX11ForegroundReader&) = delete;
    LinuxX11ForegroundReader& operator=(const LinuxX11ForegroundReader&) = delete;

    [[nodiscard]] MetricValue<ProcessId> read() noexcept;

private:
    struct State;
    std::unique_ptr<State> state_{};
};

} // namespace blackbox::telemetry::linux
