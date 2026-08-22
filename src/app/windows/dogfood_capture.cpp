#include "core/clock.hpp"
#include "storage/incident_archive.hpp"
#include "telemetry/automatic_incident_detector.hpp"
#include "telemetry/collector.hpp"
#include "telemetry/event_collector.hpp"
#include "telemetry/windows/windows_system_event_provider.hpp"
#include "telemetry/windows/windows_telemetry_provider.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace core = blackbox::core;
namespace storage = blackbox::storage;
namespace telemetry = blackbox::telemetry;
namespace windows_telemetry = blackbox::telemetry::windows;
using namespace std::chrono_literals;

namespace {

template <typename Integer>
[[nodiscard]] std::optional<Integer> parse_integer(const std::wstring_view text) noexcept {
    static_assert(std::is_unsigned_v<Integer>);
    if (text.empty()) return std::nullopt;
    Integer value{};
    for (const auto character : text) {
        if (character < L'0' || character > L'9') return std::nullopt;
        const auto digit = static_cast<Integer>(character - L'0');
        if (value > (std::numeric_limits<Integer>::max() - digit) / 10U)
            return std::nullopt;
        value = value * 10U + digit;
    }
    return value;
}

[[nodiscard]] std::wstring quote_argument(const std::wstring_view argument) {
    if (argument.find_first_of(L" \t\"") == std::wstring_view::npos) {
        return std::wstring{argument};
    }
    std::wstring result{L"\""};
    std::size_t backslashes{};
    for (const auto character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            result.append(backslashes * 2U + 1U, L'\\');
            result.push_back(L'\"');
            backslashes = 0U;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0U;
        result.push_back(character);
    }
    result.append(backslashes * 2U, L'\\');
    result.push_back(L'\"');
    return result;
}

class ProcessHandle final {
public:
    ProcessHandle() = default;
    ~ProcessHandle() {
        if (thread_ != nullptr) CloseHandle(thread_);
        if (process_ != nullptr) CloseHandle(process_);
    }
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
    ProcessHandle(ProcessHandle&& other) noexcept
        : process_(std::exchange(other.process_, nullptr)),
          thread_(std::exchange(other.thread_, nullptr)) {}
    ProcessHandle& operator=(ProcessHandle&&) = delete;

    [[nodiscard]] static std::optional<ProcessHandle> launch(
        const int argc, wchar_t** argv, const int first) {
        std::wstring command_line;
        for (int index = first; index < argc; ++index) {
            if (!command_line.empty()) command_line.push_back(L' ');
            command_line += quote_argument(argv[index]);
        }
        command_line.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0U,
                           nullptr, nullptr, &startup, &process) == 0) {
            return std::nullopt;
        }
        ProcessHandle result;
        result.process_ = process.hProcess;
        result.thread_ = process.hThread;
        return result;
    }

private:
    HANDLE process_{};
    HANDLE thread_{};
};

[[nodiscard]] std::string export_key_text(const storage::IncidentExportKey& key) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(key.bytes.size() * 2U, '0');
    for (std::size_t index = 0U; index < key.bytes.size(); ++index) {
        result[index * 2U] = digits[key.bytes[index] >> 4U];
        result[index * 2U + 1U] = digits[key.bytes[index] & 0x0FU];
    }
    return result;
}

[[nodiscard]] std::string utf8(const std::wstring_view text) {
    if (text.empty()) return {};
    const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), result.data(), required, nullptr, nullptr) <= 0) {
        return {};
    }
    return result;
}

[[nodiscard]] bool ascii_case_equal(const std::string_view left,
                                    const std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        const auto lower = [](const char value) noexcept {
            return value >= 'A' && value <= 'Z'
                ? static_cast<char>(value - 'A' + 'a') : value;
        };
        if (lower(left[index]) != lower(right[index])) return false;
    }
    return true;
}

[[nodiscard]] std::optional<std::size_t> workload_ordinal(
    const core::IncidentSnapshot& incident, const std::string_view workload_name) {
    if (workload_name.empty()) return std::nullopt;
    std::map<core::IncidentProcessIdentity, std::size_t> ordinals;
    for (const auto& sample : incident.process_samples()) {
        ordinals.emplace(sample.identity, ordinals.size());
    }
    for (const auto& process : incident.process_metadata()) {
        if (process.name.status != core::RecordedValueStatus::available ||
            !ascii_case_equal(process.name.value, workload_name)) continue;
        if (const auto found = ordinals.find(process.identity); found != ordinals.end()) {
            return found->second;
        }
    }
    return std::nullopt;
}

void wait_for(const std::chrono::seconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for((std::min)(250ms,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())));
    }
}

void usage() {
    std::wcerr <<
        L"Usage: blackbox_dogfood_capture <archive.sqlite3> <baseline-seconds> "
        L"<lead-seconds> <post-seconds> [workload-executable [arguments...]]\n"
        L"Baseline must be 60-300 seconds, lead 0-30 seconds, and post 5-60 seconds.\n";
}

} // namespace

