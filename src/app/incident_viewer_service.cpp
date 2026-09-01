#include "app/incident_viewer_service.hpp"
#include "app/incident_viewer_analysis.hpp"

#if BLACKBOX_ANALYSIS_ENABLED
#include "analysis/incident_analyzer.hpp"
#include "analysis/incident_clustering.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <utility>

namespace blackbox::app {
namespace {

[[nodiscard]] storage::IncidentListSort storage_order(const ui::IncidentListOrder order) noexcept {
    switch (order) {
    case ui::IncidentListOrder::newest_first:
        return storage::IncidentListSort::newest_first;
    case ui::IncidentListOrder::oldest_first:
        return storage::IncidentListSort::oldest_first;
    case ui::IncidentListOrder::longest_first:
        return storage::IncidentListSort::longest_first;
    case ui::IncidentListOrder::shortest_first:
        return storage::IncidentListSort::shortest_first;
    case ui::IncidentListOrder::label_ascending:
        return storage::IncidentListSort::label_ascending;
    case ui::IncidentListOrder::label_descending:
        return storage::IncidentListSort::label_descending;
    }
    return storage::IncidentListSort::newest_first;
}

[[nodiscard]] double milliseconds(const std::chrono::steady_clock::duration value) noexcept {
    return std::chrono::duration<double, std::milli>{value}.count();
}

[[nodiscard]] std::string note_preview(const std::string& note) {
    constexpr std::size_t maximum = 96U;
    if (note.size() <= maximum) return note;
    return note.substr(0U, maximum - 3U) + "...";
}



} // namespace

storage::IncidentListQuery IncidentViewerService::storage_query(const Job& job) const {
    storage::IncidentListQuery query{};
    query.offset = job.offset;
    query.limit = ui::incident_list_page_size;
    query.search = job.search;
    query.sort = storage_order(job.order);
    return query;
}

void IncidentViewerService::handle_page(const Job& job) {
    const auto query = storage_query(job);
    const auto started = std::chrono::steady_clock::now();
    auto page = repository_.list_page(query);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (!page) {
        publish_error(page.error().message);
        return;
    }
    last_page_ = *page;
    last_query_ = query;
    auto content = *snapshot();
    content.state = ui::IncidentViewerLoadState::ready;
    content.status = std::to_string(page->total_matching) + " matching incidents";
    content.incidents.clear();
    content.incidents.reserve(page->incidents.size());
    for (const auto& incident : page->incidents) {
        ui::IncidentListRow row{};
        row.id = incident.id;
        row.created_utc_milliseconds = incident.created_utc_milliseconds;
        row.created_utc = ui::format_utc_milliseconds(incident.created_utc_milliseconds);
        row.duration_seconds = static_cast<double>(incident.actual_end_nanoseconds -
                                                   incident.actual_start_nanoseconds) /
                               1'000'000'000.0;
        row.label = incident.label;
        row.note_preview = note_preview(incident.note);
        row.system_sample_count = incident.system_sample_count;
        row.process_sample_count = incident.process_sample_count;
        content.incidents.push_back(std::move(row));
    }
    content.total_matching = page->total_matching;
    content.offset = page->offset;
    content.last_query_milliseconds = milliseconds(elapsed);
    publish(std::move(content));
}

void IncidentViewerService::handle_detail(const Job& job) {
    const auto query_started = std::chrono::steady_clock::now();
    auto incident = repository_.load(job.incident_id);
    if (!incident) {
        publish_error(incident.error().message);
        return;
    }
    auto annotation = repository_.annotation(job.incident_id);
    const auto query_elapsed = std::chrono::steady_clock::now() - query_started;
    if (!annotation) {
        publish_error(annotation.error().message);
        return;
    }
    loaded_incident_ = *incident;
    loaded_annotation_ = *annotation;
    loaded_incident_id_ = job.incident_id;
    loaded_created_utc_milliseconds_ = 0;
    const auto summary = std::find_if(
        last_page_.incidents.begin(), last_page_.incidents.end(),
        [&](const storage::StoredIncidentSummary& value) { return value.id == job.incident_id; });
    if (summary != last_page_.incidents.end()) {
        loaded_created_utc_milliseconds_ = summary->created_utc_milliseconds;
    } else if (const auto recurring = recurring_created_utc_by_id_.find(job.incident_id);
               recurring != recurring_created_utc_by_id_.end()) {
        loaded_created_utc_milliseconds_ = recurring->second;
    }
    std::string recurring_override;
    if (recurring_repository_ != nullptr) {
        if (auto loaded_override = recurring_repository_->recurring_group_override(job.incident_id);
            loaded_override) {
            recurring_override = std::move(*loaded_override);
        }
    }
#if BLACKBOX_ANALYSIS_ENABLED
    const auto analysis_started = std::chrono::steady_clock::now();
    if (analyzer_ == nullptr) {
        loaded_analysis_ = ui::IncidentAnalysisView{};
    } else {
        loaded_analysis_ = detail::analyze_incident(
            analyzer_, profile_repository_, feedback_repository_, job.incident_id, **incident,
            snapshot()->recurring);
    }
    const auto analysis_elapsed = std::chrono::steady_clock::now() - analysis_started;
#else
    loaded_analysis_ = ui::IncidentAnalysisView{};
#endif
    const auto build_started = std::chrono::steady_clock::now();
    auto detail = ui::build_incident_detail(job.incident_id, loaded_created_utc_milliseconds_,
                                            annotation->label, annotation->note, **incident);
    detail.user_feedback = static_cast<ui::IncidentFeedback>(annotation->user_feedback);
    detail.category = static_cast<ui::IncidentCategory>(annotation->category);
    detail.recurring_group_override = std::move(recurring_override);
    detail.analysis = loaded_analysis_;
    const auto build_elapsed = std::chrono::steady_clock::now() - build_started;
    auto content = *snapshot();
    content.state = ui::IncidentViewerLoadState::ready;
    content.status = "Incident loaded";
    content.detail = std::move(detail);
    content.last_query_milliseconds = milliseconds(query_elapsed);
    content.last_build_milliseconds = milliseconds(build_elapsed);
#if BLACKBOX_ANALYSIS_ENABLED
    content.last_analysis_milliseconds = milliseconds(analysis_elapsed);
#else
    content.last_analysis_milliseconds = 0.0;
#endif
    publish(std::move(content));
}

void IncidentViewerService::handle_process(const Job& job) {
    if (loaded_incident_id_ != job.incident_id || !loaded_incident_) {
        handle_detail(Job{.type = JobType::detail, .incident_id = job.incident_id});
        if (loaded_incident_id_ != job.incident_id || !loaded_incident_) return;
    }
    const auto build_started = std::chrono::steady_clock::now();
    auto detail = ui::build_incident_detail(job.incident_id, loaded_created_utc_milliseconds_,
                                            loaded_annotation_.label, loaded_annotation_.note,
                                            *loaded_incident_, job.identity);
    detail.user_feedback = static_cast<ui::IncidentFeedback>(loaded_annotation_.user_feedback);
    detail.category = static_cast<ui::IncidentCategory>(loaded_annotation_.category);
    const auto previous = snapshot();
    if (previous->detail && previous->detail->id == job.incident_id) {
        detail.recurring_group_override = previous->detail->recurring_group_override;
    }
    detail.analysis = loaded_analysis_;
    auto content = *snapshot();
    content.detail = std::move(detail);
    content.status = "Process timeline loaded";
    content.last_build_milliseconds =
        milliseconds(std::chrono::steady_clock::now() - build_started);
    publish(std::move(content));
}

void IncidentViewerService::handle_recurring() {
    const auto started = std::chrono::steady_clock::now();
    auto content = *snapshot();
#if BLACKBOX_ANALYSIS_ENABLED
    if (recurring_repository_ == nullptr) {
        content.recurring.state = ui::RecurringIncidentViewState::disabled;
        content.recurring.status = "Recurring storage unavailable";
        publish(std::move(content));
        return;
    }
    const auto records =
        recurring_repository_->recurring_incidents(storage::maximum_recurring_incidents);
    if (!records) {
        content.recurring.state = ui::RecurringIncidentViewState::error;
        content.recurring.status = records.error().message;
        content.recurring.elapsed_milliseconds =
            milliseconds(std::chrono::steady_clock::now() - started);
        publish(std::move(content));
        return;
    }
    std::vector<analysis::IncidentClusterInput> inputs;
    std::vector<storage::StoredIncidentFeatureCache> updates;
    inputs.reserve(records->size());
    updates.reserve(records->size());
    recurring_created_utc_by_id_.clear();
    std::size_t cached_count{};
    std::size_t load_failures{};
    for (const auto& record : *records) {
        recurring_created_utc_by_id_.emplace(record.id, record.created_utc_milliseconds);
        analysis::IncidentFeatureVector feature{};
        auto cache_valid =
            record.cached_feature.has_value() &&
            record.cached_feature->feature_version == analysis::incident_feature_version &&
            record.cached_feature->values.size() == analysis::incident_feature_dimension_count &&
            record.cached_feature->available.size() == analysis::incident_feature_dimension_count;
        if (cache_valid) {
            feature.incident_id = record.id;
            feature.created_utc_milliseconds = record.created_utc_milliseconds;
            for (std::size_t index = 0U; index < analysis::incident_feature_dimension_count;
                 ++index) {
                feature.values[index] = record.cached_feature->values[index];
                feature.available[index] = record.cached_feature->available[index] != 0U;
            }
            ++cached_count;
        } else {
            auto incident = repository_.load(record.id);
            if (!incident) {
                ++load_failures;
                continue;
            }
            feature = analysis::extract_incident_features(
                record.id, record.created_utc_milliseconds, **incident);
            storage::StoredIncidentFeatureCache update{};
            update.incident_id = record.id;
            update.feature_version = feature.version;
            update.values.assign(feature.values.begin(), feature.values.end());
            update.available.reserve(feature.available.size());
            for (const auto available : feature.available)
                update.available.push_back(available ? 1U : 0U);
            updates.push_back(std::move(update));
        }
        inputs.push_back({feature, record.override_group});
    }
    std::string cache_warning;
    if (!updates.empty()) {
        if (const auto stored = recurring_repository_->store_incident_features(updates); !stored) {
            cache_warning = "; cache update unavailable: " + stored.error().message;
        }
    }
    const auto clustered = analysis::cluster_incidents(inputs);
    const auto member_row = [&records](const std::int64_t id) {
        const auto record = std::find_if(records->begin(), records->end(),
                                         [id](const auto& value) { return value.id == id; });
        ui::RecurringIncidentMemberRow row{};
        row.id = id;
        if (record != records->end()) {
            row.created_utc = ui::format_utc_milliseconds(record->created_utc_milliseconds);
            row.created_utc_milliseconds = record->created_utc_milliseconds;
            row.label = record->label;
            switch (record->user_feedback) {
            case storage::IncidentUserFeedback::unanswered:
                row.user_feedback = ui::IncidentFeedback::unanswered;
                break;
            case storage::IncidentUserFeedback::noticed_problem:
                row.user_feedback = ui::IncidentFeedback::noticed_problem;
                break;
            case storage::IncidentUserFeedback::did_not_notice_problem:
                row.user_feedback = ui::IncidentFeedback::did_not_notice_problem;
                break;
            }
            switch (record->category) {
            case storage::IncidentCategory::unknown:
                row.category = ui::IncidentCategory::unknown;
                break;
            case storage::IncidentCategory::system_freeze:
                row.category = ui::IncidentCategory::system_freeze;
                break;
            case storage::IncidentCategory::game_stutter:
                row.category = ui::IncidentCategory::game_stutter;
                break;
            case storage::IncidentCategory::application_slowdown_or_hang:
                row.category = ui::IncidentCategory::application_slowdown_or_hang;
                break;
            case storage::IncidentCategory::network:
                row.category = ui::IncidentCategory::network;
                break;
            case storage::IncidentCategory::audio:
                row.category = ui::IncidentCategory::audio;
                break;
            }
        }
        return row;
    };
    ui::RecurringIncidentView view{};
    view.state = ui::RecurringIncidentViewState::ready;
    view.feature_version = clustered.feature_version;
    view.incidents_considered = clustered.inputs_considered;
    view.cached_features = cached_count;
    view.recomputed_features = updates.size();
    view.groups.reserve(clustered.clusters.size());
    for (const auto& cluster : clustered.clusters) {
        ui::RecurringIncidentGroupRow row{};
        row.name = cluster.manually_overridden
                       ? "User group: " + cluster.override_group
                       : "Automatic pattern " + std::to_string(cluster.stable_key);
        row.manually_overridden = cluster.manually_overridden;
        row.shared_evidence = detail::shared_characteristics_text(cluster);
        row.maximum_pair_distance = cluster.maximum_pair_distance;
        row.shared_characteristic_count = cluster.shared_characteristics.size();
        if (!cluster.shared_characteristics.empty()) {
            for (const auto& characteristic : cluster.shared_characteristics)
                row.average_shared_support += characteristic.support;
            row.average_shared_support /=
                static_cast<double>(cluster.shared_characteristics.size());
        }
        row.members.reserve(cluster.incident_ids.size());
        for (const auto id : cluster.incident_ids)
            row.members.push_back(member_row(id));
        view.groups.push_back(std::move(row));
    }
    view.noise.reserve(clustered.noise_incident_ids.size());
    for (const auto id : clustered.noise_incident_ids)
        view.noise.push_back(member_row(id));
    view.status = std::to_string(view.groups.size()) + " recurring groups from " +
                  std::to_string(view.incidents_considered) + " incidents";
    if (load_failures != 0U) view.status += "; " + std::to_string(load_failures) + " load failures";
    view.status += cache_warning;
    view.elapsed_milliseconds = milliseconds(std::chrono::steady_clock::now() - started);
    content.recurring = std::move(view);
    if (analyzer_ != nullptr && loaded_incident_ != nullptr && content.detail &&
        content.detail->id == loaded_incident_id_) {
        const auto analysis_started = std::chrono::steady_clock::now();
        loaded_analysis_ = detail::analyze_incident(
            analyzer_, profile_repository_, feedback_repository_, loaded_incident_id_,
            *loaded_incident_, content.recurring);
        content.detail->analysis = loaded_analysis_;
        content.last_analysis_milliseconds =
            milliseconds(std::chrono::steady_clock::now() - analysis_started);
    }
#else
    content.recurring.state = ui::RecurringIncidentViewState::disabled;
    content.recurring.status = "Recurring discovery disabled with analysis";
    content.recurring.elapsed_milliseconds =
        milliseconds(std::chrono::steady_clock::now() - started);
#endif
    publish(std::move(content));
}

void IncidentViewerService::publish(ui::IncidentViewerContent content) {
    const std::scoped_lock lock{mutex_};
    content.generation = ++generation_;
    snapshot_ = std::make_shared<const ui::IncidentViewerContent>(std::move(content));
}

void IncidentViewerService::publish_error(std::string message) {
    auto content = *snapshot();
    content.state = ui::IncidentViewerLoadState::error;
    content.status = std::move(message);
    publish(std::move(content));
}

} // namespace blackbox::app
