#include "ui/incident_viewer.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string_view>
#include <unordered_map>

namespace blackbox::ui {
namespace {

constexpr double bytes_per_mebibyte = 1024.0 * 1024.0;

[[nodiscard]] double seconds_from(const core::MonotonicTimePoint value,
                                  const core::MonotonicTimePoint event) noexcept {
    return std::chrono::duration<double>{value - event}.count();
}

void count_status(MetricAvailabilityCounts& counts,
                  const core::RecordedValueStatus status) noexcept {
    const auto index = static_cast<std::size_t>(status);
    if (index < counts.by_status.size()) ++counts.by_status[index];
}

void downsample(IncidentPlotSeries& series, const std::size_t maximum_points) {
    if (maximum_points == 0U) {
        series.seconds_from_event.clear();
        series.values.clear();
        return;
    }
    if (series.values.size() <= maximum_points) return;
    if (maximum_points == 1U) {
        const auto maximum = std::max_element(series.values.begin(), series.values.end());
        const auto index = static_cast<std::size_t>(maximum - series.values.begin());
        series.seconds_from_event = {series.seconds_from_event[index]};
        series.values = {*maximum};
        return;
    }

    std::vector<double> x;
    std::vector<double> y;
    x.reserve(maximum_points);
    y.reserve(maximum_points);
    x.push_back(series.seconds_from_event.front());
    y.push_back(series.values.front());
    const auto interior = series.values.size() - 2U;
    const auto bucket_count = std::max<std::size_t>(1U, (maximum_points - 2U) / 2U);
    for (std::size_t bucket = 0U; bucket < bucket_count; ++bucket) {
        const auto begin = 1U + (interior * bucket) / bucket_count;
        const auto end = 1U + (interior * (bucket + 1U)) / bucket_count;
        if (begin >= end) continue;
        auto minimum_index = begin;
        auto maximum_index = begin;
        for (auto index = begin + 1U; index < end; ++index) {
            if (series.values[index] < series.values[minimum_index]) minimum_index = index;
            if (series.values[index] > series.values[maximum_index]) maximum_index = index;
        }
        const auto append = [&](const std::size_t index) {
            if (x.size() + 1U < maximum_points &&
                (x.empty() || x.back() != series.seconds_from_event[index])) {
                x.push_back(series.seconds_from_event[index]);
                y.push_back(series.values[index]);
            }
        };
        if (minimum_index < maximum_index) {
            append(minimum_index);
            append(maximum_index);
        } else {
            append(maximum_index);
            append(minimum_index);
        }
    }
    if (x.size() < maximum_points) {
        x.push_back(series.seconds_from_event.back());
        y.push_back(series.values.back());
    }
    series.seconds_from_event = std::move(x);
    series.values = std::move(y);
}

template <typename Sample, typename Value, typename Convert>
void append_metric(IncidentPlotSeries& series, const Sample& sample,
                   const core::RecordedValue<Value>& value,
                   const core::MonotonicTimePoint event, Convert convert) {
    count_status(series.availability, value.status);
    if (value.status == core::RecordedValueStatus::available) {
        series.seconds_from_event.push_back(seconds_from(sample.observed_at, event));
        series.values.push_back(convert(value.value));
    }
}

[[nodiscard]] std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

struct IdentityHash {
    [[nodiscard]] std::size_t operator()(
        const core::IncidentProcessIdentity identity) const noexcept {
        return std::hash<std::uint64_t>{}(
            (static_cast<std::uint64_t>(identity.pid) << 32U) ^ identity.creation_token);
    }
};

[[nodiscard]] const char* event_source_text(const core::SystemEventSource source) noexcept {
    switch (source) {
    case core::SystemEventSource::power: return "Power";
    case core::SystemEventSource::device: return "Device";
    case core::SystemEventSource::audio: return "Audio";
    case core::SystemEventSource::service_control_manager: return "Service Control Manager";
    case core::SystemEventSource::defender: return "Microsoft Defender";
    case core::SystemEventSource::windows_update: return "Windows Update";
    case core::SystemEventSource::application: return "Application";
    case core::SystemEventSource::network: return "Network";
    case core::SystemEventSource::graphics: return "Graphics";
    case core::SystemEventSource::storage: return "Storage";
    case core::SystemEventSource::process: return "Process";
    }
    return "Unknown";
}

[[nodiscard]] const char* event_kind_text(const core::SystemEventKind kind) noexcept {
    switch (kind) {
    case core::SystemEventKind::suspend: return "System suspended";
    case core::SystemEventKind::resume_automatic: return "System resumed automatically";
    case core::SystemEventKind::resume_user: return "System resumed for user";
    case core::SystemEventKind::device_enumerated: return "Device enumerated";
    case core::SystemEventKind::device_started: return "Device started";
    case core::SystemEventKind::device_removed: return "Device removed";
    case core::SystemEventKind::audio_endpoint_added: return "Audio endpoint added";
    case core::SystemEventKind::audio_endpoint_removed: return "Audio endpoint removed";
    case core::SystemEventKind::audio_endpoint_state_changed: return "Audio endpoint state changed";
    case core::SystemEventKind::audio_default_changed: return "Default audio endpoint changed";
    case core::SystemEventKind::service_state_changed: return "Service state changed";
    case core::SystemEventKind::service_unexpected_stop: return "Service stopped unexpectedly";
    case core::SystemEventKind::defender_scan_started: return "Defender scan started";
    case core::SystemEventKind::defender_scan_completed: return "Defender scan completed";
    case core::SystemEventKind::defender_threat_detected: return "Defender threat detected";
    case core::SystemEventKind::defender_action: return "Defender action";
    case core::SystemEventKind::defender_configuration_changed: return "Defender configuration changed";
    case core::SystemEventKind::update_activity_started: return "Update activity started";
    case core::SystemEventKind::update_succeeded: return "Update succeeded";
    case core::SystemEventKind::update_failed: return "Update failed";
    case core::SystemEventKind::application_started: return "Application started";
    case core::SystemEventKind::application_terminated: return "Application terminated";
    case core::SystemEventKind::application_crash: return "Application crash reported";
    case core::SystemEventKind::application_hang: return "Application hang reported";
    case core::SystemEventKind::network_connectivity_changed:
        return "Network connectivity changed";
    case core::SystemEventKind::dns_resolution_timeout:
        return "DNS resolution timeout reported";
    case core::SystemEventKind::display_configuration_changed:
        return "Display configuration changed";
    case core::SystemEventKind::display_driver_recovery:
        return "Display timeout recovery reported";
    case core::SystemEventKind::storage_device_added: return "Storage device added";
    case core::SystemEventKind::storage_device_removed: return "Storage device removed";
    case core::SystemEventKind::storage_io_retry:
        return "Storage I/O retry reported";
    case core::SystemEventKind::process_started: return "Process started";
    case core::SystemEventKind::process_exited: return "Process exited";
    }
    return "Unknown event";
}

[[nodiscard]] const char* event_level_text(const core::SystemEventLevel level) noexcept {
    switch (level) {
    case core::SystemEventLevel::informational: return "Info";
    case core::SystemEventLevel::warning: return "Warning";
    case core::SystemEventLevel::error: return "Error";
    }
    return "Unknown";
}

template <typename Value>
void update_peak(bool& available, double& peak,
                 const core::RecordedValue<Value>& value, const double multiplier) {
    if (value.status == core::RecordedValueStatus::available) {
        available = true;
        peak = std::max(peak, static_cast<double>(value.value) * multiplier);
    }
}

} // namespace

std::string format_utc_milliseconds(const std::int64_t epoch_milliseconds) {
    using namespace std::chrono;
    const sys_time<std::chrono::milliseconds> timestamp{
        std::chrono::milliseconds{epoch_milliseconds}};
    const auto day = floor<days>(timestamp);
    const year_month_day date{day};
    const hh_mm_ss time{timestamp - day};
    // Keep room for the full signed chrono year range as well as the normal
    // four-digit product representation; truncation must never produce an
    // apparently valid timestamp.
    char text[64]{};
    std::snprintf(text, sizeof(text), "%04d-%02u-%02u %02lld:%02lld:%02lld.%03lld UTC",
                  static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                  static_cast<unsigned>(date.day()),
                  static_cast<long long>(time.hours().count()),
                  static_cast<long long>(time.minutes().count()),
                  static_cast<long long>(time.seconds().count()),
                  static_cast<long long>(time.subseconds().count()));
    return text;
}

IncidentDetailView build_incident_detail(
    const std::int64_t id, const std::int64_t created_utc_milliseconds,
    std::string label, std::string note, const core::IncidentSnapshot& incident,
    const std::optional<core::IncidentProcessIdentity> selected_process,
    const std::size_t maximum_plot_points) {
    IncidentDetailView result{};
    result.id = id;
    result.created_utc = format_utc_milliseconds(created_utc_milliseconds);
    result.label = std::move(label);
    result.note = std::move(note);
    result.selected_process = selected_process;
    const auto& header = incident.header();
    const auto event = header.window.event_time;
    result.requested_start_seconds = seconds_from(header.window.requested_start, event);
    result.requested_end_seconds = seconds_from(header.window.requested_end, event);
    result.actual_start_seconds = seconds_from(header.actual_start, event);
    result.actual_end_seconds = seconds_from(header.actual_end, event);
    result.trigger_count = header.window.trigger_count;
    result.manual_trigger_count = header.window.manual_trigger_count;
    result.automatic_trigger_count = header.window.automatic_trigger_count;
    result.automatic_resource = header.window.automatic_resource;
    result.automatic_observed_value = header.window.automatic_observed_value;
    result.automatic_baseline_value = header.window.automatic_baseline_value;
    result.automatic_score = header.window.automatic_score;
    result.automatic_signal = header.window.automatic_signal;
    result.system_sample_count = incident.system_samples().size();
    result.process_sample_count = incident.process_samples().size();

    for (const auto& sample : incident.system_samples()) {
        append_metric(result.cpu_percent, sample, sample.cpu_fraction, event,
                      [](const double value) { return value * 100.0; });
        append_metric(result.memory_percent, sample, sample.memory_fraction, event,
                      [](const double value) { return value * 100.0; });
        append_metric(result.disk_read_mib_per_second, sample,
                      sample.disk_read_bytes_per_second, event,
                      [](const double value) { return value / bytes_per_mebibyte; });
        append_metric(result.disk_write_mib_per_second, sample,
                      sample.disk_write_bytes_per_second, event,
                      [](const double value) { return value / bytes_per_mebibyte; });
        append_metric(result.network_receive_mib_per_second, sample,
                      sample.network_receive_bytes_per_second, event,
                      [](const double value) { return value / bytes_per_mebibyte; });
        append_metric(result.network_transmit_mib_per_second, sample,
                      sample.network_transmit_bytes_per_second, event,
                      [](const double value) { return value / bytes_per_mebibyte; });
        append_metric(result.disk_read_latency_milliseconds, sample,
                      sample.disk_read_latency_seconds, event,
                      [](const double value) { return value * 1'000.0; });
        append_metric(result.disk_write_latency_milliseconds, sample,
                      sample.disk_write_latency_seconds, event,
                      [](const double value) { return value * 1'000.0; });
        append_metric(result.disk_service_time_milliseconds, sample,
                      sample.disk_service_time_seconds, event,
                      [](const double value) { return value * 1'000.0; });
        append_metric(result.disk_queue_depth, sample, sample.disk_queue_depth, event,
                      [](const double value) { return value; });
        append_metric(result.network_connectivity_level, sample,
                      sample.network_connectivity_level, event,
                      [](const std::uint8_t value) {
                          return static_cast<double>(value);
                      });
        append_metric(result.network_interface_changes, sample,
                      sample.network_interface_changes, event,
                      [](const std::uint64_t value) {
                          return static_cast<double>(value);
                      });
        append_metric(result.network_tcp_retransmit_percent, sample,
                      sample.network_tcp_retransmit_fraction, event,
                      [](const double value) { return value * 100.0; });
        append_metric(result.network_tcp_failed_connections, sample,
                      sample.network_tcp_failed_connections, event,
                      [](const std::uint64_t value) {
                          return static_cast<double>(value);
                      });
        append_metric(result.network_tcp_resets, sample, sample.network_tcp_resets,
                      event, [](const std::uint64_t value) {
                          return static_cast<double>(value);
                      });
        append_metric(result.gpu_percent, sample, sample.gpu_fraction, event,
                      [](const double value) { return value * 100.0; });
        append_metric(result.gpu_dedicated_memory_mib, sample,
                      sample.gpu_dedicated_memory_bytes, event,
                      [](const std::uint64_t value) {
                          return static_cast<double>(value) / bytes_per_mebibyte;
                      });
        append_metric(result.gpu_shared_memory_mib, sample,
                      sample.gpu_shared_memory_bytes, event,
                      [](const std::uint64_t value) {
                          return static_cast<double>(value) / bytes_per_mebibyte;
                      });
        append_metric(result.foreground_gpu_percent, sample,
                      sample.foreground_gpu_fraction, event,
                      [](const double value) { return value * 100.0; });
        append_metric(result.dpc_percent, sample, sample.dpc_fraction, event,
                      [](const double value) { return value * 100.0; });
        append_metric(result.interrupt_percent, sample, sample.interrupt_fraction, event,
                      [](const double value) { return value * 100.0; });
        append_metric(result.dpc_rate, sample, sample.dpc_rate, event,
                      [](const double value) { return value; });
        append_metric(result.cpu_current_mhz, sample, sample.cpu_current_mhz, event,
                      [](const double value) { return value; });
        append_metric(result.cpu_thermal_limit_mhz, sample,
                      sample.cpu_thermal_limit_mhz, event,
                      [](const double value) { return value; });
        append_metric(result.cpu_thermal_limit_percent, sample,
                      sample.cpu_thermal_limit_fraction, event,
                      [](const double value) { return value * 100.0; });
        append_metric(result.battery_percent, sample, sample.battery_fraction, event,
                      [](const double value) { return value * 100.0; });
    }

    std::unordered_map<core::IncidentProcessIdentity, std::size_t, IdentityHash> row_by_id;
    row_by_id.reserve(incident.process_metadata().size());
    result.processes.reserve(incident.process_metadata().size());
    for (const auto& metadata : incident.process_metadata()) {
        IncidentProcessRow row{};
        row.identity = metadata.identity;
        row.name = metadata.name.status == core::RecordedValueStatus::available
                       ? metadata.name.value
                       : std::string{"PID "} + std::to_string(metadata.identity.pid);
        if (metadata.executable_path.status == core::RecordedValueStatus::available) {
            row.executable_path = metadata.executable_path.value;
        }
        row_by_id.emplace(row.identity, result.processes.size());
        result.processes.push_back(std::move(row));
    }
    for (const auto& sample : incident.process_samples()) {
        auto found = row_by_id.find(sample.identity);
        if (found == row_by_id.end()) {
            IncidentProcessRow row{};
            row.identity = sample.identity;
            row.name = std::string{"PID "} + std::to_string(sample.identity.pid);
            found = row_by_id.emplace(row.identity, result.processes.size()).first;
            result.processes.push_back(std::move(row));
        }
        auto& row = result.processes[found->second];
        ++row.sample_count;
        update_peak(row.cpu_available, row.peak_cpu_percent, sample.cpu_fraction, 100.0);
        update_peak(row.working_set_available, row.peak_working_set_mib,
                    sample.working_set_bytes, 1.0 / bytes_per_mebibyte);
        update_peak(row.disk_read_available, row.peak_disk_read_mib_per_second,
                    sample.disk_read_bytes_per_second, 1.0 / bytes_per_mebibyte);
        update_peak(row.disk_write_available, row.peak_disk_write_mib_per_second,
                    sample.disk_write_bytes_per_second, 1.0 / bytes_per_mebibyte);
        if (selected_process && sample.identity == *selected_process) {
            append_metric(result.selected_process_cpu_percent, sample, sample.cpu_fraction,
                          event, [](const double value) { return value * 100.0; });
            append_metric(result.selected_process_working_set_mib, sample,
                          sample.working_set_bytes, event,
                          [](const std::uint64_t value) {
                              return static_cast<double>(value) / bytes_per_mebibyte;
                          });
            append_metric(result.selected_process_disk_read_mib_per_second, sample,
                          sample.disk_read_bytes_per_second, event,
                          [](const double value) { return value / bytes_per_mebibyte; });
            append_metric(result.selected_process_disk_write_mib_per_second, sample,
                          sample.disk_write_bytes_per_second, event,
                          [](const double value) { return value / bytes_per_mebibyte; });
        }
    }

    std::optional<core::IncidentProcessIdentity> previous_foreground;
    for (const auto& sample : incident.system_samples()) {
        if (sample.foreground_process.status != core::RecordedValueStatus::available ||
            (previous_foreground.has_value() &&
             *previous_foreground == sample.foreground_process.value)) {
            continue;
        }
        previous_foreground = sample.foreground_process.value;
        ForegroundApplicationRow row{};
        row.seconds_from_event = seconds_from(sample.observed_at, event);
        row.identity = sample.foreground_process.value;
        const auto found = row_by_id.find(row.identity);
        row.name = found != row_by_id.end()
                       ? result.processes[found->second].name
                       : std::string{"PID "} + std::to_string(row.identity.pid);
        if (sample.foreground_gpu_fraction.status ==
            core::RecordedValueStatus::available) {
            row.gpu_available = true;
            row.gpu_percent = sample.foreground_gpu_fraction.value * 100.0;
        }
        result.foreground_applications.push_back(std::move(row));
    }
    result.system_events.reserve(incident.system_events().size());
    for (const auto& recorded_event : incident.system_events()) {
        std::string description{event_kind_text(recorded_event.kind)};
        if (recorded_event.has_process_identity) {
            const core::IncidentProcessIdentity identity{
                recorded_event.process_pid,
                recorded_event.process_creation_token};
            const auto found = row_by_id.find(identity);
            description += ": ";
            description += found != row_by_id.end()
                               ? result.processes[found->second].name
                               : std::string{"PID "} +
                                     std::to_string(recorded_event.process_pid);
        }
        result.system_events.push_back(SystemEventRow{
            seconds_from(recorded_event.observed_at, event),
            event_source_text(recorded_event.source),
            std::move(description),
            event_level_text(recorded_event.level),
            recorded_event.native_event_id});
    }

    for (auto* series : {&result.cpu_percent, &result.memory_percent,
                         &result.disk_read_mib_per_second,
                         &result.disk_write_mib_per_second,
                         &result.network_receive_mib_per_second,
                         &result.network_transmit_mib_per_second,
                         &result.disk_read_latency_milliseconds,
                         &result.disk_write_latency_milliseconds,
                         &result.disk_service_time_milliseconds,
                         &result.disk_queue_depth,
                         &result.network_connectivity_level,
                         &result.network_interface_changes,
                         &result.network_tcp_retransmit_percent,
                         &result.network_tcp_failed_connections,
                         &result.network_tcp_resets,
                         &result.gpu_percent,
                         &result.gpu_dedicated_memory_mib,
                         &result.gpu_shared_memory_mib,
                         &result.foreground_gpu_percent,
                         &result.dpc_percent,
                         &result.interrupt_percent,
                         &result.dpc_rate,
                         &result.cpu_current_mhz,
                         &result.cpu_thermal_limit_mhz,
                         &result.cpu_thermal_limit_percent,
                         &result.battery_percent,
                         &result.selected_process_cpu_percent,
                         &result.selected_process_working_set_mib,
                         &result.selected_process_disk_read_mib_per_second,
                         &result.selected_process_disk_write_mib_per_second}) {
        downsample(*series, maximum_plot_points);
    }
    return result;
}

std::vector<std::size_t> filter_and_sort_processes(
    const std::vector<IncidentProcessRow>& rows, const std::string& filter,
    const IncidentProcessSort sort, const bool ascending,
    const std::size_t maximum_results) {
    const auto needle = lower(filter);
    std::vector<std::size_t> indices;
    indices.reserve(std::min(rows.size(), maximum_results));
    for (std::size_t index = 0U; index < rows.size(); ++index) {
        const auto& row = rows[index];
        const auto haystack = lower(row.name + " " + row.executable_path + " " +
                                    std::to_string(row.identity.pid));
        if (needle.empty() || haystack.find(needle) != std::string::npos) {
            indices.push_back(index);
        }
    }
    const auto compare = [&](const std::size_t left_index, const std::size_t right_index) {
        const auto& left = rows[left_index];
        const auto& right = rows[right_index];
        const auto ordered = [ascending, left_index, right_index](const auto& left_value,
                                                                 const auto& right_value) {
            if (left_value == right_value) return left_index < right_index;
            return ascending ? left_value < right_value : left_value > right_value;
        };
        switch (sort) {
        case IncidentProcessSort::name:
            return ordered(lower(left.name), lower(right.name));
        case IncidentProcessSort::pid:
            return ordered(left.identity.pid, right.identity.pid);
        case IncidentProcessSort::peak_cpu:
            return ordered(left.peak_cpu_percent, right.peak_cpu_percent);
        case IncidentProcessSort::peak_working_set:
            return ordered(left.peak_working_set_mib, right.peak_working_set_mib);
        case IncidentProcessSort::peak_disk_read:
            return ordered(left.peak_disk_read_mib_per_second,
                           right.peak_disk_read_mib_per_second);
        case IncidentProcessSort::peak_disk_write:
            return ordered(left.peak_disk_write_mib_per_second,
                           right.peak_disk_write_mib_per_second);
        }
        return left_index < right_index;
    };
    std::stable_sort(indices.begin(), indices.end(), compare);
    if (indices.size() > maximum_results) indices.resize(maximum_results);
    return indices;
}

void synchronize_incident_editor(IncidentViewerState& state) {
    if (!state.content || state.synchronized_generation == state.content->generation) return;
    state.synchronized_generation = state.content->generation;
    state.visible_process_indices.clear();
    if (!state.content->detail) {
        state.editor_incident_id = 0;
        state.label_editor.fill('\0');
        state.note_editor.fill('\0');
        state.recurring_group_override_editor.fill('\0');
        state.category_editor = IncidentCategory::unknown;
        return;
    }
    const auto& detail = *state.content->detail;
    state.editor_incident_id = detail.id;
    const auto copy = [](auto& destination, const std::string& source) {
        destination.fill('\0');
        const auto count = std::min(destination.size() - 1U, source.size());
        std::copy_n(source.begin(), count, destination.begin());
    };
    copy(state.label_editor, detail.label);
    copy(state.note_editor, detail.note);
    copy(state.recurring_group_override_editor,
         detail.recurring_group_override);
    state.category_editor = detail.category;
    state.visible_process_indices = filter_and_sort_processes(
        detail.processes, state.process_filter.data(), state.process_sort,
        state.process_sort_ascending);
}

} // namespace blackbox::ui
