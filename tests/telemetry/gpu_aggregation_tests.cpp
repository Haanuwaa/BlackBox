#include "telemetry/gpu_aggregation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

namespace telemetry = blackbox::telemetry;

TEST_CASE("GPU aggregation keeps busiest-device semantics and sums memory",
          "[telemetry][gpu][aggregation]") {
  const std::array devices{
      telemetry::GpuDeviceReading{
          11U, telemetry::MetricValue<telemetry::Ratio>::available({0.25}),
          telemetry::MetricValue<telemetry::ByteCount>::available({512U})},
      telemetry::GpuDeviceReading{
          12U, telemetry::MetricValue<telemetry::Ratio>::available({0.75}),
          telemetry::MetricValue<telemetry::ByteCount>::available({1024U})}};

  const auto result = telemetry::aggregate_gpu_devices(devices);
  REQUIRE(result.busiest_engine_usage.has_value());
  CHECK(result.busiest_engine_usage.value.value == 0.75);
  REQUIRE(result.dedicated_memory_used.has_value());
  CHECK(result.dedicated_memory_used.value.value == 1536U);
}

TEST_CASE("GPU aggregation preserves useful partial multi-device evidence",
          "[telemetry][gpu][aggregation]") {
  const std::array devices{
      telemetry::GpuDeviceReading{
          21U, telemetry::MetricValue<telemetry::Ratio>::available({0.5}),
          telemetry::MetricValue<telemetry::ByteCount>::unavailable(
              telemetry::MetricStatus::unsupported)},
      telemetry::GpuDeviceReading{
          22U,
          telemetry::MetricValue<telemetry::Ratio>::unavailable(
              telemetry::MetricStatus::inaccessible),
          telemetry::MetricValue<telemetry::ByteCount>::available({2048U})}};

  const auto result = telemetry::aggregate_gpu_devices(devices);
  REQUIRE(result.busiest_engine_usage.has_value());
  CHECK(result.busiest_engine_usage.value.value == 0.5);
  REQUIRE(result.dedicated_memory_used.has_value());
  CHECK(result.dedicated_memory_used.value.value == 2048U);
}

TEST_CASE(
    "GPU aggregation rejects duplicate identities and checked-sum overflow",
    "[telemetry][gpu][aggregation][robustness]") {
  const std::array devices{
      telemetry::GpuDeviceReading{
          31U, telemetry::MetricValue<telemetry::Ratio>::available({0.2}),
          telemetry::MetricValue<telemetry::ByteCount>::available(
              {(std::numeric_limits<std::uint64_t>::max)()})},
      telemetry::GpuDeviceReading{
          32U, telemetry::MetricValue<telemetry::Ratio>::available({0.3}),
          telemetry::MetricValue<telemetry::ByteCount>::available({1U})},
      telemetry::GpuDeviceReading{
          31U, telemetry::MetricValue<telemetry::Ratio>::available({0.9}),
          telemetry::MetricValue<telemetry::ByteCount>::available({10U})}};

  const auto result = telemetry::aggregate_gpu_devices(devices);
  REQUIRE(result.busiest_engine_usage.has_value());
  CHECK(result.busiest_engine_usage.value.value == 0.3);
  CHECK(result.dedicated_memory_used.status ==
        telemetry::MetricStatus::temporarily_unavailable);
}

TEST_CASE("GPU aggregation reports inaccessible when no device is readable",
          "[telemetry][gpu][permission]") {
  const std::array devices{
      telemetry::GpuDeviceReading{
          41U,
          telemetry::MetricValue<telemetry::Ratio>::unavailable(
              telemetry::MetricStatus::unsupported),
          telemetry::MetricValue<telemetry::ByteCount>::unavailable(
              telemetry::MetricStatus::unsupported)},
      telemetry::GpuDeviceReading{
          42U,
          telemetry::MetricValue<telemetry::Ratio>::unavailable(
              telemetry::MetricStatus::inaccessible),
          telemetry::MetricValue<telemetry::ByteCount>::unavailable(
              telemetry::MetricStatus::inaccessible)}};

  const auto result = telemetry::aggregate_gpu_devices(devices);
  CHECK(result.busiest_engine_usage.status ==
        telemetry::MetricStatus::inaccessible);
  CHECK(result.dedicated_memory_used.status ==
        telemetry::MetricStatus::inaccessible);
}

TEST_CASE("GPU inventory contract rejects identifying-count inconsistencies",
          "[telemetry][gpu][inventory][contract]") {
  telemetry::PlatformCapabilities capabilities{};
  capabilities.gpu_inventory = true;
  telemetry::GpuInventoryEvidence valid{};
  valid.device_count = telemetry::MetricValue<std::uint32_t>::available(2U);
  valid.integrated_device_count =
      telemetry::MetricValue<std::uint32_t>::available(1U);
  valid.discrete_device_count =
      telemetry::MetricValue<std::uint32_t>::available(1U);
  valid.unknown_device_count =
      telemetry::MetricValue<std::uint32_t>::available(0U);
  valid.render_device_available = telemetry::MetricValue<bool>::available(true);
  CHECK(telemetry::validate_gpu_inventory_contract(capabilities, valid) ==
        telemetry::ProviderContractViolation::none);

  valid.discrete_device_count.value = 2U;
  CHECK(telemetry::validate_gpu_inventory_contract(capabilities, valid) ==
        telemetry::ProviderContractViolation::invalid_gpu_inventory);

  capabilities.gpu_inventory = false;
  CHECK(telemetry::validate_gpu_inventory_contract(capabilities, valid) ==
        telemetry::ProviderContractViolation::capability_status_mismatch);
}
