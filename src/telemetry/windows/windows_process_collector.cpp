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
    HandleGuard() = default;
    explicit HandleGuard(const HANDLE handle) noexcept : value{handle} {}
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    HandleGuard(HandleGuard&& other) noexcept : value{other.value} {
        other.value = INVALID_HANDLE_VALUE;
    }
    HandleGuard& operator=(HandleGuard&& other) noexcept {
        if (this == &other) return *this;
        reset();
        value = other.value;
        other.value = INVALID_HANDLE_VALUE;
        return *this;
    }
    ~HandleGuard() {
        reset();
    }
    [[nodiscard]] bool valid() const noexcept {
        return value != nullptr && value != INVALID_HANDLE_VALUE;
    }
    void reset(const HANDLE replacement = INVALID_HANDLE_VALUE) noexcept {
        if (valid()) {
            CloseHandle(value);
        }
        value = replacement;
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
        HandleGuard process{};
        std::uint64_t generation{};
        bool path_terminal{};
    };

    static constexpr std::size_t maximum_processes = 8'192U;
    static constexpr DWORD maximum_path_characters = 32'768U;

    State() {
        metadata.reserve(512U);
        active_by_pid.reserve(512U);
    }

    [[nodiscard]] MetricStatus collect(
        const bool collect_counters,
        const bool resolve_paths,
        RawTelemetrySnapshot& destination) {
        const bool emit_lifecycle = lifecycle_warmed;
        ++generation;
        last_diagnostics = {};
        const auto enumeration_status = resolve_paths
            ? collect_metadata_enumeration(collect_counters, emit_lifecycle,
                                           destination)
            : collect_fast_enumeration(collect_counters, emit_lifecycle,
                                       destination);
        if (enumeration_status != MetricStatus::available) {
            destination.process_lifecycle_events.clear();
            lifecycle_warmed = false;
            return enumeration_status;
        }

        for (auto iterator = metadata.begin(); iterator != metadata.end();) {
            if (iterator->second.generation != generation) {
                if (emit_lifecycle) {
                    destination.process_lifecycle_events.push_back(
                        RawProcessLifecycleEvent{iterator->first,
                                                 RawProcessLifecycleKind::exited});
                }
                const auto active = active_by_pid.find(iterator->first.pid.value);
                if (active != active_by_pid.end() && active->second == iterator->first) {
                    active_by_pid.erase(active);
                }
                if (iterator->second.process.valid()) {
                    --cached_handles;
                }
                iterator = metadata.erase(iterator);
            } else {
                ++iterator;
            }
        }
        lifecycle_warmed = true;
        return MetricStatus::available;
    }

    [[nodiscard]] MetricStatus collect_metadata_enumeration(
        const bool collect_counters,
        const bool emit_lifecycle,
        RawTelemetrySnapshot& destination) {
        HandleGuard snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0U)};
        if (snapshot.value == INVALID_HANDLE_VALUE) return last_error_status();
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        BOOL has_entry = Process32FirstW(snapshot.value, &entry);
        while (has_entry != FALSE) {
            ++destination.process_diagnostics.enumerated;
            collect_entry(entry, collect_counters, true, emit_lifecycle,
                          destination);
            entry.dwSize = sizeof(entry);
            has_entry = Process32NextW(snapshot.value, &entry);
        }
        const auto final_error = GetLastError();
        return final_error == ERROR_NO_MORE_FILES
                   ? MetricStatus::available
                   : final_error == ERROR_ACCESS_DENIED
                         ? MetricStatus::inaccessible
                         : MetricStatus::temporarily_unavailable;
    }

    [[nodiscard]] MetricStatus collect_fast_enumeration(
        const bool collect_counters,
        const bool emit_lifecycle,
        RawTelemetrySnapshot& destination) {
        DWORD bytes{};
        if (EnumProcesses(process_ids.data(), static_cast<DWORD>(sizeof(process_ids)),
                          &bytes) == 0) {
            return last_error_status();
        }
        if (bytes >= sizeof(process_ids) || bytes % sizeof(process_ids[0]) != 0U) {
            return MetricStatus::temporarily_unavailable;
        }
        const auto count = static_cast<std::size_t>(bytes / sizeof(process_ids[0]));
        for (std::size_t index = 0U; index < count; ++index) {
            if (process_ids[index] == 0U) continue;
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            entry.th32ProcessID = process_ids[index];
            ++destination.process_diagnostics.enumerated;
            collect_entry(entry, collect_counters, false, emit_lifecycle,
                          destination);
        }
        return MetricStatus::available;
    }

    void collect_entry(const PROCESSENTRY32W& entry,
                       const bool collect_counters,
                       const bool resolve_paths,
                       const bool emit_lifecycle,
                       RawTelemetrySnapshot& destination) {
        const auto pid = static_cast<std::uint32_t>(entry.th32ProcessID);
        auto active = active_by_pid.find(pid);
        auto cached = metadata.end();
        if (active != active_by_pid.end()) {
            cached = metadata.find(active->second);
            if (cached == metadata.end()) {
                active_by_pid.erase(active);
            } else if (cached->second.process.valid()) {
                const auto wait = WaitForSingleObject(cached->second.process.value, 0U);
                if (wait == WAIT_TIMEOUT) {
                    ++last_diagnostics.handles_reused;
                } else if (wait == WAIT_OBJECT_0) {
                    if (emit_lifecycle) {
                        destination.process_lifecycle_events.push_back(
                            RawProcessLifecycleEvent{cached->first,
                                                     RawProcessLifecycleKind::exited});
                    }
                    active_by_pid.erase(active);
                    --cached_handles;
                    metadata.erase(cached);
                    cached = metadata.end();
                } else {
                    cached->second.process.reset();
                    --cached_handles;
                }
            }
        }

        HandleGuard opened{};
        const bool reused_cached_handle =
            cached != metadata.end() && cached->second.process.valid();
        HANDLE process = reused_cached_handle
                             ? cached->second.process.value
                             : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
                                               SYNCHRONIZE,
                                           FALSE, entry.th32ProcessID);
        if (!reused_cached_handle && process != nullptr) {
            opened.reset(process);
            ++last_diagnostics.handles_opened;
        }
        if (process == nullptr || process == INVALID_HANDLE_VALUE) {
            // The Tool Help snapshot still proves that this PID existed at
            // enumeration time. Preserve any prior durable identity so an
            // access transition cannot fabricate an exit/start pair.
            for (auto& [identity, metadata_entry] : metadata) {
                if (identity.pid.value == pid) metadata_entry.generation = generation;
            }
            ++last_diagnostics.handle_open_failures;
            ++destination.process_diagnostics.inaccessible;
            return;
        }

        FILETIME creation{};
        FILETIME exit{};
        FILETIME kernel{};
        FILETIME user{};
        const bool need_times = !reused_cached_handle || collect_counters;
        if (need_times &&
            GetProcessTimes(process, &creation, &exit, &kernel, &user) == 0) {
            // The process was present in this enumeration even if its durable
            // creation token became unreadable before we queried it. Preserve
            // any prior identity for this PID so the transient race cannot be
            // misreported as a lifecycle transition.
            for (auto& [identity, metadata_entry] : metadata) {
                if (identity.pid.value == pid) metadata_entry.generation = generation;
            }
            const auto error = GetLastError();
            if (error == ERROR_INVALID_PARAMETER || error == ERROR_INVALID_HANDLE) {
                ++destination.process_diagnostics.exited_during_sample;
            } else {
                ++destination.process_diagnostics.inaccessible;
            }
            return;
        }

        const ProcessIdentity identity = reused_cached_handle
            ? cached->first
            : ProcessIdentity{ProcessId{pid}, file_time_ticks(creation)};
        if (!reused_cached_handle) cached = metadata.find(identity);
        const bool is_new = cached == metadata.end();
        const bool retain_opened = opened.valid() &&
                                   cached_handles < maximum_cached_process_handles;
        if (is_new) {
            if (metadata.size() >= maximum_processes) {
                ++destination.process_diagnostics.inaccessible;
                return;
            }
            CachedMetadata value{};
            value.info.identity = identity;
            if (retain_opened) {
                value.process = std::move(opened);
                ++cached_handles;
            }
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
            active_by_pid[pid] = identity;
            if (emit_lifecycle) {
                destination.process_lifecycle_events.push_back(
                    RawProcessLifecycleEvent{identity,
                                             RawProcessLifecycleKind::started});
            }
        } else if (retain_opened) {
            cached->second.process = std::move(opened);
            ++cached_handles;
            active_by_pid[pid] = identity;
        }
        cached->second.generation = generation;

        if (resolve_paths) {
            cached->second.info.parent_pid = MetricValue<ProcessId>::available(
                ProcessId{static_cast<std::uint32_t>(entry.th32ParentProcessID)});
            auto name = utf8(entry.szExeFile);
            if (!name.empty()) {
                cached->second.info.name =
                    MetricValue<std::string>::available(std::move(name));
            }
        }

        if (resolve_paths && !cached->second.path_terminal) {
            DWORD size = maximum_path_characters;
            if (QueryFullProcessImageNameW(process, 0U, path_buffer.data(), &size) != 0) {
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
            if (GetProcessMemoryInfo(process, &memory, sizeof(memory)) != 0) {
                counters.working_set = MetricValue<ByteCount>::available(
                    ByteCount{static_cast<std::uint64_t>(memory.WorkingSetSize)});
            } else {
                counters.working_set = failed_metric<ByteCount>();
            }

            IO_COUNTERS io{};
            if (GetProcessIoCounters(process, &io) != 0) {
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
    std::unordered_map<std::uint32_t, ProcessIdentity> active_by_pid{};
    std::array<DWORD, maximum_processes> process_ids{};
    std::array<wchar_t, maximum_path_characters + 1U> path_buffer{};
    std::uint64_t generation{};
    std::size_t cached_handles{};
    bool lifecycle_warmed{};
    WindowsProcessCollectorDiagnostics last_diagnostics{};
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

std::size_t WindowsProcessCollector::cached_handle_count() const noexcept {
    return state_ != nullptr ? state_->cached_handles : 0U;
}

WindowsProcessCollectorDiagnostics
WindowsProcessCollector::diagnostics() const noexcept {
    return state_ != nullptr ? state_->last_diagnostics
                             : WindowsProcessCollectorDiagnostics{};
}

} // namespace blackbox::telemetry::windows
