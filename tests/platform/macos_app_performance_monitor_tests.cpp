#include "platform/macos/macos_app_performance_monitor.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("macOS app performance monitor is idempotent and bounded") {
    blackbox::platform::macos::MacosAppPerformanceMonitor monitor;
    monitor.start();
    monitor.start();
    const auto snapshot = monitor.snapshot();
    CHECK(snapshot.status !=
          blackbox::platform::macos::AppPerformanceReportStatus::failed);
    monitor.stop();
    monitor.stop();
}
