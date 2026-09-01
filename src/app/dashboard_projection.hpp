#pragma once

#include "telemetry/types.hpp"

#include <cstddef>
#include <vector>

namespace blackbox::app {

// Reorders only the requested prefix. Work is O(N log K), where K is the
// bounded number of rows the dashboard can display.
void select_top_dashboard_processes(std::vector<telemetry::ProcessSample>& processes,
                                    std::size_t maximum_rows);

} // namespace blackbox::app
