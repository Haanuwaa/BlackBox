#include "analysis/workload_context.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

namespace blackbox::analysis {
namespace {

using namespace std::string_view_literals;

constexpr std::array known_contexts{
    WorkloadContextKind::idle,
    WorkloadContextKind::gaming,
    WorkloadContextKind::development,
    WorkloadContextKind::compilation,
    WorkloadContextKind::video_playback_or_call,
    WorkloadContextKind::heavy_download,
    WorkloadContextKind::desktop,
};

[[nodiscard]] constexpr std::size_t context_index(
    const WorkloadContextKind context) noexcept {
    return static_cast<std::size_t>(context) - 1U;
}

[[nodiscard]] double clamp_unit(const double value) noexcept {
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] bool available(const core::RecordedValue<double>& value) noexcept {
    return value.status == core::RecordedValueStatus::available &&
           std::isfinite(value.value) && value.value >= 0.0;
}

struct Average final {
    double sum{};
    std::size_t count{};

    void add(const double value) noexcept {
        const auto maximum = (std::numeric_limits<double>::max)();
        sum = value > maximum - sum ? maximum : sum + value;
        ++count;
    }
    [[nodiscard]] double value() const noexcept {
        return count == 0U ? 0.0 : sum / static_cast<double>(count);
    }
};

[[nodiscard]] char ascii_lower(const char value) noexcept {
    return value >= 'A' && value <= 'Z'
               ? static_cast<char>(value - 'A' + 'a')
               : value;
}

[[nodiscard]] std::string executable_stem(std::string_view value) {
    const auto separator = value.find_last_of("/\\");
    if (separator != std::string_view::npos) value.remove_prefix(separator + 1U);
    if (const auto dot = value.find_last_of('.'); dot != std::string_view::npos) {
        value = value.substr(0U, dot);
    }
    constexpr std::size_t maximum_name_bytes = 260U;
    value = value.substr(0U, (std::min)(value.size(), maximum_name_bytes));
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) result.push_back(ascii_lower(character));
    return result;
}

template <std::size_t Size>
[[nodiscard]] bool matches_any(const std::string_view value,
                               const std::array<std::string_view, Size>& terms) noexcept {
    return std::any_of(terms.begin(), terms.end(), [value](const auto term) {
        return value == term || (term.size() >= 5U && value.find(term) != std::string_view::npos);
    });
}

struct NameMatches final {
    std::array<std::size_t, known_contexts.size()> counts{};
};

[[nodiscard]] NameMatches process_name_matches(
    const core::IncidentSnapshot& incident, const std::size_t maximum_metadata,
    std::size_t& considered) {
    constexpr std::array gaming{"game"sv, "gameclient"sv, "unity"sv, "unreal"sv,
                                "playnite"sv};
    constexpr std::array development{"code"sv, "devenv"sv, "idea"sv, "pycharm"sv,
                                     "rider"sv, "clangd"sv, "emacs"sv, "vim"sv,
                                     "python"sv, "node"sv};
    constexpr std::array compilation{"cl"sv, "clang"sv, "gcc"sv, "g++"sv,
                                     "link"sv, "msbuild"sv, "ninja"sv, "cmake"sv,
                                     "rustc"sv, "cargo"sv, "javac"sv};
    constexpr std::array video{"zoom"sv, "teams"sv, "webex"sv, "discord"sv,
                               "vlc"sv, "obs"sv, "ffmpeg"sv, "media"sv};
    constexpr std::array download{"curl"sv, "wget"sv, "aria2"sv, "download"sv,
                                  "bitsadmin"sv, "torrent"sv};
    constexpr std::array desktop{"explorer"sv, "dwm"sv, "shellhost"sv,
                                 "searchhost"sv, "taskmgr"sv};

    NameMatches result{};
    for (const auto& process : incident.process_metadata()) {
        if (considered == maximum_metadata) break;
        ++considered;
        if (process.name.status != core::RecordedValueStatus::available) continue;
        const auto stem = executable_stem(process.name.value);
        if (stem.empty()) continue;
        if (matches_any(stem, gaming))
            ++result.counts[context_index(WorkloadContextKind::gaming)];
        if (matches_any(stem, development))
            ++result.counts[context_index(WorkloadContextKind::development)];
        if (matches_any(stem, compilation))
            ++result.counts[context_index(WorkloadContextKind::compilation)];
        if (matches_any(stem, video))
            ++result.counts[context_index(WorkloadContextKind::video_playback_or_call)];
        if (matches_any(stem, download))
            ++result.counts[context_index(WorkloadContextKind::heavy_download)];
        if (matches_any(stem, desktop))
            ++result.counts[context_index(WorkloadContextKind::desktop)];
    }
    return result;
}

[[nodiscard]] double probability_for(const WorkloadContextAssessment& assessment,
                                     const WorkloadContextKind context) noexcept {
    for (const auto& probability : assessment.probabilities) {
        if (probability.context == context) return probability.probability;
    }
    return 0.0;
}

[[nodiscard]] double expected_resource_probability(
    const WorkloadContextAssessment& assessment,
    const ResourceKind resource) noexcept {
    const auto expected = [resource](const WorkloadContextKind context) noexcept {
        switch (context) {
        case WorkloadContextKind::gaming:
            switch (resource) {
            case ResourceKind::cpu: return 0.90;
            case ResourceKind::memory: return 0.55;
            case ResourceKind::disk: return 0.15;
            case ResourceKind::network: return 0.20;
            }
            break;
        case WorkloadContextKind::development:
            switch (resource) {
            case ResourceKind::cpu: return 0.35;
            case ResourceKind::memory: return 0.45;
            case ResourceKind::disk: return 0.30;
            case ResourceKind::network: return 0.10;
            }
            break;
        case WorkloadContextKind::compilation:
            switch (resource) {
            case ResourceKind::cpu: return 1.00;
            case ResourceKind::memory: return 0.35;
            case ResourceKind::disk: return 0.80;
            case ResourceKind::network: return 0.05;
            }
            break;
        case WorkloadContextKind::video_playback_or_call:
            switch (resource) {
            case ResourceKind::cpu: return 0.45;
            case ResourceKind::memory: return 0.30;
            case ResourceKind::disk: return 0.20;
            case ResourceKind::network: return 0.75;
            }
            break;
        case WorkloadContextKind::heavy_download:
            switch (resource) {
            case ResourceKind::cpu: return 0.10;
            case ResourceKind::memory: return 0.10;
            case ResourceKind::disk: return 0.65;
            case ResourceKind::network: return 1.00;
            }
            break;
        case WorkloadContextKind::desktop:
            switch (resource) {
            case ResourceKind::cpu: return 0.20;
            case ResourceKind::memory: return 0.25;
            case ResourceKind::disk: return 0.15;
            case ResourceKind::network: return 0.10;
            }
            break;
        case WorkloadContextKind::unknown:
        case WorkloadContextKind::idle: return 0.0;
        }
        return 0.0;
    };

    double result{};
    for (const auto context : known_contexts) {
        result += probability_for(assessment, context) * expected(context);
    }
    return clamp_unit(result);
}

[[nodiscard]] ResourceKind process_resource(const ProcessAnomaly& process) noexcept {
    const MetricAnomalyEvidence* strongest = nullptr;
    for (const auto& evidence : process.evidence) {
        if (evidence.availability == EvidenceAvailability::available &&
            (strongest == nullptr || evidence.score > strongest->score)) {
            strongest = &evidence;
        }
    }
    if (strongest == nullptr) return ResourceKind::cpu;
    switch (strongest->metric) {
    case MetricKind::process_cpu: return ResourceKind::cpu;
    case MetricKind::process_working_set: return ResourceKind::memory;
    case MetricKind::process_disk_read:
    case MetricKind::process_disk_write: return ResourceKind::disk;
    default: return ResourceKind::cpu;
    }
}

} // namespace

