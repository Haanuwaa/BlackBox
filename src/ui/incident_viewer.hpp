#pragma once

#include "core/incident.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace blackbox::ui {

inline constexpr std::size_t incident_list_page_size = 50U;
inline constexpr std::size_t incident_plot_point_capacity = 2'048U;
inline constexpr std::size_t incident_process_visible_capacity = 500U;
inline constexpr std::size_t incident_search_capacity = 128U;
inline constexpr std::size_t incident_label_capacity = 128U;
inline constexpr std::size_t incident_note_capacity = 4'096U;
inline constexpr std::size_t incident_analysis_process_capacity = 20U;
inline constexpr std::size_t recurring_group_override_capacity = 64U;

enum class IncidentViewerLoadState : std::uint8_t {
    disabled,
    idle,
    loading,
    ready,
    error,
};

enum class IncidentListOrder : std::uint8_t {
    newest_first,
    oldest_first,
    longest_first,
    shortest_first,
    label_ascending,
    label_descending,
};

enum class IncidentProcessSort : std::uint8_t {
    name,
    pid,
    peak_cpu,
    peak_working_set,
    peak_disk_read,
    peak_disk_write,
};

enum class IncidentAnalysisViewState : std::uint8_t {
    disabled,
    ready,
    cold_start,
    error,
};

enum class IncidentFeedback : std::uint8_t {
    unanswered,
    noticed_problem,
    did_not_notice_problem,
};

enum class IncidentCategory : std::uint8_t {
    unknown,
    system_freeze,
    game_stutter,
    application_slowdown_or_hang,
    network,
    audio,
};

struct IncidentAnomalyRow {
    std::string name{};
    double score{};
    std::string confidence{};
    std::string evidence{};
};

struct IncidentContributorRow {
    std::string name{};
    double score{};
    std::string assessment{};
    std::string timing{};
    std::string evidence{};
    std::string executable_key{};
    enum class Resource : std::uint8_t { cpu, memory, disk, network };
    enum class Attribution : std::uint8_t {
        unsure,
        confirmed_contributor,
        not_a_contributor,
    };
    enum class TemporalRelationship : std::uint8_t {
        preceding_activity,
        marker_spanning_ambiguous,
        post_marker_reaction,
    };
    Resource resource{Resource::cpu};
    Attribution attribution{Attribution::unsure};
    TemporalRelationship temporal_relationship{TemporalRelationship::preceding_activity};
    double score_before_feedback{};
    double feedback_multiplier{1.0};
    std::size_t feedback_matching_observations{};
    std::size_t feedback_confirmed_observations{};
    std::size_t feedback_rejected_observations{};
    std::string feedback_status{"No matching attribution history"};
};

struct IncidentContextProbabilityRow {
    std::string name{};
    double probability{};
};

struct IncidentContextView {
    bool enabled{};
    std::string primary{"Unknown"};
    double confidence{};
    double uncertainty{1.0};
    std::vector<IncidentContextProbabilityRow> probabilities{};
    std::vector<std::string> evidence{};
};

enum class RecurringIncidentViewState : std::uint8_t {
    disabled,
    loading,
    ready,
    error,
};

struct RecurringIncidentMemberRow {
    std::int64_t id{};
    std::string created_utc{};
    std::string label{};
    std::int64_t created_utc_milliseconds{};
    IncidentFeedback user_feedback{IncidentFeedback::unanswered};
    IncidentCategory category{IncidentCategory::unknown};
};

struct RecurringIncidentGroupRow {
    std::string name{};
    bool manually_overridden{};
    std::string shared_evidence{};
    double maximum_pair_distance{};
    std::size_t shared_characteristic_count{};
    double average_shared_support{};
    std::vector<RecurringIncidentMemberRow> members{};
};

struct IncidentDiagnosisView {
    bool available{};
    std::string incident_type{"Unknown"};
    std::string basis{"No aligned symptom evidence"};
    std::string confidence{"Unavailable"};
    double calibrated_confidence{};
    double evidence_coverage{};
    double correlated_evidence_penalty{};
    double confidence_before_feedback{};
    double feedback_multiplier{1.0};
    bool suppressed_by_feedback{};
    std::string primary_contributor{};
    std::vector<std::string> evidence{};
    std::uint32_t pipeline_version{};
    std::uint32_t evidence_model_version{};
    std::uint64_t configuration_fingerprint{};
    std::string inference{"Statistical only"};
};

struct FeedbackCalibrationView {
    bool applicable{};
    bool ready{};
    bool suppressing{};
    std::size_t observations_considered{};
    std::size_t matching_observations{};
    std::size_t confirmed_problem_observations{};
    std::size_t false_positive_observations{};
    double false_positive_fraction{};
    double confidence_multiplier{1.0};
    std::uint64_t profile_revision{};
    std::int64_t reset_after_utc_milliseconds{};
    bool rollback_available{};
    std::string status{"No automatic-trigger feedback profile applies"};
};

