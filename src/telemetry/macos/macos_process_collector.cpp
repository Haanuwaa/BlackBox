#include "telemetry/macos/macos_process_collector.hpp"

#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/resource.h>

#include <array>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>

namespace blackbox::telemetry::macos {
namespace {

[[nodiscard]] constexpr MetricStatus errno_status(const int error) noexcept {
    return error == EACCES || error == EPERM ? MetricStatus::inaccessible
                                             : MetricStatus::temporarily_unavailable;
}

[[nodiscard]] std::string bounded_string(const char* value,
                                         const std::size_t capacity) {
    const auto length = strnlen(value, capacity);
    return std::string{value, length};
}

[[nodiscard]] bool creation_token(const proc_bsdinfo& info,
                                  std::uint64_t& destination) noexcept {
    constexpr std::uint64_t microseconds_per_second = 1'000'000U;
    const auto seconds = static_cast<std::uint64_t>(info.pbi_start_tvsec);
    const auto microseconds = static_cast<std::uint64_t>(info.pbi_start_tvusec);
    if (seconds > std::numeric_limits<std::uint64_t>::max() /
                      microseconds_per_second ||
        microseconds >= microseconds_per_second) {
        return false;
    }
    destination = seconds * microseconds_per_second + microseconds;
    return destination != 0U;
}

template <typename T>
[[nodiscard]] MetricValue<T> unavailable(const int error) noexcept {
    return MetricValue<T>::unavailable(errno_status(error));
}

void increment(std::uint32_t& value) noexcept {
    if (value != std::numeric_limits<std::uint32_t>::max()) ++value;
}

} // namespace

struct MacosProcessCollector::State {
    struct IdentityHash {
        [[nodiscard]] std::size_t operator()(const ProcessIdentity& identity) const noexcept {
            return static_cast<std::size_t>(
                identity.creation_token ^
                (static_cast<std::uint64_t>(identity.pid.value) *
                 0x9e3779b97f4a7c15ULL));
        }
    };

    struct CachedMetadata {
        ProcessInfo info{};
        std::uint64_t generation{};
        bool path_terminal{};
    };

    static constexpr std::size_t maximum_processes = 8'192U;

    State() {
        metadata.reserve(512U);
    }

    [[nodiscard]] MetricStatus collect(const bool collect_counters,
                                       const bool resolve_paths,
                                       RawTelemetrySnapshot& destination) {
        errno = 0;
        const auto listed = proc_listallpids(
            process_ids.data(), static_cast<int>(sizeof(process_ids)));
        if (listed <= 0) {
            lifecycle_warmed = false;
            return errno_status(errno);
        }

        const auto count = std::min(
            process_ids.size(), static_cast<std::size_t>(listed));
        const bool emit_lifecycle = lifecycle_warmed;
        ++generation;
        for (std::size_t index = 0U; index < count; ++index) {
            if (process_ids[index] <= 0) continue;
            increment(destination.process_diagnostics.enumerated);
            collect_process(process_ids[index], collect_counters, resolve_paths,
                            emit_lifecycle, destination);
        }

        for (auto iterator = metadata.begin(); iterator != metadata.end();) {
            if (iterator->second.generation != generation) {
                if (emit_lifecycle) {
                    destination.process_lifecycle_events.push_back(
                        RawProcessLifecycleEvent{iterator->first,
                                                 RawProcessLifecycleKind::exited});
                }
                iterator = metadata.erase(iterator);
            } else {
                ++iterator;
            }
        }
        lifecycle_warmed = true;
        return metadata.empty() ? MetricStatus::temporarily_unavailable
                                : MetricStatus::available;
    }

