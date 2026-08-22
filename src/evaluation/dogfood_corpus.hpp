#pragma once

#include "analysis/incident_analyzer.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace blackbox::evaluation {

inline constexpr std::uint32_t dogfood_protocol_version = 1U;
inline constexpr std::size_t maximum_dogfood_incidents = 10'000U;
inline constexpr std::size_t maximum_dogfood_sessions = 2'000U;
inline constexpr std::size_t maximum_dogfood_annotations = 100'000U;
inline constexpr std::uintmax_t maximum_annotation_ballot_bytes = 4U * 1'024U;
inline constexpr std::size_t maximum_hardware_profiles = 128U;
inline constexpr std::size_t minimum_calibration_diagnoses = 10U;
inline constexpr std::size_t minimum_held_out_truth_rows = 10U;
inline constexpr std::size_t minimum_qualification_hardware_profiles = 3U;
inline constexpr std::size_t minimum_natural_sessions = 6U;
inline constexpr double minimum_quiet_exposure_seconds = 36'000.0;
inline constexpr double minimum_quiet_exposure_per_profile_split_seconds = 3'600.0;

enum class CorpusSplit : std::uint8_t {
    development,
    calibration,
    held_out,
};

enum class DogfoodSessionKind : std::uint8_t {
    controlled,
    natural,
    quiet,
};

enum class SymptomClass : std::uint8_t {
    cpu_starvation,
    disk_stall,
    network_interruption,
    application_crash,
    application_hang,
    game_stutter,
    audio_interruption,
    quiet,
    ambiguous,
};

inline constexpr std::size_t dogfood_symptom_class_count =
    static_cast<std::size_t>(SymptomClass::ambiguous) + 1U;

enum class TruthCertainty : std::uint8_t {
    confirmed,
    probable,
    uncertain,
    unresolvable,
};

enum class UsefulnessRating : std::uint8_t {
    unscored,
    not_useful,
    unsure,
    useful,
};

struct DogfoodManifest {
    std::string corpus_id{};
    bool frozen{};
    std::uint32_t pipeline_version{};
    std::uint64_t configuration_fingerprint{};
    std::uint64_t annotation_fingerprint{};
    friend bool operator==(const DogfoodManifest&, const DogfoodManifest&) = default;
};

struct HardwareProfile {
    std::string profile_id{};
    std::string os_family{};
    std::string os_build_bucket{};
    std::string cpu_family{};
    std::size_t logical_processors{};
    std::string memory_gib_bucket{};
    std::string gpu_family{};
    std::string power_mode{};
    friend bool operator==(const HardwareProfile&, const HardwareProfile&) = default;
};

struct DogfoodSession {
    std::string session_id{};
    std::string hardware_profile_id{};
    std::string operator_id{};
    CorpusSplit split{CorpusSplit::development};
    DogfoodSessionKind kind{DogfoodSessionKind::natural};
    SymptomClass symptom{SymptomClass::ambiguous};
    double duration_seconds{};
    std::size_t expected_incidents{};
    std::size_t automatic_captures{};
    bool consent_attested{};
    friend bool operator==(const DogfoodSession&, const DogfoodSession&) = default;
};

struct IncidentTruth {
    std::string incident_key{};
    std::string session_id{};
    CorpusSplit split{CorpusSplit::development};
    SymptomClass symptom{SymptomClass::ambiguous};
    TruthCertainty certainty{TruthCertainty::uncertain};
    bool user_visible{};
    analysis::IncidentType expected_diagnosis{analysis::IncidentType::unknown};
    std::optional<std::size_t> expected_contributor_ordinal{};
    analysis::WorkloadContextKind expected_context{
        analysis::WorkloadContextKind::unknown};
    std::string recurrence_family{};
    bool detector_should_capture{};
    UsefulnessRating usefulness{UsefulnessRating::unscored};
    std::size_t annotator_count{1U};
    bool disagreement{};
    friend bool operator==(const IncidentTruth&, const IncidentTruth&) = default;
};