struct ConfirmedSimilarIncidentView {
    bool applicable{};
    bool ready{};
    bool manually_excluded{};
    std::string symptom{"Unknown"};
    std::size_t observations_considered{};
    std::size_t answered_observations{};
    std::size_t confirmed_problem_observations{};
    std::size_t false_positive_observations{};
    std::size_t categorized_confirmations{};
    std::size_t matching_confirmations{};
    double problem_fraction{};
    double category_consensus{};
    std::string status{"No confirmed similar-incident context applies"};
};

struct ObservedResourcePressureView {
    bool available{};
    std::string resource{"None"};
    std::string metric{"None"};
    double score{};
    double statistical_score{};
    std::string evidence{"No metric cleared the practical-effect floor"};
};

struct RecurringIncidentView {
    RecurringIncidentViewState state{RecurringIncidentViewState::disabled};
    std::string status{"Recurring discovery disabled"};
    std::int32_t feature_version{};
    std::size_t incidents_considered{};
    std::size_t cached_features{};
    std::size_t recomputed_features{};
    std::vector<RecurringIncidentGroupRow> groups{};
    std::vector<RecurringIncidentMemberRow> noise{};
    double elapsed_milliseconds{};
};

struct IncidentAnalysisView {
    IncidentAnalysisViewState state{IncidentAnalysisViewState::disabled};
    std::string status{"Analysis disabled"};
    double baseline_start_seconds{};
    double baseline_end_seconds{};
    double evaluation_start_seconds{};
    double evaluation_end_seconds{};
    std::size_t missing_values{};
    ObservedResourcePressureView pressure{};
    IncidentDiagnosisView diagnosis{};
    FeedbackCalibrationView feedback{};
    ConfirmedSimilarIncidentView similar_incidents{};
    IncidentContextView context{};
    std::vector<IncidentAnomalyRow> resources{};
    std::vector<IncidentAnomalyRow> processes{};
    std::vector<IncidentContributorRow> contributors{};
};

struct MetricAvailabilityCounts {
    std::array<std::size_t, 4U> by_status{};
    [[nodiscard]] std::size_t available() const noexcept { return by_status[0]; }
};

struct IncidentPlotSeries {
    std::vector<double> seconds_from_event{};
    std::vector<double> values{};
    MetricAvailabilityCounts availability{};
};

struct IncidentListRow {
    std::int64_t id{};
    std::int64_t created_utc_milliseconds{};
    std::string created_utc{};
    double duration_seconds{};
    std::string label{};
    std::string note_preview{};
    std::size_t system_sample_count{};
    std::size_t process_sample_count{};
};

struct IncidentProcessRow {
    core::IncidentProcessIdentity identity{};
    std::string name{};
    std::string executable_path{};
    std::size_t sample_count{};
    bool cpu_available{};
    bool working_set_available{};
    bool disk_read_available{};
    bool disk_write_available{};
    double peak_cpu_percent{};
    double peak_working_set_mib{};
    double peak_disk_read_mib_per_second{};
    double peak_disk_write_mib_per_second{};
};

struct ForegroundApplicationRow {
    double seconds_from_event{};
    core::IncidentProcessIdentity identity{};
    core::IncidentApplicationIdentity application_identity{};
    bool has_process_identity{};
    std::string name{};
    bool gpu_available{};
    double gpu_percent{};
};

struct SystemEventRow {
    double seconds_from_event{};
    std::string source{};
    std::string event{};
    std::string level{};
    std::uint32_t native_event_id{};
};