    void collect_process(const pid_t native_pid,
                         const bool collect_counters,
                         const bool resolve_paths,
                         const bool emit_lifecycle,
                         RawTelemetrySnapshot& destination) {
        proc_bsdinfo bsd{};
        errno = 0;
        const auto bsd_bytes = proc_pidinfo(
            native_pid, PROC_PIDTBSDINFO, 0U, &bsd, sizeof(bsd));
        if (bsd_bytes != static_cast<int>(sizeof(bsd))) {
            const auto error = errno;
            if (error == ESRCH) increment(destination.process_diagnostics.exited_during_sample);
            else increment(destination.process_diagnostics.inaccessible);
            return;
        }

        std::uint64_t token{};
        if (!creation_token(bsd, token)) {
            increment(destination.process_diagnostics.inaccessible);
            return;
        }
        const auto pid = static_cast<std::uint32_t>(native_pid);
        const ProcessIdentity identity{ProcessId{pid}, token};
        auto cached = metadata.find(identity);
        const bool is_new = cached == metadata.end();
        if (is_new) {
            if (metadata.size() >= maximum_processes) {
                increment(destination.process_diagnostics.inaccessible);
                return;
            }
            CachedMetadata value{};
            value.info.identity = identity;
            value.info.parent_pid = MetricValue<ProcessId>::available(
                ProcessId{static_cast<std::uint32_t>(bsd.pbi_ppid)});
            auto name = bounded_string(bsd.pbi_name, sizeof(bsd.pbi_name));
            if (name.empty()) name = bounded_string(bsd.pbi_comm, sizeof(bsd.pbi_comm));
            value.info.name = name.empty()
                                  ? MetricValue<std::string>::unavailable(
                                        MetricStatus::temporarily_unavailable)
                                  : MetricValue<std::string>::available(std::move(name));
            value.info.executable_path = MetricValue<std::string>::unavailable(
                MetricStatus::temporarily_unavailable);
            cached = metadata.emplace(identity, std::move(value)).first;
            if (emit_lifecycle) {
                destination.process_lifecycle_events.push_back(
                    RawProcessLifecycleEvent{identity, RawProcessLifecycleKind::started});
            }
        }
        cached->second.generation = generation;

        if (resolve_paths && !cached->second.path_terminal) {
            path_buffer.fill('\0');
            errno = 0;
            const auto path_bytes = proc_pidpath(
                native_pid, path_buffer.data(), static_cast<std::uint32_t>(path_buffer.size()));
            if (path_bytes > 0) {
                cached->second.info.executable_path =
                    MetricValue<std::string>::available(
                        bounded_string(path_buffer.data(), path_buffer.size()));
                cached->second.path_terminal =
                    cached->second.info.executable_path.has_value() &&
                    !cached->second.info.executable_path.value.empty();
                increment(destination.process_diagnostics.metadata_resolved);
            } else {
                const auto status = errno_status(errno);
                cached->second.info.executable_path =
                    MetricValue<std::string>::unavailable(status);
                cached->second.path_terminal = status == MetricStatus::inaccessible;
                increment(destination.process_diagnostics.metadata_failures);
            }
        }

        if (is_new || resolve_paths) {
            destination.process_metadata.push_back(cached->second.info);
        }
        if (!collect_counters) return;

        rusage_info_v2 usage{};
        errno = 0;
        const auto usage_result = proc_pid_rusage(
            native_pid, RUSAGE_INFO_V2,
            reinterpret_cast<rusage_info_t*>(&usage));
        RawProcessCounters counters{};
        counters.identity = identity;
        if (usage_result == 0) {
            if (usage.ri_user_time <= std::numeric_limits<std::uint64_t>::max() -
                                          usage.ri_system_time &&
                usage.ri_user_time + usage.ri_system_time <=
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                counters.cpu_time = MetricValue<std::chrono::nanoseconds>::available(
                    std::chrono::nanoseconds{static_cast<std::int64_t>(
                        usage.ri_user_time + usage.ri_system_time)});
            } else {
                counters.cpu_time = MetricValue<std::chrono::nanoseconds>::unavailable(
                    MetricStatus::temporarily_unavailable);
            }
            counters.working_set = MetricValue<ByteCount>::available(
                ByteCount{usage.ri_phys_footprint});
            counters.disk_read_bytes = MetricValue<ByteCount>::available(
                ByteCount{usage.ri_diskio_bytesread});
            counters.disk_write_bytes = MetricValue<ByteCount>::available(
                ByteCount{usage.ri_diskio_byteswritten});
        } else {
            const auto error = errno;
            counters.cpu_time = unavailable<std::chrono::nanoseconds>(error);
            counters.working_set = unavailable<ByteCount>(error);
            counters.disk_read_bytes = unavailable<ByteCount>(error);
            counters.disk_write_bytes = unavailable<ByteCount>(error);
        }
        destination.processes.push_back(std::move(counters));
        increment(destination.process_diagnostics.sampled);
    }

    std::array<pid_t, maximum_processes> process_ids{};
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> path_buffer{};
    std::unordered_map<ProcessIdentity, CachedMetadata, IdentityHash> metadata{};
    std::uint64_t generation{};
    bool lifecycle_warmed{};
};

MacosProcessCollector::MacosProcessCollector() noexcept
    : state_{new (std::nothrow) State{}} {}

MacosProcessCollector::~MacosProcessCollector() = default;

MetricStatus MacosProcessCollector::collect(const bool collect_counters,
                                            const bool resolve_paths,
                                            RawTelemetrySnapshot& destination) {
    return state_ != nullptr
               ? state_->collect(collect_counters, resolve_paths, destination)
               : MetricStatus::temporarily_unavailable;
}

std::size_t MacosProcessCollector::cache_size() const noexcept {
    return state_ != nullptr ? state_->metadata.size() : 0U;
}

} // namespace blackbox::telemetry::macos