struct IncidentAnnotation {
    std::string incident_key{};
    std::string annotator_id{};
    SymptomClass symptom{SymptomClass::ambiguous};
    TruthCertainty certainty{TruthCertainty::uncertain};
    bool user_visible{};
    analysis::IncidentType expected_diagnosis{analysis::IncidentType::unknown};
    std::optional<std::size_t> expected_contributor_ordinal{};
    analysis::WorkloadContextKind expected_context{
        analysis::WorkloadContextKind::unknown};
    std::string recurrence_family{};
    UsefulnessRating usefulness{UsefulnessRating::unscored};
    friend bool operator==(const IncidentAnnotation&, const IncidentAnnotation&) = default;
};

struct DogfoodBallotComparison {
    std::string incident_key{};
    std::string first_annotator_id{};
    std::string second_annotator_id{};
    bool disagreement{};
    friend bool operator==(const DogfoodBallotComparison&,
                           const DogfoodBallotComparison&) = default;
};

struct DogfoodHardwareQualification {
    std::string profile_id{};
    bool calibration_natural{};
    bool held_out_natural{};
    double calibration_quiet_seconds{};
    double held_out_quiet_seconds{};
    bool calibration_scorable_truth{};
    bool held_out_scorable_truth{};

    [[nodiscard]] bool fully_qualified() const noexcept {
        return calibration_natural && held_out_natural &&
               calibration_quiet_seconds >=
                   minimum_quiet_exposure_per_profile_split_seconds &&
               held_out_quiet_seconds >=
                   minimum_quiet_exposure_per_profile_split_seconds &&
               calibration_scorable_truth && held_out_scorable_truth;
    }
    friend bool operator==(const DogfoodHardwareQualification&,
                           const DogfoodHardwareQualification&) = default;
};

struct DogfoodQualificationReport {
    std::size_t represented_hardware_profiles{};
    std::size_t fully_qualified_hardware_profiles{};
    std::size_t natural_sessions{};
    std::size_t calibration_natural_sessions{};
    std::size_t held_out_natural_sessions{};
    double quiet_exposure_seconds{};
    double calibration_quiet_exposure_seconds{};
    double held_out_quiet_exposure_seconds{};
    std::size_t calibration_supported_diagnoses{};
    std::size_t held_out_scorable_truth_rows{};
    std::array<bool, dogfood_symptom_class_count> calibration_symptom_coverage{};
    std::array<bool, dogfood_symptom_class_count> held_out_symptom_coverage{};
    std::size_t insufficient_independent_annotation_rows{};
    std::vector<DogfoodHardwareQualification> hardware_qualification{};
    std::vector<std::string> unmet_requirements{};

    [[nodiscard]] bool ready_to_freeze() const noexcept {
        return unmet_requirements.empty();
    }
    friend bool operator==(const DogfoodQualificationReport&,
                           const DogfoodQualificationReport&) = default;
};

struct DogfoodArchiveMapEntry {
    std::string hardware_profile_id{};
    std::filesystem::path archive_path{};
    friend bool operator==(const DogfoodArchiveMapEntry&,
                           const DogfoodArchiveMapEntry&) = default;
};

struct DogfoodIncidentLocation {
    std::string incident_key{};
    std::string hardware_profile_id{};
    friend bool operator==(const DogfoodIncidentLocation&,
                           const DogfoodIncidentLocation&) = default;
};

struct DogfoodSessionArchiveEvidence {
    std::vector<std::string> incident_keys{};
    std::size_t automatic_captures{};
    friend bool operator==(const DogfoodSessionArchiveEvidence&,
                           const DogfoodSessionArchiveEvidence&) = default;
};

struct DogfoodCorpus {
    DogfoodManifest manifest{};
    std::vector<HardwareProfile> hardware_profiles{};
    std::vector<DogfoodSession> sessions{};
    std::vector<IncidentTruth> incidents{};
    std::vector<IncidentAnnotation> annotations{};
    friend bool operator==(const DogfoodCorpus&, const DogfoodCorpus&) = default;
};