struct IncidentDetailView {
    std::int64_t id{};
    std::string created_utc{};
    std::string label{};
    std::string note{};
    double requested_start_seconds{};
    double requested_end_seconds{};
    double actual_start_seconds{};
    double actual_end_seconds{};
    std::uint32_t trigger_count{};
    std::uint32_t manual_trigger_count{};
    std::uint32_t automatic_trigger_count{};
    core::AutomaticIncidentResource automatic_resource{core::AutomaticIncidentResource::none};
    double automatic_observed_value{};
    double automatic_baseline_value{};
    double automatic_score{};
    core::AutomaticIncidentSignal automatic_signal{
        core::AutomaticIncidentSignal::throughput_or_utilization};
    IncidentFeedback user_feedback{IncidentFeedback::unanswered};
    IncidentCategory category{IncidentCategory::unknown};
    std::string recurring_group_override{};
    std::size_t system_sample_count{};
    std::size_t process_sample_count{};
    IncidentAnalysisView analysis{};
    IncidentPlotSeries cpu_percent{};
    IncidentPlotSeries memory_percent{};
    IncidentPlotSeries disk_read_mib_per_second{};
    IncidentPlotSeries disk_write_mib_per_second{};
    IncidentPlotSeries network_receive_mib_per_second{};
    IncidentPlotSeries network_transmit_mib_per_second{};
    IncidentPlotSeries disk_read_latency_milliseconds{};
    IncidentPlotSeries disk_write_latency_milliseconds{};
    IncidentPlotSeries disk_service_time_milliseconds{};
    IncidentPlotSeries disk_queue_depth{};
    IncidentPlotSeries network_connectivity_level{};
    IncidentPlotSeries network_interface_changes{};
    IncidentPlotSeries network_tcp_retransmit_percent{};
    IncidentPlotSeries network_tcp_failed_connections{};
    IncidentPlotSeries network_tcp_resets{};
    IncidentPlotSeries gpu_percent{};
    IncidentPlotSeries gpu_dedicated_memory_mib{};
    IncidentPlotSeries gpu_shared_memory_mib{};
    IncidentPlotSeries foreground_gpu_percent{};
    IncidentPlotSeries dpc_percent{};
    IncidentPlotSeries interrupt_percent{};
    IncidentPlotSeries dpc_rate{};
    IncidentPlotSeries cpu_current_mhz{};
    IncidentPlotSeries cpu_thermal_limit_mhz{};
    IncidentPlotSeries cpu_thermal_limit_percent{};
    IncidentPlotSeries battery_percent{};
    IncidentPlotSeries cpu_some_pressure_percent{};
    IncidentPlotSeries memory_some_pressure_percent{};
    IncidentPlotSeries memory_full_pressure_percent{};
    IncidentPlotSeries io_some_pressure_percent{};
    IncidentPlotSeries io_full_pressure_percent{};
    IncidentPlotSeries thermal_pressure_state{};
    IncidentPlotSeries memory_pressure_state{};
    std::vector<ForegroundApplicationRow> foreground_applications{};
    std::vector<SystemEventRow> system_events{};
    std::vector<IncidentProcessRow> processes{};
    std::optional<core::IncidentProcessIdentity> selected_process{};
    IncidentPlotSeries selected_process_cpu_percent{};
    IncidentPlotSeries selected_process_working_set_mib{};
    IncidentPlotSeries selected_process_disk_read_mib_per_second{};
    IncidentPlotSeries selected_process_disk_write_mib_per_second{};
};

struct IncidentViewerContent {
    IncidentViewerLoadState state{IncidentViewerLoadState::idle};
    std::string status{};
    std::vector<IncidentListRow> incidents{};
    std::uint64_t total_matching{};
    std::size_t offset{};
    std::optional<IncidentDetailView> detail{};
    RecurringIncidentView recurring{};
    std::uint64_t generation{};
    double last_query_milliseconds{};
    double last_build_milliseconds{};
    double last_analysis_milliseconds{};
};

struct IncidentViewerState {
    std::shared_ptr<const IncidentViewerContent> content{
        std::make_shared<const IncidentViewerContent>()};
    std::array<char, incident_search_capacity + 1U> search{};
    IncidentListOrder order{IncidentListOrder::newest_first};
    std::array<char, incident_label_capacity + 1U> label_editor{};
    std::array<char, incident_note_capacity + 1U> note_editor{};
    std::array<char, recurring_group_override_capacity + 1U> recurring_group_override_editor{};
    IncidentCategory category_editor{IncidentCategory::unknown};
    std::array<char, incident_search_capacity + 1U> process_filter{};
    IncidentProcessSort process_sort{IncidentProcessSort::peak_cpu};
    bool process_sort_ascending{};
    std::vector<std::size_t> visible_process_indices{};
    std::uint64_t synchronized_generation{};
    std::int64_t editor_incident_id{};
};

[[nodiscard]] std::string format_utc_milliseconds(std::int64_t milliseconds);
[[nodiscard]] IncidentDetailView
build_incident_detail(std::int64_t id, std::int64_t created_utc_milliseconds, std::string label,
                      std::string note, const core::IncidentSnapshot& incident,
                      std::optional<core::IncidentProcessIdentity> selected_process = std::nullopt,
                      std::size_t maximum_plot_points = incident_plot_point_capacity);
[[nodiscard]] std::vector<std::size_t>
filter_and_sort_processes(const std::vector<IncidentProcessRow>& rows, const std::string& filter,
                          IncidentProcessSort sort, bool ascending,
                          std::size_t maximum_results = incident_process_visible_capacity);
void synchronize_incident_editor(IncidentViewerState& state);

} // namespace blackbox::ui
