#include "app/dashboard_projection.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

namespace app = blackbox::app;
namespace telemetry = blackbox::telemetry;

TEST_CASE("dashboard projection selects a bounded deterministic process prefix",
          "[app][dashboard][projection][performance]") {
    std::vector<telemetry::ProcessSample> processes(200U);
    for (std::size_t index = 0U; index < processes.size(); ++index) {
        auto& process = processes[index];
        process.identity = {telemetry::ProcessId{static_cast<std::uint32_t>(index + 1U)},
                            index + 10U};
        process.cpu_usage = telemetry::MetricValue<telemetry::Ratio>::available(
            telemetry::Ratio{static_cast<double>(index % 100U) / 100.0});
        process.working_set = telemetry::MetricValue<telemetry::ByteCount>::available(
            telemetry::ByteCount{index * 1'024U});
    }

    app::select_top_dashboard_processes(processes, 50U);

    REQUIRE(processes.size() == 200U);
    for (std::size_t index = 1U; index < 50U; ++index) {
        const auto& previous = processes[index - 1U];
        const auto& current = processes[index];
        CHECK(previous.cpu_usage.value.value >= current.cpu_usage.value.value);
        if (previous.cpu_usage.value.value == current.cpu_usage.value.value) {
            CHECK(previous.working_set.value.value >= current.working_set.value.value);
        }
    }
    CHECK(processes.front().working_set.value.value == 199U * 1'024U);
}
