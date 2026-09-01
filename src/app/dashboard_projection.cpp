#include "app/dashboard_projection.hpp"

#include <algorithm>

namespace blackbox::app {
namespace {

[[nodiscard]] bool process_order(const telemetry::ProcessSample& left,
                                 const telemetry::ProcessSample& right) noexcept {
    const auto left_cpu = left.cpu_usage.has_value() ? left.cpu_usage.value.value : -1.0;
    const auto right_cpu = right.cpu_usage.has_value() ? right.cpu_usage.value.value : -1.0;
    if (left_cpu != right_cpu) return left_cpu > right_cpu;
    const auto left_memory = left.working_set.has_value() ? left.working_set.value.value : 0U;
    const auto right_memory = right.working_set.has_value() ? right.working_set.value.value : 0U;
    return left_memory > right_memory;
}

} // namespace

void select_top_dashboard_processes(std::vector<telemetry::ProcessSample>& processes,
                                    const std::size_t maximum_rows) {
    const auto count = std::min(processes.size(), maximum_rows);
    if (count == 0U) return;
    if (count < processes.size()) {
        std::partial_sort(processes.begin(), processes.begin() + static_cast<std::ptrdiff_t>(count),
                          processes.end(), process_order);
    } else {
        std::sort(processes.begin(), processes.end(), process_order);
    }
}

} // namespace blackbox::app
