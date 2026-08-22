if(NOT DEFINED SOURCE_ROOT OR NOT IS_DIRECTORY "${SOURCE_ROOT}/src")
    message(FATAL_ERROR "SOURCE_ROOT must identify the BlackBox source tree")
endif()

function(require_literal relative_path literal description)
    set(path "${SOURCE_ROOT}/${relative_path}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Direct-v1 contract is missing ${relative_path}")
    endif()
    file(READ "${path}" contents)
    string(FIND "${contents}" "${literal}" offset)
    if(offset EQUAL -1)
        message(FATAL_ERROR
            "Direct-v1 contract failed: ${description} is not fixed to version 1 in ${relative_path}")
    endif()
endfunction()

require_literal("src/storage/incident_archive.hpp"
    "inline constexpr std::int32_t current_schema_version = 1"
    "SQLite archive schema")
require_literal("src/storage/archive_schema.hpp"
    "PRAGMA user_version=1;"
    "SQLite schema publication")
require_literal("src/storage/incident_dataset.hpp"
    "inline constexpr std::uint32_t incident_dataset_format_version = 1U"
    "offline dataset")
require_literal("src/app/product_settings.hpp"
    "inline constexpr std::uint32_t product_settings_format_version = 1U"
    "product settings")
require_literal("src/app/recorder_settings.hpp"
    "inline constexpr std::uint32_t recorder_settings_format_version = 1U"
    "recorder settings")
require_literal("src/app/support_bundle.hpp"
    "inline constexpr std::uint32_t support_bundle_format_version = 1U"
    "support bundle")
require_literal("src/app/wall_clock_report.hpp"
    "inline constexpr std::uint32_t wall_clock_report_format_version = 1U"
    "wall-clock report")
require_literal("src/evaluation/dogfood_corpus.hpp"
    "inline constexpr std::uint32_t dogfood_protocol_version = 1U"
    "dogfood corpus")
require_literal("src/evaluation/dogfood_corpus.cpp"
    "expected_incidents\\tautomatic_captures\\tconsent_attested"
    "dogfood session consent attestation")
require_literal("src/evaluation/evaluation_run_transaction.hpp"
    "inline constexpr std::uint32_t evaluation_transaction_format_version = 1U"
    "evaluation transaction")
require_literal("src/evaluation/diagnostic_evaluation.hpp"
    "inline constexpr std::uint32_t diagnostic_evaluation_report_format_version = 1U"
    "diagnostic evaluation report")
require_literal("src/evaluation/diagnostic_evaluation.hpp"
    "inline constexpr std::uint32_t confidence_calibration_artifact_format_version = 1U"
    "confidence calibration artifact")
require_literal("src/evaluation/truth_review.hpp"
    "inline constexpr std::uint32_t truth_review_format_version = 1U"
    "truth review")
require_literal("src/evaluation/campaign_status.hpp"
    "inline constexpr std::uint32_t campaign_status_format_version = 1U"
    "campaign status")
require_literal("src/evaluation/dogfood_corpus.cpp"
    "supported_diagnosis_recall,supported_diagnosis_precision"
    "held-out supported-recall metric")
require_literal("src/evaluation/diagnostic_evaluation_artifact.cpp"
    "write_rate(output, \"miss_rate\""
    "held-out miss-rate numerator and denominator")
require_literal("src/evaluation/diagnostic_evaluation_artifact.cpp"
    "\\\"symptom_counts\\\""
    "held-out symptom coverage")
require_literal("src/evaluation/diagnostic_evaluation_artifact.cpp"
    "\\\"hardware_distribution\\\""
    "held-out hardware distribution")
require_literal("src/evaluation/diagnostic_evaluation_artifact.cpp"
    "\\\"exposure_hours\\\""
    "held-out quiet exposure denominator")
require_literal("src/evaluation/diagnostic_evaluation_artifact.cpp"
    "\\\"bins\\\""
    "held-out calibration-bin counts")
require_literal("src/evaluation/diagnostic_evaluation_artifact.cpp"
    "verify_diagnostic_evaluation_artifact"
    "independent held-out artifact verifier")
