#include "evaluation/diagnostic_evaluation.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace blackbox::evaluation {
namespace {

[[nodiscard]] DiagnosticEvaluationError error(
    const DiagnosticEvaluationErrorCode code, std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] bool scorable(const IncidentTruth& truth) noexcept {
    return !truth.disagreement &&
           (truth.certainty == TruthCertainty::confirmed ||
            truth.certainty == TruthCertainty::probable);
}

void finish_rate(RateMetric& metric) noexcept {
    metric.rate = metric.eligible == 0U
                      ? 0.0
                      : static_cast<double>(metric.successful) /
                            static_cast<double>(metric.eligible);
}

[[nodiscard]] std::size_t choose_two(const std::size_t value) noexcept {
    return value < 2U ? 0U : value * (value - 1U) / 2U;
}

[[nodiscard]] bool valid_incident_type(
    const analysis::IncidentType value) noexcept {
    switch (value) {
    case analysis::IncidentType::unknown:
    case analysis::IncidentType::cpu_pressure:
    case analysis::IncidentType::memory_pressure:
    case analysis::IncidentType::storage_pressure:
    case analysis::IncidentType::network_pressure:
    case analysis::IncidentType::multi_resource_pressure:
    case analysis::IncidentType::application_crash:
    case analysis::IncidentType::application_hang:
    case analysis::IncidentType::dns_resolution_timeout:
    case analysis::IncidentType::display_driver_recovery:
    case analysis::IncidentType::storage_io_retry: return true;
    }
    return false;
}

[[nodiscard]] bool valid_context(
    const analysis::WorkloadContextKind value) noexcept {
    switch (value) {
    case analysis::WorkloadContextKind::unknown:
    case analysis::WorkloadContextKind::idle:
    case analysis::WorkloadContextKind::gaming:
    case analysis::WorkloadContextKind::development:
    case analysis::WorkloadContextKind::compilation:
    case analysis::WorkloadContextKind::video_playback_or_call:
    case analysis::WorkloadContextKind::heavy_download:
    case analysis::WorkloadContextKind::desktop: return true;
    }
    return false;
}

[[nodiscard]] bool valid_pressure(
    const std::optional<analysis::ResourceKind> value) noexcept {
    if (!value) return true;
    switch (*value) {
    case analysis::ResourceKind::cpu:
    case analysis::ResourceKind::memory:
    case analysis::ResourceKind::disk:
    case analysis::ResourceKind::network: return true;
    }
    return false;
}

[[nodiscard]] bool valid_incident_key(const std::string_view value) noexcept {
    return value.size() == 32U &&
           std::all_of(value.begin(), value.end(), [](const char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] bool valid_cluster(const std::string_view value) noexcept {
    return value.size() <= 64U &&
           std::all_of(value.begin(), value.end(), [](const char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') ||
                      character == '-' || character == '_' || character == '.';
           });
}

} // namespace

std::expected<DiagnosticEvaluationReport, DiagnosticEvaluationError>
evaluate_diagnostics(const DogfoodCorpus& corpus,
                     const std::vector<DiagnosticPrediction>& predictions,
                     const CorpusSplit split) noexcept {
    try {
        if (!corpus.manifest.frozen) {
            return std::unexpected{error(DiagnosticEvaluationErrorCode::corpus_not_frozen,
                                         "evaluation requires a frozen corpus")};
        }
        std::map<std::string, const IncidentTruth*> truth_by_key;
        for (const auto& truth : corpus.incidents) {
            if (truth.split == split) truth_by_key.emplace(truth.incident_key, &truth);
        }
        if (truth_by_key.empty()) {
            return std::unexpected{error(DiagnosticEvaluationErrorCode::no_truth_for_split,
                                         "selected split has no incident truth")};
        }
        if (predictions.size() > truth_by_key.size()) {
            return std::unexpected{error(DiagnosticEvaluationErrorCode::invalid_prediction,
                                         "prediction count exceeds selected-split truth")};
        }
        std::map<std::string, const DiagnosticPrediction*> prediction_by_key;
        for (const auto& prediction : predictions) {
            if (!std::isfinite(prediction.confidence) || prediction.confidence < 0.0 ||
                prediction.confidence > 1.0 ||
                !valid_incident_key(prediction.incident_key) ||
                prediction.contributor_ordinals.size() > 20U ||
                !valid_cluster(prediction.recurrence_cluster) ||
                !valid_incident_type(prediction.diagnosis) ||
                !valid_context(prediction.context) ||
                !valid_pressure(prediction.observed_pressure) ||
                !std::isfinite(prediction.practical_pressure_score) ||
                prediction.practical_pressure_score < 0.0 ||
                prediction.practical_pressure_score > 1.0 ||
                !std::isfinite(prediction.raw_statistical_score) ||
                prediction.raw_statistical_score < 0.0 ||
                prediction.raw_statistical_score > 1.0 ||
                std::any_of(prediction.contributor_ordinals.begin(),
                            prediction.contributor_ordinals.end(),
                            [](const auto ordinal) { return ordinal >= 8'192U; }) ||
                std::set<std::size_t>{prediction.contributor_ordinals.begin(),
                                      prediction.contributor_ordinals.end()}.size() !=
                    prediction.contributor_ordinals.size()) {
                return std::unexpected{error(DiagnosticEvaluationErrorCode::invalid_prediction,
                                             "prediction is outside protocol bounds")};
            }
            if (!truth_by_key.contains(prediction.incident_key)) {
                return std::unexpected{error(
                    DiagnosticEvaluationErrorCode::prediction_without_truth,
                    "prediction has no truth row in the selected split")};
            }
            if (!prediction_by_key.emplace(prediction.incident_key, &prediction).second) {
                return std::unexpected{error(DiagnosticEvaluationErrorCode::duplicate_prediction,
                                             "duplicate prediction key")};
            }
        }

        DiagnosticEvaluationReport report{};
        report.split = split;
        report.truth_rows = truth_by_key.size();
        std::set<std::string> represented_profiles;
        std::map<std::string, std::string> profile_by_session;
        for (const auto& session : corpus.sessions) {
            if (session.split == split) {
                profile_by_session.emplace(session.session_id, session.hardware_profile_id);
                if (session.symptom == SymptomClass::quiet) {
                    report.quiet_automatic_captures += session.automatic_captures;
                }
            }
        }
        for (const auto& session : corpus.sessions) {
            if (session.split == split && session.symptom == SymptomClass::quiet) {
                report.quiet_exposure_hours += session.duration_seconds / 3'600.0;
            }
        }
        report.false_captures_per_hour = report.quiet_exposure_hours == 0.0
                                             ? 0.0
                                             : static_cast<double>(
                                                   report.quiet_automatic_captures) /
                                                   report.quiet_exposure_hours;

        double brier_sum{};
        std::map<std::string, std::size_t> true_family_counts;
        std::map<std::string, std::size_t> predicted_cluster_counts;
        std::map<std::pair<std::string, std::string>, std::size_t> contingency;

        for (const auto& [key, truth] : truth_by_key) {
            ++report.symptom_counts[static_cast<std::size_t>(truth->symptom)];
            if (const auto profile = profile_by_session.find(truth->session_id);
                profile != profile_by_session.end()) {
                represented_profiles.insert(profile->second);
            }
            if (truth->disagreement) ++report.disagreement_excluded;
            else if (truth->certainty == TruthCertainty::uncertain ||
                     truth->certainty == TruthCertainty::unresolvable) {
                ++report.uncertain_excluded;
            }
            const auto found = prediction_by_key.find(key);
            const DiagnosticPrediction* prediction{};
            if (found == prediction_by_key.end()) {
                ++report.predictions_missing;
            } else {
                ++report.predictions_matched;
                prediction = found->second;
            }
            if (!scorable(*truth)) continue;
            ++report.unknown_rate.eligible;

            if (truth->expected_diagnosis != analysis::IncidentType::unknown) {
                ++report.supported_diagnosis_recall.eligible;
            } else {
                ++report.unknown_truth_abstention.eligible;
            }
            if (truth->expected_contributor_ordinal) {
                ++report.top1_contributor_accuracy.eligible;
                ++report.top3_contributor_accuracy.eligible;
            }
            if (truth->expected_context != analysis::WorkloadContextKind::unknown) {
                ++report.context_accuracy.eligible;
            }
            if (truth->detector_should_capture) {
                ++report.automatic_detection_recall.eligible;
            }
            if (truth->usefulness == UsefulnessRating::useful ||
                truth->usefulness == UsefulnessRating::not_useful) {
                ++report.usefulness.eligible;
                report.usefulness.successful +=
                    truth->usefulness == UsefulnessRating::useful ? 1U : 0U;
            }
            if (!truth->recurrence_family.empty()) {
                ++true_family_counts[truth->recurrence_family];
            }
            if (prediction == nullptr) continue;

            report.unknown_rate.successful +=
                prediction->diagnosis == analysis::IncidentType::unknown ? 1U : 0U;
            if (truth->expected_diagnosis != analysis::IncidentType::unknown) {
                report.supported_diagnosis_recall.successful +=
                    prediction->diagnosis == truth->expected_diagnosis ? 1U : 0U;
            } else {
                report.unknown_truth_abstention.successful +=
                    prediction->diagnosis == analysis::IncidentType::unknown ? 1U : 0U;
            }
            if (prediction->diagnosis != analysis::IncidentType::unknown) {
                const auto correct =
                    truth->expected_diagnosis != analysis::IncidentType::unknown &&
                    prediction->diagnosis == truth->expected_diagnosis;
                ++report.supported_diagnosis_precision.eligible;
                report.supported_diagnosis_precision.successful += correct ? 1U : 0U;
                const auto residual = prediction->confidence - (correct ? 1.0 : 0.0);
                brier_sum += residual * residual;
                ++report.calibration_rows;
                const auto bin = (std::min)(
                    calibration_bin_count - 1U,
                    static_cast<std::size_t>(prediction->confidence *
                                             calibration_bin_count));
                auto& aggregate = report.calibration_bins[bin];
                ++aggregate.count;
                aggregate.average_confidence += prediction->confidence;
                aggregate.accuracy += correct ? 1.0 : 0.0;
            }
            if (truth->expected_contributor_ordinal) {
                if (!prediction->contributor_ordinals.empty() &&
                    prediction->contributor_ordinals.front() ==
                        *truth->expected_contributor_ordinal) {
                    ++report.top1_contributor_accuracy.successful;
                }
                const auto top = (std::min)(std::size_t{3U},
                                            prediction->contributor_ordinals.size());
                if (std::find(prediction->contributor_ordinals.begin(),
                              prediction->contributor_ordinals.begin() +
                                  static_cast<std::ptrdiff_t>(top),
                              *truth->expected_contributor_ordinal) !=
                    prediction->contributor_ordinals.begin() +
                        static_cast<std::ptrdiff_t>(top)) {
                    ++report.top3_contributor_accuracy.successful;
                }
            }
            if (truth->expected_context != analysis::WorkloadContextKind::unknown) {
                report.context_accuracy.successful +=
                    prediction->context == truth->expected_context ? 1U : 0U;
            }
            if (truth->detector_should_capture) {
                report.automatic_detection_recall.successful +=
                    prediction->automatic_capture ? 1U : 0U;
            }
            if (!truth->recurrence_family.empty() &&
                !prediction->recurrence_cluster.empty()) {
                    ++contingency[{truth->recurrence_family,
                                  prediction->recurrence_cluster}];
            }
            if (!prediction->recurrence_cluster.empty()) {
                ++predicted_cluster_counts[prediction->recurrence_cluster];
            }
        }
        report.hardware_profiles_represented = represented_profiles.size();
        report.brier_score = report.calibration_rows == 0U
                                 ? 0.0
                                 : brier_sum /
                                       static_cast<double>(report.calibration_rows);
        for (auto& bin : report.calibration_bins) {
            if (bin.count == 0U) continue;
            bin.average_confidence /= static_cast<double>(bin.count);
            bin.accuracy /= static_cast<double>(bin.count);
            report.expected_calibration_error +=
                static_cast<double>(bin.count) /
                static_cast<double>((std::max)(std::size_t{1U},
                                               report.calibration_rows)) *
                std::abs(bin.average_confidence - bin.accuracy);
        }
        finish_rate(report.supported_diagnosis_recall);
        finish_rate(report.supported_diagnosis_precision);
        finish_rate(report.unknown_truth_abstention);
        report.false_assertion_rate.eligible =
            report.unknown_truth_abstention.eligible;
        report.false_assertion_rate.successful =
            report.unknown_truth_abstention.eligible -
            report.unknown_truth_abstention.successful;
        finish_rate(report.false_assertion_rate);
        finish_rate(report.top1_contributor_accuracy);
        finish_rate(report.top3_contributor_accuracy);
        finish_rate(report.context_accuracy);
        finish_rate(report.automatic_detection_recall);
        report.automatic_detection_miss_rate.eligible =
            report.automatic_detection_recall.eligible;
        report.automatic_detection_miss_rate.successful =
            report.automatic_detection_recall.eligible -
            report.automatic_detection_recall.successful;
        finish_rate(report.automatic_detection_miss_rate);
        finish_rate(report.usefulness);
        finish_rate(report.unknown_rate);
        for (const auto& [family, count] : true_family_counts) {
            static_cast<void>(family);
            report.recurrence_true_pairs += choose_two(count);
        }
        for (const auto& [cluster, count] : predicted_cluster_counts) {
            static_cast<void>(cluster);
            report.recurrence_predicted_pairs += choose_two(count);
        }
        for (const auto& [cell, count] : contingency) {
            static_cast<void>(cell);
            report.recurrence_true_positive_pairs += choose_two(count);
        }
        report.recurrence_pair_precision = report.recurrence_predicted_pairs == 0U
            ? 0.0 : static_cast<double>(report.recurrence_true_positive_pairs) /
                        static_cast<double>(report.recurrence_predicted_pairs);
        report.recurrence_pair_recall = report.recurrence_true_pairs == 0U
            ? 0.0 : static_cast<double>(report.recurrence_true_positive_pairs) /
                        static_cast<double>(report.recurrence_true_pairs);
        const auto pair_sum = report.recurrence_pair_precision +
                              report.recurrence_pair_recall;
        report.recurrence_pair_f1 = pair_sum == 0.0 ? 0.0 :
            2.0 * report.recurrence_pair_precision * report.recurrence_pair_recall /
                pair_sum;
        return report;
    } catch (const std::exception& exception) {
        return std::unexpected{error(DiagnosticEvaluationErrorCode::invalid_prediction,
                                     exception.what())};
    } catch (...) {
        return std::unexpected{error(DiagnosticEvaluationErrorCode::invalid_prediction,
                                     "unknown evaluation failure")};
    }
}

DiagnosticQualification qualify_v0151(
    const DiagnosticEvaluationReport& report) noexcept {
    DiagnosticQualification result{};
    result.passed =
        report.supported_diagnosis_precision.eligible > 0U &&
        report.supported_diagnosis_recall.eligible > 0U &&
        report.unknown_truth_abstention.eligible > 0U &&
        report.top3_contributor_accuracy.eligible > 0U &&
        report.supported_diagnosis_precision.rate >= result.minimum_precision &&
        report.supported_diagnosis_recall.rate >= result.minimum_supported_recall &&
        report.unknown_truth_abstention.rate >= result.minimum_unknown_abstention &&
        report.top3_contributor_accuracy.rate >= result.minimum_top3_contributor;
    return result;
}

std::optional<ConfidenceCalibrationModel> fit_isotonic_confidence_calibration(
    std::vector<CalibrationSample> samples) noexcept {
    try {
        if (samples.size() < 10U ||
            std::any_of(samples.begin(), samples.end(), [](const auto& sample) {
                return !std::isfinite(sample.confidence) || sample.confidence < 0.0 ||
                       sample.confidence > 1.0;
            })) {
            return std::nullopt;
        }
        std::sort(samples.begin(), samples.end(), [](const auto& left, const auto& right) {
            if (left.confidence != right.confidence) return left.confidence < right.confidence;
            return left.correct < right.correct;
        });
        struct Block { double maximum{}; double successes{}; std::size_t count{}; };
        std::vector<Block> blocks;
        blocks.reserve(samples.size());
        for (const auto& sample : samples) {
            blocks.push_back({sample.confidence, sample.correct ? 1.0 : 0.0, 1U});
            while (blocks.size() >= 2U) {
                const auto last_rate = blocks.back().successes /
                                       static_cast<double>(blocks.back().count);
                const auto previous_rate = blocks[blocks.size() - 2U].successes /
                                           static_cast<double>(blocks[blocks.size() - 2U].count);
                if (previous_rate <= last_rate) break;
                const auto merged = Block{
                    blocks.back().maximum,
                    blocks.back().successes + blocks[blocks.size() - 2U].successes,
                    blocks.back().count + blocks[blocks.size() - 2U].count};
                blocks.pop_back();
                blocks.back() = merged;
            }
        }
        while (blocks.size() > maximum_calibration_knots) {
            std::size_t smallest = 0U;
            for (std::size_t index = 1U; index + 1U < blocks.size(); ++index) {
                if (blocks[index].count < blocks[smallest].count) smallest = index;
            }
            const auto right = (smallest + 1U < blocks.size()) ? smallest + 1U : smallest;
            const auto left = right - 1U;
            blocks[left] = {blocks[right].maximum,
                            blocks[left].successes + blocks[right].successes,
                            blocks[left].count + blocks[right].count};
            blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(right));
        }
        ConfidenceCalibrationModel model{};
        model.source_samples = samples.size();
        for (const auto& block : blocks) {
            model.knots.push_back({block.maximum,
                                   block.successes / static_cast<double>(block.count),
                                   block.count});
        }
        return model;
    } catch (...) {
        return std::nullopt;
    }
}

double apply_confidence_calibration(const ConfidenceCalibrationModel& model,
                                    const double confidence) noexcept {
    if (model.knots.empty() || !std::isfinite(confidence)) return 0.0;
    const auto bounded = std::clamp(confidence, 0.0, 1.0);
    const auto found = std::find_if(model.knots.begin(), model.knots.end(),
                                    [&](const auto& knot) {
                                        return bounded <= knot.maximum_input;
                                    });
    return found == model.knots.end() ? model.knots.back().calibrated_probability
                                     : found->calibrated_probability;
}

AssertionThresholdSelection select_assertion_threshold(
    std::vector<CalibrationSample> calibrated_samples,
    const double minimum_precision) noexcept {
    AssertionThresholdSelection selection{};
    selection.minimum_precision = minimum_precision;
    selection.calibration_rows = calibrated_samples.size();
    if (calibrated_samples.empty() || !std::isfinite(minimum_precision) ||
        minimum_precision <= 0.5 || minimum_precision > 1.0 ||
        std::any_of(calibrated_samples.begin(), calibrated_samples.end(),
                    [](const auto& sample) {
                        return !std::isfinite(sample.confidence) ||
                               sample.confidence < 0.0 || sample.confidence > 1.0;
                    })) {
        return selection;
    }
    std::sort(calibrated_samples.begin(), calibrated_samples.end(),
              [](const auto& left, const auto& right) {
                  if (left.confidence != right.confidence)
                      return left.confidence > right.confidence;
                  return left.correct > right.correct;
              });
    std::size_t correct{};
    for (std::size_t index = 0U; index < calibrated_samples.size();) {
        const auto threshold = calibrated_samples[index].confidence;
        auto end = index;
        while (end < calibrated_samples.size() &&
               calibrated_samples[end].confidence == threshold) {
            correct += calibrated_samples[end].correct ? 1U : 0U;
            ++end;
        }
        const auto precision = static_cast<double>(correct) /
                               static_cast<double>(end);
        if (precision >= minimum_precision) {
            selection.assertions_enabled = true;
            selection.threshold = threshold;
            selection.asserted_rows = end;
            selection.observed_precision = precision;
        }
        index = end;
    }
    return selection;
}

} // namespace blackbox::evaluation
