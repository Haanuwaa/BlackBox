#include "storage/incident_dataset.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string_view>
#include <system_error>

namespace blackbox::storage {
namespace {

[[nodiscard]] StorageError dataset_error(std::string message) {
    return StorageError{StorageErrorCode::io, 0, std::move(message)};
}

[[nodiscard]] std::string export_key_text(const IncidentExportKey& key) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(key.bytes.size() * 2U, '0');
    for (std::size_t index = 0; index < key.bytes.size(); ++index) {
        result[index * 2U] = digits[key.bytes[index] >> 4U];
        result[index * 2U + 1U] = digits[key.bytes[index] & 0x0FU];
    }
    return result;
}

[[nodiscard]] std::expected<IncidentExportKey, StorageError>
parse_export_key(const std::string_view text) {
    IncidentExportKey key{};
    if (text.size() != key.bytes.size() * 2U) {
        return std::unexpected{dataset_error("invalid dataset incident key")};
    }
    const auto nibble = [](const char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < key.bytes.size(); ++index) {
        const auto high = nibble(text[index * 2U]);
        const auto low = nibble(text[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return std::unexpected{dataset_error("invalid dataset incident key")};
        }
        key.bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return key;
}

[[nodiscard]] constexpr std::string_view category_name(const IncidentCategory value) {
    switch (value) {
    case IncidentCategory::unknown: return "unknown";
    case IncidentCategory::system_freeze: return "system_freeze";
    case IncidentCategory::game_stutter: return "game_stutter";
    case IncidentCategory::application_slowdown_or_hang:
        return "application_slowdown_or_hang";
    case IncidentCategory::network: return "network";
    case IncidentCategory::audio: return "audio";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view feedback_name(
    const IncidentUserFeedback value) {
    switch (value) {
    case IncidentUserFeedback::unanswered: return "unanswered";
    case IncidentUserFeedback::noticed_problem: return "noticed_problem";
    case IncidentUserFeedback::did_not_notice_problem: return "did_not_notice_problem";
    }
    return "unanswered";
}

[[nodiscard]] constexpr std::string_view origin_name(
    const ClassificationChangeOrigin value) {
    switch (value) {
    case ClassificationChangeOrigin::capture: return "capture";
    case ClassificationChangeOrigin::user: return "user";
    case ClassificationChangeOrigin::dataset_import: return "dataset_import";
    }
    return "user";
}

[[nodiscard]] std::optional<IncidentCategory> parse_category(
    const std::string_view value) {
    for (auto raw = 0; raw <= static_cast<int>(IncidentCategory::audio); ++raw) {
        const auto category = static_cast<IncidentCategory>(raw);
        if (value == category_name(category)) return category;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<IncidentUserFeedback> parse_feedback(
    const std::string_view value) {
    for (auto raw = 0;
         raw <= static_cast<int>(IncidentUserFeedback::did_not_notice_problem); ++raw) {
        const auto feedback = static_cast<IncidentUserFeedback>(raw);
        if (value == feedback_name(feedback)) return feedback;
    }
    return std::nullopt;
}

template <typename Value>
void write_recorded(std::ostream& output, const core::RecordedValue<Value>& value) {
    output << static_cast<int>(value.status) << '\t';
    if (value.status == core::RecordedValueStatus::available) output << value.value;
}

void write_recorded_byte(std::ostream& output,
                         const core::RecordedValue<std::uint8_t>& value) {
    output << static_cast<int>(value.status) << '\t';
    if (value.status == core::RecordedValueStatus::available) {
        output << static_cast<unsigned int>(value.value);
    }
}

[[nodiscard]] std::vector<std::string_view> split_tsv(const std::string& line) {
    std::vector<std::string_view> fields;
    std::size_t begin = 0;
    while (true) {
        const auto end = line.find('\t', begin);
        fields.emplace_back(line.data() + begin,
                            (end == std::string::npos ? line.size() : end) - begin);
        if (end == std::string::npos) break;
        begin = end + 1U;
    }
    return fields;
}

[[nodiscard]] std::int64_t relative_nanoseconds(
    const core::MonotonicTimePoint value, const core::MonotonicTimePoint event) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value - event).count();
}

} // namespace

std::expected<IncidentDatasetStatistics, StorageError> export_incident_dataset(
    SqliteIncidentArchive& archive, const std::filesystem::path& destination) noexcept {
    struct IncompleteDatasetCleanup {
        const std::filesystem::path& path;
        bool active{};
        ~IncompleteDatasetCleanup() {
            if (!active) return;
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } cleanup{destination};
    try {
        std::error_code error;
        if (std::filesystem::exists(destination, error) || error) {
            return std::unexpected{dataset_error(
                "dataset destination must not already exist")};
        }
        if (!std::filesystem::create_directories(destination, error) || error) {
            return std::unexpected{dataset_error("cannot create dataset destination")};
        }
        cleanup.active = true;
        std::ofstream manifest{destination / "manifest.json", std::ios::binary};
        std::ofstream incidents{destination / "incidents.tsv", std::ios::binary};
        std::ofstream systems{destination / "system_samples.tsv", std::ios::binary};
        std::ofstream processes{destination / "process_samples.tsv", std::ios::binary};
        std::ofstream system_events{destination / "system_events.tsv", std::ios::binary};
        std::ofstream history{destination / "classification_history.tsv", std::ios::binary};
        manifest.flush();
        incidents.flush();
        systems.flush();
        processes.flush();
        system_events.flush();
        history.flush();
        if (!manifest || !incidents || !systems || !processes || !system_events || !history) {
            return std::unexpected{dataset_error("cannot create dataset files")};
        }
        manifest << R"json({
  "format":"blackbox-offline-dataset",
  "version":1,
  "application_version":")json" << BLACKBOX_VERSION << R"json(",
  "time":{"created_utc_ms":"milliseconds since Unix epoch","offset_ns":"nanoseconds relative to incident event"},
  "recorded_value_status":{"0":"available","1":"warming_up","2":"unsupported","3":"temporarily_unavailable"},
  "units":{"cpu_fraction":"ratio 0..1","gpu_fraction":"ratio 0..1","memory_bytes":"bytes","memory_fraction":"ratio 0..1","io_rate":"bytes/second","disk_latency":"seconds","disk_queue_depth":"requests","frequency":"MHz","battery_fraction":"ratio 0..1","uptime":"seconds","tcp_retransmit_fraction":"ratio 0..1","event_count":"count per sample interval"},
  "privacy":{"pseudonymous_incident_keys":true,"normalized_system_events":true,"excluded":["archive paths","executable paths","process names","process identifiers","foreground process identity","creation tokens","Event Log messages","device identifiers","audio endpoint identifiers","window titles","free-form labels","free-form notes"]}
}
)json";
        incidents << "incident_key\tcreated_utc_ms\tcategory\tuser_feedback\t"
                     "manual_trigger_count\tautomatic_trigger_count\tautomatic_resource\t"
                     "automatic_signal\tautomatic_score\tsystem_sample_count\tprocess_sample_count\t"
                     "system_event_count\n";
        systems << "incident_key\tsample_index\toffset_ns\tcpu_status\tcpu_fraction\t"
                   "memory_used_status\tmemory_used_bytes\tmemory_total_status\t"
                   "memory_total_bytes\tmemory_fraction_status\tmemory_fraction\t"
                   "disk_read_status\tdisk_read_bps\tdisk_write_status\tdisk_write_bps\t"
                   "network_receive_status\tnetwork_receive_bps\tnetwork_transmit_status\t"
                   "network_transmit_bps\tdisk_read_latency_status\tdisk_read_latency_seconds\t"
                   "disk_write_latency_status\tdisk_write_latency_seconds\t"
                   "disk_service_time_status\tdisk_service_time_seconds\t"
                   "disk_queue_depth_status\tdisk_queue_depth\tdisk_device_status\tdisk_device_id\t"
                   "network_connectivity_status\tnetwork_connectivity_level\t"
                   "network_interfaces_status\tnetwork_active_interfaces\t"
                   "network_changes_status\tnetwork_interface_changes\t"
                   "tcp_retransmit_status\ttcp_retransmit_fraction\t"
                   "tcp_failures_status\ttcp_failed_connections\t"
                   "tcp_resets_status\ttcp_resets\t"
                   "gpu_status\tgpu_fraction\tgpu_dedicated_status\tgpu_dedicated_bytes\t"
                   "gpu_shared_status\tgpu_shared_bytes\tforeground_gpu_status\tforeground_gpu_fraction\t"
                   "dpc_status\tdpc_fraction\tinterrupt_status\tinterrupt_fraction\t"
                   "dpc_rate_status\tdpc_rate\tcpu_current_status\tcpu_current_mhz\t"
                   "cpu_max_status\tcpu_max_mhz\tcpu_thermal_limit_status\tcpu_thermal_limit_mhz\t"
                   "cpu_thermal_fraction_status\tcpu_thermal_limit_fraction\t"
                   "power_source_status\tpower_source\tbattery_status\tbattery_fraction\t"
                   "battery_saver_status\tbattery_saver\tuptime_status\tuptime_seconds\n";
        processes << "incident_key\tsample_index\toffset_ns\tprocess_ordinal\t"
                     "cpu_status\tcpu_fraction\tworking_set_status\tworking_set_bytes\t"
                     "disk_read_status\tdisk_read_bps\tdisk_write_status\tdisk_write_bps\n";
        system_events << "incident_key\tevent_index\toffset_ns\thas_source_utc_time\t"
                         "source_utc_ms\tsource\tkind\tlevel\tnative_event_id\tdetail\t"
                         "process_ordinal\n";
        history << "incident_key\tchanged_utc_ms\tcategory\tuser_feedback\torigin\n";
        incidents << std::setprecision(17);
        systems << std::setprecision(17);
        processes << std::setprecision(17);
        system_events << std::setprecision(17);

        IncidentDatasetStatistics statistics{};
        std::size_t offset = 0U;
        while (true) {
            auto page = archive.list_page(IncidentListQuery{
                .offset = offset, .limit = maximum_incident_page_size,
                .sort = IncidentListSort::oldest_first});
            if (!page) return std::unexpected{page.error()};
            for (const auto& summary : page->incidents) {
                auto snapshot = archive.load(summary.id);
                if (!snapshot) return std::unexpected{snapshot.error()};
                auto annotation = archive.annotation(summary.id);
                if (!annotation) return std::unexpected{annotation.error()};
                auto events = archive.classification_history(summary.id);
                if (!events) return std::unexpected{events.error()};
                const auto key = export_key_text(summary.export_key);
                const auto& header = (*snapshot)->header();
                incidents << key << '\t' << summary.created_utc_milliseconds << '\t'
                          << category_name(annotation->category) << '\t'
                          << feedback_name(annotation->user_feedback) << '\t'
                          << header.window.manual_trigger_count << '\t'
                          << header.window.automatic_trigger_count << '\t'
                          << static_cast<int>(header.window.automatic_resource) << '\t'
                          << static_cast<int>(header.window.automatic_signal) << '\t'
                          << header.window.automatic_score << '\t'
                          << (*snapshot)->system_samples().size() << '\t'
                          << (*snapshot)->process_samples().size() << '\t'
                          << (*snapshot)->system_events().size() << '\n';
                std::size_t index = 0U;
                for (const auto& sample : (*snapshot)->system_samples()) {
                    systems << key << '\t' << index++ << '\t'
                            << relative_nanoseconds(sample.observed_at,
                                                    header.window.event_time) << '\t';
                    write_recorded(systems, sample.cpu_fraction); systems << '\t';
                    write_recorded(systems, sample.memory_used_bytes); systems << '\t';
                    write_recorded(systems, sample.memory_total_bytes); systems << '\t';
                    write_recorded(systems, sample.memory_fraction); systems << '\t';
                    write_recorded(systems, sample.disk_read_bytes_per_second); systems << '\t';
                    write_recorded(systems, sample.disk_write_bytes_per_second); systems << '\t';
                    write_recorded(systems, sample.network_receive_bytes_per_second); systems << '\t';
                    write_recorded(systems, sample.network_transmit_bytes_per_second);
                    systems << '\t'; write_recorded(systems, sample.disk_read_latency_seconds);
                    systems << '\t'; write_recorded(systems, sample.disk_write_latency_seconds);
                    systems << '\t'; write_recorded(systems, sample.disk_service_time_seconds);
                    systems << '\t'; write_recorded(systems, sample.disk_queue_depth);
                    systems << '\t'; write_recorded(systems, sample.disk_worst_device_id);
                    systems << '\t'; write_recorded_byte(
                        systems, sample.network_connectivity_level);
                    systems << '\t'; write_recorded(
                        systems, sample.network_active_interfaces);
                    systems << '\t'; write_recorded(
                        systems, sample.network_interface_changes);
                    systems << '\t'; write_recorded(
                        systems, sample.network_tcp_retransmit_fraction);
                    systems << '\t'; write_recorded(
                        systems, sample.network_tcp_failed_connections);
                    systems << '\t'; write_recorded(systems, sample.network_tcp_resets);
                    systems << '\t'; write_recorded(systems, sample.gpu_fraction);
                    systems << '\t'; write_recorded(systems, sample.gpu_dedicated_memory_bytes);
                    systems << '\t'; write_recorded(systems, sample.gpu_shared_memory_bytes);
                    systems << '\t'; write_recorded(systems, sample.foreground_gpu_fraction);
                    systems << '\t'; write_recorded(systems, sample.dpc_fraction);
                    systems << '\t'; write_recorded(systems, sample.interrupt_fraction);
                    systems << '\t'; write_recorded(systems, sample.dpc_rate);
                    systems << '\t'; write_recorded(systems, sample.cpu_current_mhz);
                    systems << '\t'; write_recorded(systems, sample.cpu_max_mhz);
                    systems << '\t'; write_recorded(systems, sample.cpu_thermal_limit_mhz);
                    systems << '\t'; write_recorded(systems, sample.cpu_thermal_limit_fraction);
                    systems << '\t'; write_recorded_byte(systems, sample.power_source);
                    systems << '\t'; write_recorded(systems, sample.battery_fraction);
                    systems << '\t'; write_recorded(systems, sample.battery_saver);
                    systems << '\t'; write_recorded(systems, sample.system_uptime_seconds);
                    systems << '\n';
                    ++statistics.system_samples;
                }
                std::map<core::IncidentProcessIdentity, std::size_t> ordinal_by_identity;
                for (const auto& sample : (*snapshot)->process_samples()) {
                    ordinal_by_identity.emplace(sample.identity,
                                                ordinal_by_identity.size());
                }
                for (const auto& event : (*snapshot)->system_events()) {
                    if (event.has_process_identity) {
                        ordinal_by_identity.emplace(
                            core::IncidentProcessIdentity{
                                event.process_pid, event.process_creation_token},
                            ordinal_by_identity.size());
                    }
                }
                index = 0U;
                for (const auto& event : (*snapshot)->system_events()) {
                    system_events << key << '\t' << index++ << '\t'
                                  << relative_nanoseconds(event.observed_at,
                                                          header.window.event_time) << '\t'
                                  << static_cast<int>(event.has_source_utc_time) << '\t'
                                  << event.source_utc_milliseconds << '\t'
                                  << static_cast<int>(event.source) << '\t'
                                  << static_cast<int>(event.kind) << '\t'
                                  << static_cast<int>(event.level) << '\t'
                                  << event.native_event_id << '\t' << event.detail << '\t';
                    if (event.has_process_identity) {
                        const auto found = ordinal_by_identity.find(
                            {event.process_pid, event.process_creation_token});
                        if (found != ordinal_by_identity.end()) {
                            system_events << found->second;
                        }
                    }
                    system_events << '\n';
                    ++statistics.system_events;
                }
                index = 0U;
                for (const auto& sample : (*snapshot)->process_samples()) {
                    const auto [found, inserted] = ordinal_by_identity.emplace(
                        sample.identity, ordinal_by_identity.size());
                    static_cast<void>(inserted);
                    processes << key << '\t' << index++ << '\t'
                              << relative_nanoseconds(sample.observed_at,
                                                      header.window.event_time) << '\t'
                              << found->second << '\t';
                    write_recorded(processes, sample.cpu_fraction); processes << '\t';
                    write_recorded(processes, sample.working_set_bytes); processes << '\t';
                    write_recorded(processes, sample.disk_read_bytes_per_second);
                    processes << '\t';
                    write_recorded(processes, sample.disk_write_bytes_per_second);
                    processes << '\n';
                    ++statistics.process_samples;
                }
                for (const auto& event : *events) {
                    history << key << '\t' << event.changed_utc_milliseconds << '\t'
                            << category_name(event.category) << '\t'
                            << feedback_name(event.user_feedback) << '\t'
                            << origin_name(event.origin) << '\n';
                    ++statistics.classification_events;
                }
                ++statistics.incidents;
            }
            offset += page->incidents.size();
            if (offset >= page->total_matching || page->incidents.empty()) break;
        }
        if (!manifest || !incidents || !systems || !processes || !system_events || !history) {
            return std::unexpected{dataset_error("dataset write failed")};
        }
        cleanup.active = false;
        return statistics;
    } catch (const std::exception& exception) {
        return std::unexpected{dataset_error(exception.what())};
    } catch (...) {
        return std::unexpected{dataset_error("unknown dataset export failure")};
    }
}

std::expected<IncidentDatasetStatistics, StorageError>
import_incident_dataset_classifications(SqliteIncidentArchive& archive,
                                        const std::filesystem::path& source) noexcept {
    try {
        std::ifstream manifest{source / "manifest.json", std::ios::binary};
        std::ifstream incidents{source / "incidents.tsv", std::ios::binary};
        if (!manifest || !incidents) {
            return std::unexpected{dataset_error("dataset manifest or incidents file missing")};
        }
        const std::string manifest_text{std::istreambuf_iterator<char>{manifest}, {}};
        if (manifest_text.find("\"format\":\"blackbox-offline-dataset\"") ==
                std::string::npos ||
            manifest_text.find("\"version\":1") == std::string::npos) {
            return std::unexpected{dataset_error("unsupported incident dataset version")};
        }
        std::string line;
        if (!std::getline(incidents, line)) {
            return std::unexpected{dataset_error("invalid incident dataset header")};
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto version_one_header =
            "incident_key\tcreated_utc_ms\tcategory\tuser_feedback\t"
                    "manual_trigger_count\tautomatic_trigger_count\tautomatic_resource\t"
                    "automatic_signal\tautomatic_score\tsystem_sample_count\tprocess_sample_count\t"
                    "system_event_count";
        if (line != version_one_header) {
            return std::unexpected{dataset_error("invalid incident dataset header")};
        }
        IncidentDatasetStatistics statistics{};
        while (std::getline(incidents, line)) {
            if (line.empty()) continue;
            if (line.back() == '\r') line.pop_back();
            const auto fields = split_tsv(line);
            if (fields.size() != 12U) {
                return std::unexpected{dataset_error("invalid incident dataset row")};
            }
            auto key = parse_export_key(fields[0]);
            const auto category = parse_category(fields[2]);
            const auto feedback = parse_feedback(fields[3]);
            if (!key || !category || !feedback) {
                return std::unexpected{dataset_error("invalid incident classification row")};
            }
            auto incident_id = archive.incident_id_for_export_key(*key);
            if (!incident_id) return std::unexpected{incident_id.error()};
            if (!*incident_id) continue;
            auto annotation = archive.annotation(**incident_id);
            if (!annotation) return std::unexpected{annotation.error()};
            ++statistics.incidents;
            if (annotation->category == *category &&
                annotation->user_feedback == *feedback) continue;
            annotation->category = *category;
            annotation->user_feedback = *feedback;
            auto updated = archive.update_annotation_with_origin(
                **incident_id, *annotation, ClassificationChangeOrigin::dataset_import);
            if (!updated) return std::unexpected{updated.error()};
            ++statistics.classifications_updated;
        }
        return statistics;
    } catch (const std::exception& exception) {
        return std::unexpected{dataset_error(exception.what())};
    } catch (...) {
        return std::unexpected{dataset_error("unknown dataset import failure")};
    }
}

} // namespace blackbox::storage
