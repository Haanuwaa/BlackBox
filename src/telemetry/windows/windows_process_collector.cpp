#include "telemetry/windows/windows_process_collector.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <unordered_map>

namespace blackbox::telemetry::windows {
namespace {

struct HandleGuard {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~HandleGuard() {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }
};

[[nodiscard]] constexpr std::uint64_t file_time_ticks(const FILETIME value) noexcept {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
           static_cast<std::uint64_t>(value.dwLowDateTime);
}

[[nodiscard]] MetricStatus last_error_status() noexcept {
    const auto error = GetLastError();
    return error == ERROR_ACCESS_DENIED ? MetricStatus::inaccessible
                                       : MetricStatus::temporarily_unavailable;
}

[[nodiscard]] std::string utf8(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const auto required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                            result.data(), required, nullptr, nullptr) == 0) {
        return {};
    }
    result.pop_back();
    return result;
}

template <typename T>
[[nodiscard]] MetricValue<T> failed_metric() noexcept {
    return MetricValue<T>::unavailable(last_error_status());
}

} // namespace

struct WindowsProcessCollector::State {
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
    static constexpr DWORD maximum_path_characters = 32'768U;

    State() {
        metadata.reserve(512U);
    }

    [[nodiscard]] MetricStatus collect(
        const bool collect_counters,
        const bool resolve_paths,
        RawTelemetrySnapshot& destination) {
        HandleGuard snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0U)};
        if (snapshot.value == INVALID_HANDLE_VALUE) {
            lifecycle_warmed = false;
            return last_error_status();
        }

        const bool emit_lifecycle = lifecycle_warmed;
        ++generation;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        BOOL has_entry = Process32FirstW(snapshot.value, &entry);
        while (has_entry != FALSE) {
            ++destination.process_diagnostics.enumerated;
            collect_entry(entry, collect_counters, resolve_paths, emit_lifecycle,
                          destination);
            entry.dwSize = sizeof(entry);
            has_entry = Process32NextW(snapshot.value, &entry);
        }
        const auto final_error = GetLastError();
        if (final_error != ERROR_NO_MORE_FILES) {
            destination.process_lifecycle_events.clear();
            lifecycle_warmed = false;
            return final_error == ERROR_ACCESS_DENIED
                       ? MetricStatus::inaccessible
                       : MetricStatus::temporarily_unavailable;
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
        return MetricStatus::available;
    }

    void collect_entry(const PROCESSENTRY32W& entry,
                       const bool collect_counters,
                       const bool resolve_paths,
                       const bool emit_lifecycle,
                       RawTelemetrySnapshot& destination) {
        const auto pid = static_cast<std::uint32_t>(entry.th32ProcessID);
        HandleGuard process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                        FALSE, entry.th32ProcessID)};
        if (process.value == nullptr) {
            // The Tool Help snapshot still proves that this PID existed at
            // enumeration time. Preserve any prior durable identity so an
            // access transition cannot fabricate an exit/start pair.
            for (auto& [identity, cached] : metadata) {
                if (identity.pid.value == pid) cached.generation = generation;
            }
            ++destination.process_diagnostics.inaccessible;
            return;
        }

        FILETIME creation{};
        FILETIME exit{};
        FILETIME kernel{};
        FILETIME user{};
        if (GetProcessTimes(process.value, &creation, &exit, &kernel, &user) == 0) {
            // The process was present in this enumeration even if its durable
            // creation token became unreadable before we queried it. Preserve
            // any prior identity for this PID so the transient race cannot be
            // misreported as a lifecycle transition.
            for (auto& [identity, cached] : metadata) {
                if (identity.pid.value == pid) cached.generation = generation;
            }
            const auto error = GetLastError();
            if (error == ERROR_INVALID_PARAMETER || error == ERROR_INVALID_HANDLE) {
                ++destination.process_diagnostics.exited_during_sample;
            } else {
                ++destination.process_diagnostics.inaccessible;
            }
            return;
        }

        const ProcessIdentity identity{ProcessId{pid}, file_time_ticks(creation)};
        auto cached = metadata.find(identity);
        const bool is_new = cached == metadata.end();
        if (is_new) {
            if (metadata.size() >= maximum_processes) {
                ++destination.process_diagnostics.inaccessible;
                return;
            }
            CachedMetadata value{};
            value.info.identity = identity;
            value.info.parent_pid = MetricValue<ProcessId>::available(
                ProcessId{static_cast<std::uint32_t>(entry.th32ParentProcessID)});
            auto name = utf8(entry.szExeFile);
            value.info.name = name.empty()
                                  ? MetricValue<std::string>::unavailable(
                                        MetricStatus::temporarily_unavailable)
                                  : MetricValue<std::string>::available(std::move(name));
            value.info.executable_path = MetricValue<std::string>::unavailable(
                MetricStatus::temporarily_unavailable);
            cached = metadata.emplace(identity, std::move(value)).first;
            if (emit_lifecycle) {
                destination.process_lifecycle_events.push_back(
                    RawProcessLifecycleEvent{identity,
                                             RawProcessLifecycleKind::started});
            }
        }
        cached->second.generation = generation;

        if (resolve_paths && !cached->second.path_terminal) {
            DWORD size = maximum_path_characters;
            if (QueryFullProcessImageNameW(process.value, 0U, path_buffer.data(), &size) != 0) {
                path_buffer[size] = L'\0';
                auto path = utf8(path_buffer.data());
                cached->second.info.executable_path = path.empty()
                    ? MetricValue<std::string>::unavailable(
                          MetricStatus::temporarily_unavailable)
                    : MetricValue<std::string>::available(std::move(path));
                cached->second.path_terminal =
                    cached->second.info.executable_path.has_value();
                ++destination.process_diagnostics.metadata_resolved;
            } else {
                const auto status = last_error_status();
                cached->second.info.executable_path =
                    MetricValue<std::string>::unavailable(status);
                cached->second.path_terminal = status == MetricStatus::inaccessible;
                ++destination.process_diagnostics.metadata_failures;
            }
        }

        if (is_new || resolve_paths) {
            destination.process_metadata.push_back(cached->second.info);
        }

        if (collect_counters) {
            RawProcessCounters counters{};
            counters.identity = identity;
            const auto kernel_ticks = file_time_ticks(kernel);
            const auto user_ticks = file_time_ticks(user);
            if (kernel_ticks <= std::numeric_limits<std::uint64_t>::max() - user_ticks &&
                kernel_ticks + user_ticks <=
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max() / 100LL)) {
                counters.cpu_time = MetricValue<std::chrono::nanoseconds>::available(
                    std::chrono::nanoseconds{
                        static_cast<std::int64_t>((kernel_ticks + user_ticks) * 100U)});
            } else {
                counters.cpu_time = MetricValue<std::chrono::nanoseconds>::unavailable(
                    MetricStatus::temporarily_unavailable);
            }

            PROCESS_MEMORY_COUNTERS memory{};
            memory.cb = sizeof(memory);
            if (GetProcessMemoryInfo(process.value, &memory, sizeof(memory)) != 0) {
                counters.working_set = MetricValue<ByteCount>::available(
                    ByteCount{static_cast<std::uint64_t>(memory.WorkingSetSize)});
            } else {
                counters.working_set = failed_metric<ByteCount>();
            }

            IO_COUNTERS io{};
            if (GetProcessIoCounters(process.value, &io) != 0) {
                counters.disk_read_bytes = MetricValue<ByteCount>::available(
                    ByteCount{io.ReadTransferCount});
                counters.disk_write_bytes = MetricValue<ByteCount>::available(
                    ByteCount{io.WriteTransferCount});
            } else {
                counters.disk_read_bytes = failed_metric<ByteCount>();
                counters.disk_write_bytes = failed_metric<ByteCount>();
            }
            destination.processes.push_back(std::move(counters));
            ++destination.process_diagnostics.sampled;
        }
    }

    std::unordered_map<ProcessIdentity, CachedMetadata, IdentityHash> metadata{};
    std::array<wchar_t, maximum_path_characters + 1U> path_buffer{};
    std::uint64_t generation{};
    bool lifecycle_warmed{};
};

WindowsProcessCollector::WindowsProcessCollector() noexcept
    : state_{new (std::nothrow) State{}} {}

WindowsProcessCollector::~WindowsProcessCollector() = default;

MetricStatus WindowsProcessCollector::collect(
    const bool collect_counters,
    const bool resolve_paths,
    RawTelemetrySnapshot& destination) {
    return state_ != nullptr
               ? state_->collect(collect_counters, resolve_paths, destination)
               : MetricStatus::temporarily_unavailable;
}

std::size_t WindowsProcessCollector::cache_size() const noexcept {
    return state_ != nullptr ? state_->metadata.size() : 0U;
}

} // namespace blackbox::telemetry::windows
