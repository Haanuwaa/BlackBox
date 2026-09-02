#include "evaluation/truth_review.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace blackbox::evaluation {
namespace {

[[nodiscard]] TruthReviewError review_error(const TruthReviewErrorCode code, std::string message) {
    return TruthReviewError{code, std::move(message)};
}

[[nodiscard]] bool valid_key(const std::string_view value) noexcept {
    return value.size() == 32U && std::ranges::all_of(value, [](const char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] constexpr std::string_view
status_name(const core::RecordedValueStatus value) noexcept {
    switch (value) {
    case core::RecordedValueStatus::available:
        return "available";
    case core::RecordedValueStatus::unsupported:
        return "unsupported";
    case core::RecordedValueStatus::inaccessible:
        return "inaccessible";
    case core::RecordedValueStatus::temporarily_unavailable:
        return "temporarily_unavailable";
    }
    return "invalid";
}

[[nodiscard]] constexpr std::string_view source_name(const core::SystemEventSource value) noexcept {
    switch (value) {
    case core::SystemEventSource::power:
        return "power";
    case core::SystemEventSource::device:
        return "device";
    case core::SystemEventSource::audio:
        return "audio";
    case core::SystemEventSource::service_manager:
        return "service";
    case core::SystemEventSource::security:
        return "security";
    case core::SystemEventSource::update:
        return "update";
    case core::SystemEventSource::application:
        return "application";
    case core::SystemEventSource::network:
        return "network";
    case core::SystemEventSource::graphics:
        return "graphics";
    case core::SystemEventSource::storage:
        return "storage";
    case core::SystemEventSource::process:
        return "process";
    }
    return "invalid";
}

[[nodiscard]] constexpr std::string_view kind_name(const core::SystemEventKind value) noexcept {
    constexpr std::array names{"suspend",
                               "resume_automatic",
                               "resume_user",
                               "device_enumerated",
                               "device_started",
                               "device_removed",
                               "audio_endpoint_added",
                               "audio_endpoint_removed",
                               "audio_endpoint_state_changed",
                               "audio_default_changed",
                               "service_state_changed",
                               "service_unexpected_stop",
                               "security_scan_started",
                               "security_scan_completed",
                               "security_threat_detected",
                               "security_action",
                               "security_configuration_changed",
                               "update_activity_started",
                               "update_succeeded",
                               "update_failed",
                               "application_started",
                               "application_terminated",
                               "application_crash",
                               "application_hang",
                               "network_connectivity_changed",
                               "dns_resolution_timeout",
                               "display_configuration_changed",
                               "display_driver_recovery",
                               "storage_device_added",
                               "storage_device_removed",
                               "storage_io_retry",
                               "process_started",
                               "process_exited"};
    const auto index = static_cast<std::size_t>(value);
    return index < names.size() ? names[index] : "invalid";
}

[[nodiscard]] constexpr std::string_view level_name(const core::SystemEventLevel value) noexcept {
    switch (value) {
    case core::SystemEventLevel::informational:
        return "informational";
    case core::SystemEventLevel::warning:
        return "warning";
    case core::SystemEventLevel::error:
        return "error";
    }
    return "invalid";
}

[[nodiscard]] double relative_seconds(const core::MonotonicTimePoint value,
                                      const core::MonotonicTimePoint event) noexcept {
    return std::chrono::duration<double>(value - event).count();
}

template <typename Value>
void write_recorded(std::ostream& output, const core::RecordedValue<Value>& value) {
    output << status_name(value.status) << '\t';
    if (value.status == core::RecordedValueStatus::available) output << +value.value;
}

template <typename Value>
void write_json_value(std::ostream& output, const core::RecordedValue<Value>& value,
                      const double multiplier = 1.0) {
    if (value.status != core::RecordedValueStatus::available) {
        output << "null";
        return;
    }
    output << static_cast<double>(value.value) * multiplier;
}

[[nodiscard]] std::string tsv_text(std::string_view value) {
    std::string result;
    result.reserve((std::min<std::size_t>)(value.size(), 260U));
    for (const char character : value.substr(0U, 260U)) {
        result.push_back(character == '\t' || character == '\r' || character == '\n' ? ' '
                                                                                     : character);
    }
    return result;
}

[[nodiscard]] std::string html_text(const std::string_view value) {
    std::string result;
    for (const char character : value) {
        switch (character) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        case '\'':
            result += "&#39;";
            break;
        default:
            result.push_back(character);
            break;
        }
    }
    return result;
}

[[nodiscard]] std::string json_text(const std::string_view value) {
    std::ostringstream output;
    output << '"';
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20U) {
                constexpr char digits[] = "0123456789abcdef";
                output << "\\u00" << digits[character >> 4U] << digits[character & 0x0FU];
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    output << '"';
    return output.str();
}

struct ProcessReview {
    core::IncidentProcessIdentity identity{};
    std::size_t ordinal{};
    std::string label{};
    std::vector<const core::IncidentProcessSample*> samples{};
    double peak_cpu_percent{};
    double peak_working_set_mib{};
    double peak_disk_mib_per_second{};
};

[[nodiscard]] std::vector<ProcessReview> process_reviews(const core::IncidentSnapshot& incident,
                                                         const bool include_identities) {
    std::map<core::IncidentProcessIdentity, std::size_t> ordinal_by_identity;
    for (const auto& sample : incident.process_samples()) {
        if (!ordinal_by_identity.contains(sample.identity)) {
            ordinal_by_identity.emplace(sample.identity, ordinal_by_identity.size());
        }
    }
    for (const auto& event : incident.system_events()) {
        if (!event.has_process_identity) continue;
        const core::IncidentProcessIdentity identity{event.process_pid,
                                                     event.process_creation_token};
        if (!ordinal_by_identity.contains(identity)) {
            ordinal_by_identity.emplace(identity, ordinal_by_identity.size());
        }
    }
    std::map<core::IncidentProcessIdentity, std::string> names;
    if (include_identities) {
        for (const auto& metadata : incident.process_metadata()) {
            if (metadata.name.status == core::RecordedValueStatus::available) {
                names.emplace(metadata.identity, tsv_text(metadata.name.value));
            }
        }
    }
    std::vector<ProcessReview> result(ordinal_by_identity.size());
    for (const auto& [identity, ordinal] : ordinal_by_identity) {
        auto& row = result[ordinal];
        row.identity = identity;
        row.ordinal = ordinal;
        row.label = "Process ordinal " + std::to_string(ordinal);
        if (include_identities) {
            if (const auto name = names.find(identity); name != names.end()) {
                row.label += " - " + name->second;
            } else {
                row.label += " - <name unavailable>";
            }
        }
    }
    for (const auto& sample : incident.process_samples()) {
        auto& row = result[ordinal_by_identity.at(sample.identity)];
        row.samples.push_back(&sample);
        if (sample.cpu_fraction.status == core::RecordedValueStatus::available) {
            row.peak_cpu_percent =
                (std::max)(row.peak_cpu_percent, sample.cpu_fraction.value * 100.0);
        }
        if (sample.working_set_bytes.status == core::RecordedValueStatus::available) {
            row.peak_working_set_mib =
                (std::max)(row.peak_working_set_mib,
                           static_cast<double>(sample.working_set_bytes.value) / (1024.0 * 1024.0));
        }
        for (const auto* rate :
             {&sample.disk_read_bytes_per_second, &sample.disk_write_bytes_per_second}) {
            if (rate->status == core::RecordedValueStatus::available) {
                row.peak_disk_mib_per_second =
                    (std::max)(row.peak_disk_mib_per_second, rate->value / (1024.0 * 1024.0));
            }
        }
    }
    return result;
}

[[nodiscard]] bool stream_ok(std::ofstream& output) {
    output.flush();
    return static_cast<bool>(output);
}

} // namespace

std::expected<TruthReviewStatistics, TruthReviewError>
export_truth_review(const core::IncidentSnapshot& incident, const std::string_view incident_key,
                    const std::int64_t created_utc_milliseconds,
                    const std::filesystem::path& destination,
                    const TruthReviewOptions options) noexcept {
    try {
        if (!valid_key(incident_key) || destination.empty() || destination.filename().empty() ||
            destination.filename() == "." || destination.filename() == "..") {
            return std::unexpected{review_error(TruthReviewErrorCode::invalid_input,
                                                "invalid truth-review identity or destination")};
        }
        const auto& systems = incident.system_samples();
        const auto& samples = incident.process_samples();
        const auto& events = incident.system_events();
        auto processes = process_reviews(incident, options.include_local_process_identities);
        std::map<core::IncidentProcessIdentity, std::size_t> process_ordinals;
        for (const auto& process : processes) {
            process_ordinals.emplace(process.identity, process.ordinal);
        }
        if (systems.size() > maximum_truth_review_system_samples ||
            samples.size() > maximum_truth_review_process_samples ||
            processes.size() > maximum_truth_review_processes ||
            events.size() > maximum_truth_review_system_events) {
            return std::unexpected{review_error(TruthReviewErrorCode::limit_exceeded,
                                                "truth-review input exceeds a hard row bound")};
        }

        auto staging = destination;
        staging += ".partial";
        std::error_code issue;
        if (std::filesystem::exists(destination, issue) || issue ||
            std::filesystem::exists(staging, issue) || issue) {
            return std::unexpected{review_error(TruthReviewErrorCode::destination_exists,
                                                "truth-review destination or staging exists")};
        }
        const auto parent = destination.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, issue);
            if (issue) {
                return std::unexpected{
                    review_error(TruthReviewErrorCode::io, "cannot create truth-review parent")};
            }
        }
        if (!std::filesystem::create_directory(staging, issue) || issue) {
            return std::unexpected{
                review_error(TruthReviewErrorCode::io, "cannot create truth-review staging")};
        }