std::expected<WorkloadContextConfiguration, WorkloadContextConfigurationError>
validate_workload_context_configuration(
    const WorkloadContextConfiguration configuration) noexcept {
    if (configuration.maximum_process_metadata == 0U ||
        configuration.maximum_process_metadata > maximum_context_process_metadata) {
        return std::unexpected{
            WorkloadContextConfigurationError::process_metadata_limit_invalid};
    }
    if (configuration.maximum_evidence == 0U ||
        configuration.maximum_evidence > maximum_context_evidence) {
        return std::unexpected{WorkloadContextConfigurationError::evidence_limit_invalid};
    }
    if (!std::isfinite(configuration.maximum_score_reduction) ||
        configuration.maximum_score_reduction < 0.0 ||
        configuration.maximum_score_reduction > 0.25) {
        return std::unexpected{WorkloadContextConfigurationError::score_reduction_invalid};
    }
    return configuration;
}

WorkloadContextAssessment recognize_workload_context(
    const core::IncidentSnapshot& incident,
    const WorkloadContextConfiguration configuration) {
    WorkloadContextAssessment result{};
    result.enabled = configuration.enabled;
    if (!configuration.enabled) {
        result.probabilities.push_back({WorkloadContextKind::unknown, 1.0});
        return result;
    }

    Average cpu;
    Average memory;
    Average disk;
    Average network;
    for (const auto& sample : incident.system_samples()) {
        ++result.system_samples_considered;
        if (available(sample.cpu_fraction)) cpu.add(sample.cpu_fraction.value);
        if (available(sample.memory_fraction)) memory.add(sample.memory_fraction.value);
        double disk_value{};
        bool disk_available = false;
        if (available(sample.disk_read_bytes_per_second)) {
            disk_value += sample.disk_read_bytes_per_second.value;
            disk_available = true;
        }
        if (available(sample.disk_write_bytes_per_second)) {
            disk_value += sample.disk_write_bytes_per_second.value;
            disk_available = true;
        }
        if (disk_available) disk.add(disk_value);
        double network_value{};
        bool network_available = false;
        if (available(sample.network_receive_bytes_per_second)) {
            network_value += sample.network_receive_bytes_per_second.value;
            network_available = true;
        }
        if (available(sample.network_transmit_bytes_per_second)) {
            network_value += sample.network_transmit_bytes_per_second.value;
            network_available = true;
        }
        if (network_available) network.add(network_value);
    }

    const auto cpu_level = clamp_unit(cpu.value());
    const auto memory_level = clamp_unit(memory.value());
    constexpr double mebibyte = 1024.0 * 1024.0;
    const auto disk_level = clamp_unit(disk.value() / (64.0 * mebibyte));
    const auto network_level = clamp_unit(network.value() / (64.0 * mebibyte));
    const auto low_activity = 1.0 - (std::max)({clamp_unit(cpu_level / 0.15),
                                                clamp_unit(disk.value() / (4.0 * mebibyte)),
                                                clamp_unit(network.value() / (2.0 * mebibyte))});
    const auto moderate_activity = clamp_unit(1.0 - std::abs(cpu_level - 0.12) / 0.20);
    const auto sample_count = static_cast<double>(incident.system_samples().size());
    const auto coverage_for = [sample_count](const Average& average) noexcept {
        return sample_count == 0.0
                   ? 0.0
                   : static_cast<double>(average.count) / sample_count;
    };
    const auto cpu_coverage = coverage_for(cpu);
    const auto memory_coverage = coverage_for(memory);
    const auto disk_coverage = coverage_for(disk);
    const auto network_coverage = coverage_for(network);
    const auto activity_coverage = (std::min)({cpu_coverage, disk_coverage,
                                               network_coverage});
    const auto system_signal_coverage =
        (cpu_coverage + memory_coverage + disk_coverage + network_coverage) / 4.0;

    const auto names = process_name_matches(
        incident, configuration.maximum_process_metadata,
        result.process_metadata_considered);
    std::array<double, known_contexts.size()> supports{};
    std::vector<WorkloadContextEvidence> components;
    components.reserve(32U);
    const auto add = [&](const WorkloadContextKind context,
                         const ContextSignalKind signal, const double observed,
                         const double contribution) {
        const auto bounded = clamp_unit(contribution);
        supports[context_index(context)] = clamp_unit(
            supports[context_index(context)] + bounded);
        if (bounded >= 0.005) components.push_back({context, signal, observed, bounded});
    };
    const auto name_component = [&](const WorkloadContextKind context,
                                    const double coefficient) {
        const auto count = names.counts[context_index(context)];
        add(context, ContextSignalKind::process_name_match,
            static_cast<double>(count), count == 0U ? 0.0 : coefficient);
    };

    add(WorkloadContextKind::idle, ContextSignalKind::low_activity,
        low_activity, 0.82 * low_activity * activity_coverage);
    add(WorkloadContextKind::idle, ContextSignalKind::average_memory,
        memory.value(), 0.18 * (1.0 - memory_level) * memory_coverage);

    name_component(WorkloadContextKind::gaming, 0.72);
    add(WorkloadContextKind::gaming, ContextSignalKind::average_cpu,
        cpu.value(), 0.18 * cpu_level * cpu_coverage);
    add(WorkloadContextKind::gaming, ContextSignalKind::average_memory,
        memory.value(), 0.10 * memory_level * memory_coverage);

    name_component(WorkloadContextKind::development, 0.70);
    add(WorkloadContextKind::development, ContextSignalKind::average_cpu,
        cpu.value(), 0.15 * cpu_level * cpu_coverage);
    add(WorkloadContextKind::development, ContextSignalKind::average_memory,
        memory.value(), 0.10 * memory_level * memory_coverage);
    add(WorkloadContextKind::development, ContextSignalKind::average_disk_throughput,
        disk.value(), 0.05 * disk_level * disk_coverage);

    name_component(WorkloadContextKind::compilation, 0.72);
    add(WorkloadContextKind::compilation, ContextSignalKind::average_cpu,
        cpu.value(), 0.16 * cpu_level * cpu_coverage);
    add(WorkloadContextKind::compilation, ContextSignalKind::average_disk_throughput,
        disk.value(), 0.12 * disk_level * disk_coverage);

    name_component(WorkloadContextKind::video_playback_or_call, 0.78);
    add(WorkloadContextKind::video_playback_or_call, ContextSignalKind::average_cpu,
        cpu.value(), 0.10 * cpu_level * cpu_coverage);
    add(WorkloadContextKind::video_playback_or_call,
        ContextSignalKind::average_network_throughput,
        network.value(), 0.12 * network_level * network_coverage);

    name_component(WorkloadContextKind::heavy_download, 0.10);
    add(WorkloadContextKind::heavy_download,
        ContextSignalKind::average_network_throughput,
        network.value(), 0.78 * network_level * network_coverage);
    add(WorkloadContextKind::heavy_download,
        ContextSignalKind::average_disk_throughput,
        disk.value(), 0.12 * disk_level * disk_coverage);

    name_component(WorkloadContextKind::desktop, 0.70);
    add(WorkloadContextKind::desktop, ContextSignalKind::moderate_activity,
        moderate_activity, 0.20 * moderate_activity * cpu_coverage);
    add(WorkloadContextKind::desktop, ContextSignalKind::low_activity,
        low_activity, 0.10 * low_activity * activity_coverage);

    std::array<std::size_t, known_contexts.size()> order{};
    for (std::size_t index = 0U; index < order.size(); ++index) order[index] = index;
    std::sort(order.begin(), order.end(), [&supports](const auto left, const auto right) {
        if (supports[left] != supports[right]) return supports[left] > supports[right];
        return left < right;
    });
    const auto top_support = supports[order[0]];
    const auto second_support = supports[order[1]];
    const auto margin = top_support - second_support;
    const auto unknown_support = top_support < 0.25
                                     ? 0.95
                                     : std::clamp(0.15 +
                                                      (std::max)(0.0, 0.72 - top_support) * 1.5 +
                                                      (std::max)(0.0, 0.15 - margin) * 2.0,
                                                  0.15, 0.95);

    std::array<double, known_contexts.size() + 1U> weights{};
    weights[0] = std::exp(5.0 * unknown_support) - 1.0;
    double total_weight = weights[0];
    for (std::size_t index = 0U; index < supports.size(); ++index) {
        weights[index + 1U] = std::exp(5.0 * supports[index]) - 1.0;
        total_weight += weights[index + 1U];
    }
    if (!(total_weight > 0.0) || !std::isfinite(total_weight)) {
        weights.fill(0.0);
        weights[0] = 1.0;
        total_weight = 1.0;
    }
    result.probabilities.reserve(weights.size());
    result.probabilities.push_back({WorkloadContextKind::unknown,
                                    weights[0] / total_weight});
    for (std::size_t index = 0U; index < known_contexts.size(); ++index) {
        result.probabilities.push_back({known_contexts[index],
                                        weights[index + 1U] / total_weight});
    }
    const auto primary = std::max_element(
        result.probabilities.begin(), result.probabilities.end(),
        [](const auto& left, const auto& right) {
            if (left.probability != right.probability)
                return left.probability < right.probability;
            return left.context > right.context;
        });
    result.primary = primary->context;
    result.confidence = primary->probability;
    result.uncertainty = result.primary == WorkloadContextKind::unknown
                             ? primary->probability
                             : 1.0 - primary->probability;

    const auto strongest_context = known_contexts[order[0]];
    const auto runner_up_context = known_contexts[order[1]];
    std::erase_if(components, [&](const auto& component) {
        return result.primary == WorkloadContextKind::unknown
                   ? component.context != strongest_context &&
                         component.context != runner_up_context
                   : component.context != result.primary;
    });
    std::sort(components.begin(), components.end(), [](const auto& left, const auto& right) {
        if (left.contribution != right.contribution)
            return left.contribution > right.contribution;
        if (left.context != right.context) return left.context < right.context;
        return left.signal < right.signal;
    });
    if (result.primary == WorkloadContextKind::unknown) {
        components.insert(components.begin(), WorkloadContextEvidence{
            WorkloadContextKind::unknown, ContextSignalKind::ambiguous_margin,
            margin, unknown_support});
    }
    components.push_back({result.primary, ContextSignalKind::system_sample_coverage,
                          system_signal_coverage, 0.0});
    if (components.size() > configuration.maximum_evidence)
        components.resize(configuration.maximum_evidence);
    result.evidence = std::move(components);
    return result;
}

void apply_workload_context(
    IncidentAnalysis& analysis,
    const WorkloadContextConfiguration& configuration) noexcept {
    const auto multiplier = [&](const ResourceKind resource) noexcept {
        if (!configuration.enabled || !analysis.workload_context.enabled) return 1.0;
        return 1.0 - configuration.maximum_score_reduction *
                         expected_resource_probability(analysis.workload_context, resource);
    };
    for (auto& resource : analysis.resources) {
        resource.context_multiplier = multiplier(resource.resource);
        resource.score = clamp_unit(resource.uncontextualized_score *
                                    resource.context_multiplier);
    }
    for (auto& process : analysis.processes) {
        process.context_multiplier = multiplier(process_resource(process));
        process.score = clamp_unit(process.uncontextualized_score *
                                   process.context_multiplier);
    }
    std::sort(analysis.resources.begin(), analysis.resources.end(),
              [](const auto& left, const auto& right) {
                  if (left.score != right.score) return left.score > right.score;
                  return left.resource < right.resource;
              });
    std::sort(analysis.processes.begin(), analysis.processes.end(),
              [](const auto& left, const auto& right) {
                  if (left.score != right.score) return left.score > right.score;
                  return left.identity < right.identity;
              });
}

} // namespace blackbox::analysis
