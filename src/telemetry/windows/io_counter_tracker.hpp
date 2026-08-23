#pragma once

#include "telemetry/io_counter_tracker.hpp"

namespace blackbox::telemetry::windows {

using ::blackbox::telemetry::IoAggregateCounters;
using ::blackbox::telemetry::IoEntityCounters;

template <std::size_t Capacity = 128U>
using IoCounterTracker = ::blackbox::telemetry::IoCounterTracker<Capacity>;

} // namespace blackbox::telemetry::windows
