#include "telemetry/linux/linux_gpu_collector.hpp"
#include "telemetry/linux/linux_gpu_parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/types.h>
#include <unistd.h>

namespace core = blackbox::core;
namespace linux_gpu = blackbox::telemetry::linux;
namespace telemetry = blackbox::telemetry;

namespace {

class TemporaryTree final {
public:
  TemporaryTree()
      : root{std::filesystem::temp_directory_path() /
             ("blackbox-gpu-" + std::to_string(::getpid()))} {
    std::error_code error{};
    std::filesystem::remove_all(root, error);
    REQUIRE(std::filesystem::create_directories(root));
  }
  ~TemporaryTree() {
    std::error_code error{};
    std::filesystem::permissions(root, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add, error);
    std::filesystem::remove_all(root, error);
  }
  TemporaryTree(const TemporaryTree &) = delete;
  TemporaryTree &operator=(const TemporaryTree &) = delete;

  std::filesystem::path root;
};

void write_file(const std::filesystem::path &path,
                const std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  REQUIRE(output.good());
  output << contents;
  REQUIRE(output.good());
}

void add_card(const std::filesystem::path &drm_root,
              const std::string_view card, const std::string_view driver) {
  const auto device = drm_root / card / "device";
  std::filesystem::create_directories(device);
  std::error_code error{};
  std::filesystem::create_directory_symlink(
      std::filesystem::path{"/drivers"} / driver, device / "driver", error);
  REQUIRE_FALSE(error);
}

} // namespace

TEST_CASE("Linux GPU scalar parsers reject malformed and out-of-range evidence",
          "[telemetry][linux][gpu][parser]") {
  REQUIRE(linux_gpu::parse_gpu_busy_percent("73\n"));
  CHECK(linux_gpu::parse_gpu_busy_percent("73\n")->value == 0.73);
  CHECK_FALSE(linux_gpu::parse_gpu_busy_percent("101"));
  CHECK_FALSE(linux_gpu::parse_gpu_busy_percent("-1"));
  REQUIRE(linux_gpu::parse_gpu_memory_bytes("2048 KiB\n"));
  CHECK(linux_gpu::parse_gpu_memory_bytes("2048 KiB\n")->value ==
        2U * 1024U * 1024U);
  CHECK_FALSE(linux_gpu::parse_gpu_memory_bytes("12 bananas"));
}

TEST_CASE("DRM fdinfo parser emits only bounded hashed engine evidence",
          "[telemetry][linux][gpu][fdinfo][privacy]") {
  const auto parsed =
      linux_gpu::parse_drm_fdinfo("drm-driver:\ti915\n"
                                  "drm-client-id:\t7\n"
                                  "drm-pdev:\t0000:00:02.0\n"
                                  "drm-engine-render:\t250000000 ns\n"
                                  "drm-engine-capacity-render:\t1\n"
                                  "drm-resident-vram0:\t16 MiB\n",
                                  9001U);
  REQUIRE(parsed);
  REQUIRE(parsed->engines.size() == 1U);
  CHECK(parsed->engines.front().private_counter_identity != 0U);
  CHECK(parsed->engines.front().private_engine_identity != 0U);
  CHECK(parsed->engines.front().busy == 250000000U);
  REQUIRE(parsed->resident_vram.has_value());
  CHECK(parsed->resident_vram.value.value == 16U * 1024U * 1024U);
}

TEST_CASE(
    "DRM activity handles multiple engines resets and hotplug without spikes",
    "[telemetry][linux][gpu][fdinfo][reset][hotplug]") {
  linux_gpu::DrmActivityTracker tracker{};
  const auto start = core::MonotonicTimePoint{} + std::chrono::seconds{10};
  const std::array first{
      linux_gpu::DrmEngineCounter{1U, 101U, 100U, std::nullopt, 1U},
      linux_gpu::DrmEngineCounter{2U, 102U, 100U, std::nullopt, 1U}};
  CHECK(tracker.update(start, first).status ==
        telemetry::MetricStatus::temporarily_unavailable);

  const std::array second{
      linux_gpu::DrmEngineCounter{1U, 101U, 300000100U, std::nullopt, 1U},
      linux_gpu::DrmEngineCounter{2U, 102U, 700000100U, std::nullopt, 1U}};
  const auto measured = tracker.update(start + std::chrono::seconds{1}, second);
  REQUIRE(measured.has_value());
  CHECK(measured.value.value == 0.7);

  const std::array reset{
      linux_gpu::DrmEngineCounter{1U, 101U, 2U, std::nullopt, 1U}};
  CHECK(tracker.update(start + std::chrono::seconds{2}, reset).status ==
        telemetry::MetricStatus::temporarily_unavailable);
  const std::array replacement{
      linux_gpu::DrmEngineCounter{3U, 103U, 1000U, std::nullopt, 1U}};
  CHECK(tracker.update(start + std::chrono::seconds{3}, replacement).status ==
        telemetry::MetricStatus::temporarily_unavailable);
}