enum class DogfoodCorpusErrorCode : std::uint8_t {
    io,
    invalid_manifest,
    invalid_header,
    invalid_value,
    duplicate_id,
    missing_reference,
    limit_exceeded,
    fingerprint_mismatch,
    incomplete_coverage,
    already_exists,
};

struct DogfoodCorpusError {
    DogfoodCorpusErrorCode code{DogfoodCorpusErrorCode::invalid_value};
    std::string message{};
    friend bool operator==(const DogfoodCorpusError&, const DogfoodCorpusError&) = default;
};

[[nodiscard]] std::expected<DogfoodCorpus, DogfoodCorpusError>
load_dogfood_corpus(const std::filesystem::path& directory) noexcept;

// Loads exactly one completed protocol-v1 annotation ballot. The expected
// incident key and session operator are supplied out of band so a ballot cannot
// be copied into the wrong review or authored by the collection operator.
[[nodiscard]] std::expected<IncidentAnnotation, DogfoodCorpusError>
load_dogfood_annotation_ballot(const std::filesystem::path& ballot_path,
                               std::string_view expected_incident_key,
                               std::string_view session_operator_id) noexcept;

// Loads and compares two independently completed ballots. The comparison emits
// only binding metadata and the disagreement bit; it never chooses consensus.
[[nodiscard]] std::expected<DogfoodBallotComparison, DogfoodCorpusError>
compare_dogfood_annotation_ballots(
    const std::filesystem::path& first_ballot_path,
    const std::filesystem::path& second_ballot_path,
    std::string_view expected_incident_key,
    std::string_view session_operator_id) noexcept;

[[nodiscard]] std::expected<DogfoodQualificationReport, DogfoodCorpusError>
assess_dogfood_qualification(const DogfoodCorpus& corpus) noexcept;

[[nodiscard]] std::expected<std::vector<DogfoodArchiveMapEntry>, DogfoodCorpusError>
load_dogfood_archive_map(const std::filesystem::path& path,
                         const DogfoodCorpus& corpus) noexcept;

[[nodiscard]] std::expected<void, DogfoodCorpusError>
validate_dogfood_incident_provenance(
    const DogfoodCorpus& corpus,
    std::span<const DogfoodIncidentLocation> locations,
    CorpusSplit split) noexcept;

[[nodiscard]] std::expected<void, DogfoodCorpusError>
initialize_dogfood_corpus(const std::filesystem::path& directory,
                          std::string corpus_id,
                          std::uint32_t pipeline_version,
                          std::uint64_t configuration_fingerprint) noexcept;

[[nodiscard]] std::expected<void, DogfoodCorpusError>
merge_dogfood_session_packet(
    const std::filesystem::path& base_corpus_directory,
    const std::filesystem::path& session_packet_directory,
    const DogfoodSessionArchiveEvidence& archive_evidence,
    const std::filesystem::path& output_corpus_directory) noexcept;

[[nodiscard]] std::expected<std::uint64_t, DogfoodCorpusError>
freeze_dogfood_corpus(const std::filesystem::path& directory,
                      const std::filesystem::path& excluded_incidents) noexcept;

[[nodiscard]] std::uint64_t dogfood_annotation_fingerprint(
    const DogfoodCorpus& corpus) noexcept;

[[nodiscard]] const char* to_string(CorpusSplit value) noexcept;
[[nodiscard]] const char* to_string(DogfoodSessionKind value) noexcept;
[[nodiscard]] const char* to_string(SymptomClass value) noexcept;
[[nodiscard]] const char* to_string(TruthCertainty value) noexcept;
[[nodiscard]] const char* to_string(UsefulnessRating value) noexcept;
[[nodiscard]] const char* to_string(analysis::IncidentType value) noexcept;
[[nodiscard]] const char* to_string(analysis::WorkloadContextKind value) noexcept;

} // namespace blackbox::evaluation