        const auto event_time = incident.header().window.event_time;
        {
            std::ofstream output{staging / "manifest.ini", std::ios::binary};
            output << "format=" << truth_review_format_version << '\n'
                   << "artifact=blackbox-truth-review\n"
                   << "incident_key=" << incident_key << '\n'
                   << "created_utc_ms=" << created_utc_milliseconds << '\n'
                   << "prediction_free=1\n"
                   << "local_process_identities="
                   << (options.include_local_process_identities ? 1 : 0) << '\n'
                   << "system_samples=" << systems.size() << '\n'
                   << "process_samples=" << samples.size() << '\n'
                   << "processes=" << processes.size() << '\n'
                   << "system_events=" << events.size() << '\n';
            if (!stream_ok(output)) {
                return std::unexpected{
                    review_error(TruthReviewErrorCode::io, "cannot write truth-review manifest")};
            }
        }
        {
            std::ofstream output{staging / "system-samples.tsv", std::ios::binary};
            output << std::setprecision(17)
                   << "offset_seconds\tcpu_status\tcpu_fraction\tmemory_"
                      "status\tmemory_fraction\t"
                      "disk_read_status\tdisk_read_bps\tdisk_write_status\tdisk_"
                      "write_bps\t"
                      "disk_read_latency_status\tdisk_read_latency_seconds\t"
                      "disk_write_latency_status\tdisk_write_latency_seconds\t"
                      "disk_service_status\tdisk_service_seconds\tdisk_queue_"
                      "status\tdisk_queue\tdisk_service_concurrency_status\t"
                      "disk_service_concurrency\t"
                      "network_receive_status\tnetwork_receive_bps\tnetwork_"
                      "transmit_"
                      "status\t"
                      "network_transmit_bps\tretransmit_status\tretransmit_"
                      "fraction\t"
                      "failed_connections_status\tfailed_connections\treset_"
                      "status\tresets\t"
                      "gpu_status\tgpu_fraction\tdpc_status\tdpc_"
                      "fraction\tinterrupt_"
                      "status\t"
                      "interrupt_fraction\tfrequency_status\tfrequency_"
                      "mhz\tthermal_"
                      "status\t"
                      "thermal_fraction\tbattery_status\tbattery_fraction\tuptime_"
                      "status\tuptime_seconds\t"
                      "cpu_some_pressure_status\tcpu_some_pressure_fraction\t"
                      "memory_some_pressure_status\tmemory_some_pressure_"
                      "fraction\t"
                      "memory_full_pressure_status\tmemory_full_pressure_"
                      "fraction\t"
                      "io_some_pressure_status\tio_some_pressure_fraction\t"
                      "io_full_pressure_status\tio_full_pressure_fraction\t"
                      "thermal_pressure_status\tthermal_pressure_state\t"
                      "memory_pressure_status\tmemory_pressure_state\t"
                      "compressed_memory_status\tcompressed_memory_bytes\t"
                      "page_out_status\tpage_out_bps\tswap_in_status\tswap_in_bps\t"
                      "swap_out_status\tswap_out_bps\tcompression_status\tcompression_bps\t"
                      "decompression_status\tdecompression_bps\tscheduler_delay_status\t"
                      "scheduler_delay_seconds\tlogical_processors_status\tlogical_processors\t"
                      "physical_processors_status\tphysical_processors\tactive_processors_status\t"
                      "active_processors\n";
            for (const auto& sample : systems) {
                output << relative_seconds(sample.observed_at, event_time) << '\t';
                write_recorded(output, sample.cpu_fraction);
                output << '\t';
                write_recorded(output, sample.memory_fraction);
                output << '\t';
                write_recorded(output, sample.disk_read_bytes_per_second);
                output << '\t';
                write_recorded(output, sample.disk_write_bytes_per_second);
                output << '\t';
                write_recorded(output, sample.disk_read_latency_seconds);
                output << '\t';
                write_recorded(output, sample.disk_write_latency_seconds);
                output << '\t';
                write_recorded(output, sample.disk_service_time_seconds);
                output << '\t';
                write_recorded(output, sample.disk_queue_depth);
                output << '\t';
                write_recorded(output, sample.disk_service_concurrency);
                output << '\t';
                write_recorded(output, sample.network_receive_bytes_per_second);
                output << '\t';
                write_recorded(output, sample.network_transmit_bytes_per_second);
                output << '\t';
                write_recorded(output, sample.network_tcp_retransmit_fraction);
                output << '\t';
                write_recorded(output, sample.network_tcp_failed_connections);
                output << '\t';
                write_recorded(output, sample.network_tcp_resets);
                output << '\t';
                write_recorded(output, sample.gpu_fraction);
                output << '\t';
                write_recorded(output, sample.dpc_fraction);
                output << '\t';
                write_recorded(output, sample.interrupt_fraction);
                output << '\t';
                write_recorded(output, sample.cpu_current_mhz);
                output << '\t';
                write_recorded(output, sample.cpu_thermal_limit_fraction);
                output << '\t';
                write_recorded(output, sample.battery_fraction);
                output << '\t';
                write_recorded(output, sample.system_uptime_seconds);
                output << '\t';
                write_recorded(output, sample.cpu_some_pressure_fraction);
                output << '\t';
                write_recorded(output, sample.memory_some_pressure_fraction);
                output << '\t';
                write_recorded(output, sample.memory_full_pressure_fraction);
                output << '\t';
                write_recorded(output, sample.io_some_pressure_fraction);
                output << '\t';
                write_recorded(output, sample.io_full_pressure_fraction);
                output << '\t';
                write_recorded(output, sample.thermal_pressure_state);
                output << '\t';
                write_recorded(output, sample.memory_pressure_state);
                output << '\t';
                write_recorded(output, sample.compressed_memory_bytes);
                output << '\t';
                write_recorded(output, sample.memory_page_out_bytes_per_second);
                output << '\t';
                write_recorded(output, sample.memory_swap_in_bytes_per_second);
                output << '\t';
                write_recorded(output, sample.memory_swap_out_bytes_per_second);
                output << '\t';
                write_recorded(output, sample.memory_compression_bytes_per_second);
                output << '\t';
                write_recorded(output, sample.memory_decompression_bytes_per_second);
                output << '\t';
                write_recorded(output, sample.scheduler_delay_seconds);
                output << '\t';
                write_recorded(output, sample.logical_processor_count);
                output << '\t';
                write_recorded(output, sample.physical_processor_count);
                output << '\t';
                write_recorded(output, sample.active_processor_count);
                output << '\n';
            }
            if (!stream_ok(output)) {
                return std::unexpected{
                    review_error(TruthReviewErrorCode::io, "cannot write system truth evidence")};
            }
        }
        {
            std::ofstream output{staging / "processes.tsv", std::ios::binary};
            output << std::setprecision(17)
                   << "process_ordinal\tlocal_pid\tlocal_name\tsample_count\tpeak_"
                      "cpu_percent\t"
                      "peak_working_set_mib\tpeak_disk_mib_per_second\n";
            std::map<core::IncidentProcessIdentity, std::string> names;
            if (options.include_local_process_identities) {
                for (const auto& metadata : incident.process_metadata()) {
                    if (metadata.name.status == core::RecordedValueStatus::available) {
                        names.emplace(metadata.identity, tsv_text(metadata.name.value));
                    }
                }
            }
            for (const auto& process : processes) {
                output << process.ordinal << '\t';
                if (options.include_local_process_identities) {
                    output << process.identity.pid << '\t';
                    if (const auto name = names.find(process.identity); name != names.end()) {
                        output << name->second;
                    }
                } else {
                    output << '\t';
                }
                output << '\t' << process.samples.size() << '\t' << process.peak_cpu_percent << '\t'
                       << process.peak_working_set_mib << '\t' << process.peak_disk_mib_per_second
                       << '\n';
            }
            if (!stream_ok(output)) {
                return std::unexpected{
                    review_error(TruthReviewErrorCode::io, "cannot write process truth legend")};
            }
        }
        {
            std::ofstream output{staging / "process-samples.tsv", std::ios::binary};
            output << std::setprecision(17)
                   << "offset_seconds\tprocess_ordinal\tcpu_status\tcpu_fraction\t"
                      "working_set_status\tworking_set_bytes\tdisk_read_"
                      "status\tdisk_"
                      "read_bps\t"
                      "disk_write_status\tdisk_write_bps\n";
            for (const auto& process : processes) {
                for (const auto* sample : process.samples) {
                    output << relative_seconds(sample->observed_at, event_time) << '\t'
                           << process.ordinal << '\t';
                    write_recorded(output, sample->cpu_fraction);
                    output << '\t';
                    write_recorded(output, sample->working_set_bytes);
                    output << '\t';
                    write_recorded(output, sample->disk_read_bytes_per_second);
                    output << '\t';
                    write_recorded(output, sample->disk_write_bytes_per_second);
                    output << '\n';
                }
            }
            if (!stream_ok(output)) {
                return std::unexpected{
                    review_error(TruthReviewErrorCode::io, "cannot write process truth evidence")};
            }
        }
        {
            std::ofstream output{staging / "system-events.tsv", std::ios::binary};
            output << std::setprecision(17)
                   << "offset_seconds\tsource\tkind\tlevel\tnative_event_"
                      "id\tdetail\tprocess_ordinal\n";
            for (const auto& event : events) {
                output << relative_seconds(event.observed_at, event_time) << '\t'
                       << source_name(event.source) << '\t' << kind_name(event.kind) << '\t'
                       << level_name(event.level) << '\t' << event.native_event_id << '\t'
                       << event.detail << '\t';
                if (event.has_process_identity) {
                    const auto found =
                        process_ordinals.find({event.process_pid, event.process_creation_token});
                    if (found != process_ordinals.end()) output << found->second;
                }
                output << '\n';
            }
            if (!stream_ok(output)) {
                return std::unexpected{
                    review_error(TruthReviewErrorCode::io, "cannot write event truth evidence")};
            }
        }
        {
            std::ofstream output{staging / "ballot-template.tsv", std::ios::binary};
            output << "incident_key\tannotator_id\tsymptom\tcertainty\tuser_"
                      "visible\t"
                      "expected_diagnosis\texpected_contributor_ordinal\texpected_"
                      "context\t"
                      "recurrence_family\tusefulness\n"
                   << incident_key << "\t\t\t\t\t\t\t\t\t\n";
            if (!stream_ok(output)) {
                return std::unexpected{review_error(TruthReviewErrorCode::io,
                                                    "cannot write independent ballot template")};
            }
        }
        {
            std::ofstream output{staging / "review.html", std::ios::binary};
            output << std::setprecision(17)
                   << "<!doctype html><html lang=\"en\"><head><meta "
                      "charset=\"utf-8\">"
                      "<meta name=\"viewport\" "
                      "content=\"width=device-width,initial-scale=1\">"
                      "<title>BlackBox truth review</title><style>"
                      "body{font:15px "
                      "system-ui;background:#10141c;color:#edf2f7;margin:24px;"
                      "max-width:"
                      "1400px}"
                      "h1,h2{color:#fff} .notice{padding:12px;border:1px solid "
                      "#60708b;background:#182131}"
                      "canvas{width:100%;height:320px;background:#090d13;"
                      "border:1px "
                      "solid #334155}"
                      "select{background:#111827;color:#fff;padding:6px}table{"
                      "border-"
                      "collapse:collapse;width:100%}"
                      "th,td{border-bottom:1px solid "
                      "#334155;padding:6px;text-align:left}small{color:#aebbd0}"
                      "</style></head><body><h1>Independent incident truth "
                      "review</h1><div class=\"notice\">"
                      "This artifact contains recorded evidence only. It does "
                      "not run "
                      "or display BlackBox "
                      "predictions. Fix an independent ballot before viewing "
                      "development prediction output."
                      "</div><p>Incident <code>"
                   << incident_key << "</code>; event marker is at 0 seconds. "
                   << (options.include_local_process_identities
                           ? "This local artifact includes process names and "
                             "PIDs; do "
                             "not copy it into the corpus."
                           : "Process identity is represented only by "
                             "incident-local "
                             "ordinal.")
                   << "</p><h2>System timeline</h2><label>Metric <select "
                      "id=\"metric\">"
                      "<option value=\"cpu\">CPU (%)</option><option "
                      "value=\"memory\">Memory (%)</option>"
                      "<option value=\"diskLatency\">Disk read latency "
                      "(ms)</option>"
                      "<option value=\"diskQueue\">Disk queue depth</option>"
                      "<option value=\"retransmit\">TCP retransmit (%)</option>"
                      "<option value=\"gpu\">GPU (%)</option><option "
                      "value=\"dpc\">DPC "
                      "(%)</option>"
                      "<option value=\"interrupt\">Interrupt "
                      "(%)</option></select></label>"
                      "<canvas id=\"system\" width=\"1200\" "
                      "height=\"320\"></canvas>"
                      "<h2>Process timeline</h2><label>Process <select "
                      "id=\"process\">";
            for (const auto& process : processes) {
                output << "<option value=\"" << process.ordinal << "\">" << html_text(process.label)
                       << "</option>";
            }
            output << "</select></label><canvas id=\"processChart\" "
                      "width=\"1200\" "
                      "height=\"320\"></canvas>"
                      "<h2>Process evidence "
                      "summary</h2><table><thead><tr><th>Ordinal</th><th>Local "
                      "label</th>"
                      "<th>Samples</th><th>Peak CPU</th><th>Peak working set "
                      "MiB</th><th>Peak disk MiB/s</th>"
                      "</tr></thead><tbody>";
            for (const auto& process : processes) {
                output << "<tr><td>" << process.ordinal << "</td><td>" << html_text(process.label)
                       << "</td><td>" << process.samples.size() << "</td><td>"
                       << process.peak_cpu_percent << "</td><td>" << process.peak_working_set_mib
                       << "</td><td>" << process.peak_disk_mib_per_second << "</td></tr>";
            }
            output << "</tbody></table><h2>Recorded system "
                      "events</h2><table><thead><tr><th>Time</th>"
                      "<th>Source</th><th>Kind</th><th>Level</th><th>Process</"
                      "th></"
                      "tr></thead><tbody>";
            for (const auto& event : events) {
                output << "<tr><td>" << relative_seconds(event.observed_at, event_time)
                       << "</td><td>" << source_name(event.source) << "</td><td>"
                       << kind_name(event.kind) << "</td><td>" << level_name(event.level)
                       << "</td><td>";
                if (event.has_process_identity) {
                    const auto found =
                        process_ordinals.find({event.process_pid, event.process_creation_token});
                    if (found != process_ordinals.end()) {
                        output << "Process ordinal " << found->second;
                    }
                }
                output << "</td></tr>";
            }
            output << "</tbody></table><p><small>Unavailable values create "
                      "gaps. Raw "
                      "normalized rows are "
                      "retained beside this page. Complete one copy of "
                      "ballot-template.tsv per independent "
                      "annotator; the session operator cannot be an "
                      "annotator.</small></p><script>const sys=[";
            for (std::size_t index = 0U; index < systems.size(); ++index) {
                if (index != 0U) output << ',';
                const auto& sample = systems[index];
                output << "{t:" << relative_seconds(sample.observed_at, event_time) << ",cpu:";
                write_json_value(output, sample.cpu_fraction, 100.0);
                output << ",memory:";
                write_json_value(output, sample.memory_fraction, 100.0);
                output << ",diskLatency:";
                write_json_value(output, sample.disk_read_latency_seconds, 1'000.0);
                output << ",diskQueue:";
                write_json_value(output, sample.disk_queue_depth);
                output << ",diskServiceConcurrency:";
                write_json_value(output, sample.disk_service_concurrency);
                output << ",schedulerDelay:";
                write_json_value(output, sample.scheduler_delay_seconds, 1'000.0);
                output << ",retransmit:";
                write_json_value(output, sample.network_tcp_retransmit_fraction, 100.0);
                output << ",gpu:";
                write_json_value(output, sample.gpu_fraction, 100.0);
                output << ",dpc:";
                write_json_value(output, sample.dpc_fraction, 100.0);
                output << ",interrupt:";
                write_json_value(output, sample.interrupt_fraction, 100.0);
                output << '}';
            }
            output << "];const proc={";
            for (std::size_t process_index = 0U; process_index < processes.size();
                 ++process_index) {
                if (process_index != 0U) output << ',';
                const auto& process = processes[process_index];
                output << json_text(std::to_string(process.ordinal)) << ":[";
                for (std::size_t index = 0U; index < process.samples.size(); ++index) {
                    if (index != 0U) output << ',';
                    const auto& sample = *process.samples[index];
                    output << "{t:" << relative_seconds(sample.observed_at, event_time) << ",cpu:";
                    write_json_value(output, sample.cpu_fraction, 100.0);
                    output << '}';
                }
                output << ']';
            }
            output << "};function draw(id,rows,key,label){const "
                      "c=document.getElementById(id),x=c.getContext('2d');"
                      "x.clearRect(0,0,c.width,c.height);x.fillStyle='#dbeafe';x."
                      "fillText(label,12,18);"
                      "if(!rows.length)return;const "
                      "ts=rows.map(r=>r.t),vals=rows.map(r=>r[key]).filter(v=>v!=="
                      "null);"
                      "if(!vals.length){x.fillText('No available "
                      "values',12,42);return;}const "
                      "lo=Math.min(...ts),hi=Math.max(...ts);"
                      "const "
                      "top=Math.max(...vals,0.000001),px=t=>40+(t-lo)/"
                      "Math.max(hi-lo,.001)*(c.width-60),"
                      "py=v=>c.height-30-v/"
                      "top*(c.height-60);x.strokeStyle='#38bdf8';x.lineWidth=2;x."
                      "beginPath();let open=false;"
                      "for(const r of rows){const "
                      "v=r[key];if(v===null){open=false;continue;}if(!open){x."
                      "moveTo(px("
                      "r.t),py(v));open=true;}"
                      "else "
                      "x.lineTo(px(r.t),py(v));}x.stroke();if(lo<=0&&hi>=0){x."
                      "strokeStyle='#fb7185';x.beginPath();"
                      "x.moveTo(px(0),25);x.lineTo(px(0),c.height-25);x.stroke();}"
                      "x."
                      "fillStyle='#94a3b8';"
                      "x.fillText(lo.toFixed(1)+' "
                      "s',35,c.height-8);x.fillText(hi.toFixed(1)+' "
                      "s',c.width-70,c.height-8);"
                      "x.fillText('max '+top.toFixed(2),c.width-110,18);}const "
                      "labels={cpu:'CPU (%)',memory:'Memory (%)',"
                      "diskLatency:'Disk read latency (ms)',diskQueue:'Disk queue "
                      "depth',retransmit:'TCP retransmit (%)',"
                      "gpu:'GPU (%)',dpc:'DPC (%)',interrupt:'Interrupt "
                      "(%)'};function "
                      "drawSystem(){const "
                      "k=document.getElementById('metric').value;"
                      "draw('system',sys,k,labels[k]);}function "
                      "drawProcess(){const "
                      "k=document.getElementById('process').value;"
                      "draw('processChart',proc[k]||[],'cpu','Process CPU "
                      "(%)');}document.getElementById('metric').onchange="
                      "drawSystem;"
                      "document.getElementById('process').onchange=drawProcess;"
                      "drawSystem();drawProcess();</script></body></html>";
            if (!stream_ok(output)) {
                return std::unexpected{review_error(TruthReviewErrorCode::io,
                                                    "cannot write interactive truth review")};
            }
        }

        static const std::set<std::filesystem::path> expected{
            "manifest.ini",      "system-samples.tsv",  "processes.tsv", "process-samples.tsv",
            "system-events.tsv", "ballot-template.tsv", "review.html"};
        std::set<std::filesystem::path> actual;
        for (std::filesystem::directory_iterator iterator{staging, issue}, end;
             !issue && iterator != end; iterator.increment(issue)) {
            const auto status = iterator->symlink_status(issue);
            if (issue || !std::filesystem::is_regular_file(status) ||
                iterator->file_size(issue) == 0U || issue) {
                return std::unexpected{
                    review_error(TruthReviewErrorCode::io, "truth-review staging is not exact")};
            }
            actual.emplace(iterator->path().filename());
        }
        if (issue || actual != expected) {
            return std::unexpected{review_error(TruthReviewErrorCode::io,
                                                "truth-review staging file set is not exact")};
        }
        std::filesystem::rename(staging, destination, issue);
        if (issue) {
            return std::unexpected{
                review_error(TruthReviewErrorCode::io, "cannot publish truth-review directory")};
        }
        return TruthReviewStatistics{systems.size(), samples.size(), processes.size(),
                                     events.size(), options.include_local_process_identities};
    } catch (const std::exception& exception) {
        return std::unexpected{review_error(TruthReviewErrorCode::io, exception.what())};
    } catch (...) {
        return std::unexpected{
            review_error(TruthReviewErrorCode::io, "unknown truth-review export failure")};
    }
}

} // namespace blackbox::evaluation