TEST_CASE(
    "Linux GPU collector discovers AMD devices and refreshes hotplug inventory",
    "[telemetry][linux][gpu][amd][inventory][hotplug]") {
  TemporaryTree tree{};
  const auto drm = tree.root / "drm";
  const auto proc = tree.root / "proc";
  std::filesystem::create_directories(drm / "renderD128");
  std::filesystem::create_directories(proc);
  add_card(drm, "card0", "amdgpu");
  write_file(drm / "card0/device/gpu_busy_percent", "75\n");
  write_file(drm / "card0/device/mem_info_vram_used", "1048576\n");

  linux_gpu::LinuxGpuCollector collector{{drm, proc, false}};
  const auto inventory = collector.inventory();
  REQUIRE(inventory.device_count.has_value());
  CHECK(inventory.device_count.value == 1U);
  CHECK(inventory.discrete_device_count.value == 1U);
  CHECK(inventory.render_device_available.value);
  const auto sample =
      collector.collect(core::MonotonicTimePoint{}, std::nullopt, false);
  REQUIRE(sample.system.busiest_engine_usage.has_value());
  CHECK(sample.system.busiest_engine_usage.value.value == 0.75);
  REQUIRE(sample.system.dedicated_memory_used.has_value());
  CHECK(sample.system.dedicated_memory_used.value.value == 1048576U);

  add_card(drm, "card1", "i915");
  static_cast<void>(
      collector.collect(core::MonotonicTimePoint{}, std::nullopt, true));
  REQUIRE(collector.inventory().device_count.has_value());
  CHECK(collector.inventory().device_count.value == 2U);
}

TEST_CASE("Linux NVIDIA backend is optional and dynamically loads NVML",
          "[telemetry][linux][gpu][nvidia][nvml]") {
  TemporaryTree tree{};
  const auto drm = tree.root / "drm";
  const auto proc = tree.root / "proc";
  std::filesystem::create_directories(drm);
  std::filesystem::create_directories(proc);
  linux_gpu::LinuxGpuCollector collector{
      {drm, proc, true, std::filesystem::path{BLACKBOX_FAKE_NVML_PATH}}};

  const auto sample =
      collector.collect(core::MonotonicTimePoint{}, std::nullopt, false);
  REQUIRE(sample.system.busiest_engine_usage.has_value());
  CHECK(sample.system.busiest_engine_usage.value.value == 0.8);
  REQUIRE(sample.system.dedicated_memory_used.has_value());
  CHECK(sample.system.dedicated_memory_used.value.value == 3U * 1024U * 1024U);
  REQUIRE(collector.inventory().device_count.has_value());
  CHECK(collector.inventory().device_count.value == 2U);
  CHECK(collector.supports_system_usage());
  CHECK(collector.supports_memory());
}

TEST_CASE(
    "Linux DRM foreground sampling is bounded and warms after one interval",
    "[telemetry][linux][gpu][fdinfo][foreground]") {
  TemporaryTree tree{};
  const auto drm = tree.root / "drm";
  const auto proc = tree.root / "proc";
  add_card(drm, "card0", "i915");
  const auto fdinfo = proc / "77/fdinfo/9";
  write_file(fdinfo, "drm-client-id: 3\ndrm-pdev: 0000:00:02.0\n"
                     "drm-engine-render: 100 ns\n");
  linux_gpu::LinuxGpuCollector collector{{drm, proc, false}};
  const telemetry::ProcessIdentity foreground{{77U}, 123456U};
  const auto start = core::MonotonicTimePoint{} + std::chrono::seconds{1};
  CHECK(collector.collect(start, foreground, false).foreground_usage.status ==
        telemetry::MetricStatus::temporarily_unavailable);

  write_file(fdinfo, "drm-client-id: 3\ndrm-pdev: 0000:00:02.0\n"
                     "drm-engine-render: 500000100 ns\n");
  const auto second =
      collector.collect(start + std::chrono::seconds{1}, foreground, false);
  REQUIRE(second.foreground_usage.has_value());
  CHECK(second.foreground_usage.value.value == 0.5);
}