int wmain(const int argc, wchar_t** argv) {
    if (argc < 5) {
        usage();
        return 2;
    }
    const auto baseline = parse_integer<unsigned>(argv[2]);
    const auto lead = parse_integer<unsigned>(argv[3]);
    const auto post = parse_integer<unsigned>(argv[4]);
    if (!baseline || *baseline < 60U || *baseline > 300U || !lead || *lead > 30U ||
        *lead > *baseline || !post || *post < 5U || *post > 60U) {
        usage();
        return 2;
    }
    const std::filesystem::path archive_path{argv[1]};
    if (const auto parent = archive_path.parent_path(); !parent.empty()) {
        std::error_code issue;
        std::filesystem::create_directories(parent, issue);
        if (issue) {
            std::wcerr << L"Cannot create archive directory.\n";
            return 1;
        }
    }
    storage::SqliteIncidentArchive archive{
        storage::ArchiveConfiguration{.path = archive_path}};
    if (auto opened = archive.open(); !opened) {
        std::cerr << "Archive open failed: " << opened.error().message << '\n';
        return 1;
    }

    try {
        core::SystemMonotonicClock clock;
        windows_telemetry::WindowsTelemetryProvider provider{clock};
        windows_telemetry::WindowsSystemEventProvider event_provider;
        auto event_collector = std::make_unique<telemetry::SystemEventCollector>(
            event_provider, clock, telemetry::EventCollectorConfiguration{});
        telemetry::AutomaticIncidentDetector detector;
        const auto recorder = telemetry::validate_recorder_configuration(
            telemetry::RecorderConfiguration{
                .sample_interval = 1s,
                .history_duration = std::chrono::seconds{*baseline + *post + 30U},
                .late_tolerance = 50ms,
                .metadata_interval = 30s,
                .incident_pre_window = std::chrono::seconds{*baseline},
                .incident_post_window = std::chrono::seconds{*post},
                .resume_gap_threshold = 5s,
                .collect_process_paths = true});
        if (!recorder) {
            std::cerr << "Recorder configuration rejected.\n";
            return 1;
        }
        auto collector = std::make_unique<telemetry::TelemetryCollector>(
            provider, clock, *recorder, &detector, event_collector.get(),
            event_collector.get());
        collector->set_process_lifecycle_enabled(true);
        event_collector->set_incident_capture_sink(collector.get());
        collector->start();
        event_collector->start();
        std::cout << "Collecting a real provider baseline for " << *baseline
                  << " seconds.\n";
        wait_for(std::chrono::seconds{*baseline - *lead});
        auto workload = argc > 5 ? ProcessHandle::launch(argc, argv, 5)
                                 : std::optional<ProcessHandle>{};
        if (argc > 5) {
            if (!workload) {
                event_collector->stop();
                collector->stop();
                std::wcerr << L"Cannot launch the requested workload executable.\n";
                return 1;
            }
            std::cout << "Workload launched with " << *lead
                      << " seconds of pre-marker lead.\n";
        }
        wait_for(std::chrono::seconds{*lead});
        const auto before = collector->incident_capture_status().incidents_completed;
        const auto requested = collector->request_incident_capture();
        if (requested == core::IncidentCaptureRequestResult::queue_full ||
            requested == core::IncidentCaptureRequestResult::stopped) {
            event_collector->stop();
            collector->stop();
            std::cerr << "Manual marker was rejected by the bounded capture queue.\n";
            return 1;
        }
        std::cout << "Incident marker recorded; collecting " << *post
                  << " seconds after the marker.\n";
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds{*post + 15U};
        while (std::chrono::steady_clock::now() < deadline &&
               collector->incident_capture_status().incidents_completed == before) {
            std::this_thread::sleep_for(100ms);
        }
        event_collector->stop();
        collector->stop();
        std::vector<std::shared_ptr<const core::IncidentSnapshot>> incidents;
        while (auto incident = collector->try_dequeue_incident()) {
            incidents.push_back(std::move(incident));
        }
        if (incidents.empty()) {
            std::cerr << "No immutable incident completed before the deadline.\n";
            return 1;
        }
        const auto workload_name = argc > 5
            ? utf8(std::filesystem::path{argv[5]}.filename().wstring())
            : std::string{};
        for (const auto& incident : incidents) {
            auto stored = archive.store(*incident);
            if (!stored) {
                std::cerr << "Incident store failed: " << stored.error().message << '\n';
                return 1;
            }
            auto summaries = archive.list(1'000U);
            if (!summaries) {
                std::cerr << "Cannot resolve the stored incident key.\n";
                return 1;
            }
            const auto found = std::find_if(summaries->begin(), summaries->end(),
                                            [&](const auto& summary) {
                                                return summary.id == *stored;
                                            });
            if (found == summaries->end()) {
                std::cerr << "Stored incident was not discoverable.\n";
                return 1;
            }
            std::cout << "incident_id=" << *stored
                      << " incident_key=" << export_key_text(found->export_key)
                      << " manual_triggers="
                      << incident->header().window.manual_trigger_count
                      << " automatic_triggers="
                      << incident->header().window.automatic_trigger_count
                      << " system_samples=" << incident->system_samples().size()
                      << " process_samples=" << incident->process_samples().size()
                      << " system_events=" << incident->system_events().size();
            if (argc > 5) {
                const auto ordinal = workload_ordinal(*incident, workload_name);
                std::cout << " workload_ordinal="
                          << (ordinal ? std::to_string(*ordinal) : "unavailable");
            }
            std::cout << '\n';
        }
        const auto diagnostics = collector->diagnostics();
        const auto event_diagnostics = event_collector->diagnostics();
        std::cout << "collector_failures=" << diagnostics.failed_samples
                  << " collector_drops=" << diagnostics.dropped_samples
                  << " event_drops=" << event_diagnostics.native_events_dropped
                  << " incidents_stored=" << incidents.size() << '\n';
        return diagnostics.failed_samples == 0U && diagnostics.dropped_samples == 0U &&
                       event_diagnostics.native_events_dropped == 0U
                   ? 0
                   : 1;
    } catch (const std::exception& exception) {
        std::cerr << "Dogfood capture failed: " << exception.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dogfood capture failed with an unknown error.\n";
        return 1;
    }
}
