#pragma once

#include <chrono>

namespace blackbox::core {

using MonotonicClock = std::chrono::steady_clock;
using MonotonicTimePoint = MonotonicClock::time_point;

class IMonotonicClock {
public:
    virtual ~IMonotonicClock() = default;

    [[nodiscard]] virtual MonotonicTimePoint now() const noexcept = 0;
};

class SystemMonotonicClock final : public IMonotonicClock {
public:
    [[nodiscard]] MonotonicTimePoint now() const noexcept override {
        return MonotonicClock::now();
    }
};

} // namespace blackbox::core