require_literal("src/evaluation/diagnostic_evaluation_artifact.cpp"
    "\\\"calibration_artifact_fingerprint\\\""
    "held-out calibration artifact provenance")
require_literal("src/evaluation/confidence_calibration_artifact.cpp"
    "std::filesystem::symlink_status"
    "non-link confidence calibration input")
require_literal("src/evaluation/confidence_calibration_artifact.cpp"
    "ConfidenceCalibrationArtifactErrorCode::noncanonical"
    "canonical confidence calibration input")
require_literal("src/evaluation/confidence_calibration_artifact.cpp"
    "serialize_confidence_calibration_artifact"
    "single confidence calibration serializer")
require_literal("src/app/dogfood_tool.cpp"
    "Published evaluation output failed independent direct-V1"
    "post-publication evaluation verification")
require_literal("src/app/dogfood_tool.cpp"
    "Published calibration output failed direct-V1"
    "post-publication calibration verification")
require_literal("src/app/dogfood_tool.cpp"
    "verify-evaluation"
    "standalone evaluation verification command")
require_literal("src/app/dogfood_tool.cpp"
    "report_artifact_fingerprint=\" << *report_fingerprint"
    "standalone report fingerprint recomputation")

file(READ "${SOURCE_ROOT}/src/storage/archive_schema.hpp" archive_schema)
string(REGEX MATCHALL "PRAGMA user_version=[0-9]+" schema_version_writes "${archive_schema}")
list(LENGTH schema_version_writes schema_version_write_count)
if(NOT schema_version_write_count EQUAL 1 OR
   NOT "${schema_version_writes}" STREQUAL "PRAGMA user_version=1")
    message(FATAL_ERROR
        "Direct-v1 contract failed: the archive schema must contain exactly one version-1 publication")
endif()

file(GLOB_RECURSE production_sources
    "${SOURCE_ROOT}/src/*.cpp"
    "${SOURCE_ROOT}/src/*.h"
    "${SOURCE_ROOT}/src/*.hpp")
set(forbidden_compatibility_tokens
    "migration"
    "migrate_"
    "legacy_reader"
    "compatibility_reader"
    "archive_schema_v2"
    "archive_schema_v3")
foreach(path IN LISTS production_sources)
    file(READ "${path}" contents)
    string(TOLOWER "${contents}" lower_contents)
    foreach(token IN LISTS forbidden_compatibility_tokens)
        string(FIND "${lower_contents}" "${token}" offset)
        if(NOT offset EQUAL -1)
            file(RELATIVE_PATH relative_path "${SOURCE_ROOT}" "${path}")
            message(FATAL_ERROR
                "Direct-v1 contract failed: forbidden compatibility token '${token}' in ${relative_path}")
        endif()
    endforeach()
endforeach()

file(GLOB direct_v1_documents
    "${SOURCE_ROOT}/*.md"
    "${SOURCE_ROOT}/docs/*.md")
foreach(path IN LISTS direct_v1_documents)
    file(READ "${path}" contents)
    string(TOLOWER "${contents}" lower_contents)
    string(REGEX MATCH
        "(schema|dataset)[ -]?v[2-9][0-9]*|(schema|dataset)[ -]+version[ -]+[2-9][0-9]*"
        obsolete_persisted_version "${lower_contents}")
    if(NOT "${obsolete_persisted_version}" STREQUAL "")
        file(RELATIVE_PATH relative_path "${SOURCE_ROOT}" "${path}")
        message(FATAL_ERROR
            "Direct-v1 contract failed: obsolete persisted version '${obsolete_persisted_version}' in ${relative_path}")
    endif()
endforeach()

foreach(path IN LISTS production_sources)
    file(READ "${path}" contents)
    string(FIND "${contents}" "diagnosis_accuracy" old_metric_offset)
    if(NOT old_metric_offset EQUAL -1)
        file(RELATIVE_PATH relative_path "${SOURCE_ROOT}" "${path}")
        message(FATAL_ERROR
            "Direct-v1 contract failed: obsolete diagnosis_accuracy alias in ${relative_path}")
    endif()
endforeach()

message(STATUS
    "Direct-v1 contract verified: schema=1 persisted_formats=11 migration_paths=0 compatibility_readers=0")
