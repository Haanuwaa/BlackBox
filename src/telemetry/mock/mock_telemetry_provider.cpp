#include "telemetry/mock/mock_telemetry_provider.hpp"

#include <chrono>
#include <string>

namespace blackbox::telemetry::mock {
namespace {

using namespace std::chrono_literals;

constexpr std::uint64_t kibibyte = 1024U;
constexpr std::uint64_t mebibyte = 1024U * kibibyte;
constexpr std::uint64_t gibibyte = 1024U * mebibyte;
constexpr std::uint64_t spike_sequence = 4U;
constexpr ProcessIdentity worker_identity{ProcessId{4242U}, 1U};

template <typename T>
[[nodiscard]] MetricValue<T> capability_value(const bool supported, T value) {
    if (!supported) {
        return MetricValue<T>::unavailable(MetricStatus::unsupported);
    }
    return MetricValue<T>::available(std::move(value));
}

} // namespace

MockTelemetryProvider::MockTelemetryProvider(const core::IMonotonicClock& clock,
                                             const Scenario scenario)
    : clock_{clock}, scenario_{scenario} {
    capabilities_.cpu_usage = true;
    capabilities_.memory_usage = true;
    capabilities_.process_cpu = true;
    capabilities_.process_memory = true;
    capabilities_.process_disk_io = true;
    capabilities_.disk_throughput = true;
    capabilities_.network_usage = true;
}

ProviderSampleResult MockTelemetryProvider::sample(
    const SamplingRequest request,
    RawTelemetrySnapshot& destination) {
    advance_counters();
    destination.reset(clock_.now(), request.tiers);

    const auto temporary_bytes = [] {
        return MetricValue<ByteCount>::unavailable(MetricStatus::temporarily_unavailable);
    };

    destination.system.cpu_time = MetricValue<CpuTimeCounters>::unavailable(
        MetricStatus::temporarily_unavailable);
    destination.system.memory_total = temporary_bytes();
    destination.system.memory_available = temporary_bytes();
    destination.system.disk_read_bytes = temporary_bytes();
    destination.system.disk_write_bytes = temporary_bytes();
    destination.system.network_receive_bytes = temporary_bytes();
    destination.system.network_transmit_bytes = temporary_bytes();
    destination.system.logical_processor_count =
        MetricValue<std::uint32_t>::unavailable(MetricStatus::temporarily_unavailable);

    if (request.tiers.contains(SamplingTier::fast)) {
        destination.system.cpu_time = capability_value(capabilities_.cpu_usage, cpu_);
        destination.system.disk_read_bytes = capability_value(capabilities_.disk_throughput, disk_read_);
        destination.system.disk_write_bytes = capability_value(capabilities_.disk_throughput, disk_write_);
        destination.system.network_receive_bytes = capability_value(capabilities_.network_usage,
                                                                     network_receive_);
        destination.system.network_transmit_bytes = capability_value(capabilities_.network_usage,
                                                                      network_transmit_);
    }

    if (request.tiers.contains(SamplingTier::normal)) {
        destination.system.logical_processor_count =
            MetricValue<std::uint32_t>::available(8U);
        destination.system.memory_total = capability_value(capabilities_.memory_usage,
                                                            ByteCount{16U * gibibyte});
        destination.system.memory_available = capability_value(capabilities_.memory_usage,
                                                                ByteCount{10U * gibibyte});

        RawProcessCounters process{};
        process.identity = worker_identity;
        process.cpu_time = capability_value(capabilities_.process_cpu, process_cpu_time_);
        process.working_set = capability_value(capabilities_.process_memory,
                                               ByteCount{200U * mebibyte});
        process.disk_read_bytes = capability_value(capabilities_.process_disk_io,
                                                   process_disk_read_);
        process.disk_write_bytes = capability_value(capabilities_.process_disk_io,
                                                    process_disk_write_);
        destination.processes.push_back(process);
    }

    if (request.tiers.contains(SamplingTier::slow)) {
        ProcessInfo info{};
        info.identity = worker_identity;
        info.parent_pid = MetricValue<ProcessId>::available(ProcessId{1000U});
        info.name = MetricValue<std::string>::available("mock-worker");
        info.executable_path = MetricValue<std::string>::available("/mock/worker");
        destination.process_metadata.push_back(std::move(info));
    }

    return ProviderSampleResult{ProviderSampleStatus::complete, sequence_++};
}

PlatformCapabilities MockTelemetryProvider::capabilities() const noexcept {
    return capabilities_;
}

void MockTelemetryProvider::set_capabilities(const PlatformCapabilities capabilities) noexcept {
    capabilities_ = capabilities;
}

void MockTelemetryProvider::reset(const Scenario scenario) noexcept {
    scenario_ = scenario;
    sequence_ = 0U;
    cpu_ = CpuTimeCounters{};
    disk_read_ = ByteCount{};
    disk_write_ = ByteCount{};
    network_receive_ = ByteCount{};
    network_transmit_ = ByteCount{};
    process_cpu_time_ = 0ns;
    process_disk_read_ = ByteCount{};
    process_disk_write_ = ByteCount{};
}

void MockTelemetryProvider::advance_counters() noexcept {
    const bool spike = sequence_ == spike_sequence;

    cpu_.total_ticks += 1000U;
    cpu_.busy_ticks += (scenario_ == Scenario::cpu_spike && spike) ? 900U : 250U;

    disk_read_.value += (scenario_ == Scenario::disk_spike && spike) ? 64U * mebibyte : mebibyte;
    disk_write_.value += (scenario_ == Scenario::disk_spike && spike) ? 32U * mebibyte : 256U * kibibyte;

    if (!(scenario_ == Scenario::network_drop && spike)) {
        network_receive_.value += 128U * kibibyte;
        network_transmit_.value += 64U * kibibyte;
    }

    process_cpu_time_ += (scenario_ == Scenario::process_spike && spike) ? 800ms : 50ms;
    process_disk_read_.value += (scenario_ == Scenario::process_spike && spike) ? 8U * mebibyte : 64U * kibibyte;
    process_disk_write_.value += (scenario_ == Scenario::process_spike && spike) ? 4U * mebibyte : 32U * kibibyte;
}

} // namespace blackbox::telemetry::mock
