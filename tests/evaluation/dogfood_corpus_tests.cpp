#include "evaluation/dogfood_corpus.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace evaluation = blackbox::evaluation;

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        path = std::filesystem::temp_directory_path() /
            ("blackbox-dogfood-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path{};
};

void append_complete_rows(const std::filesystem::path& path) {
    {
        std::ofstream output(path / "hardware.tsv", std::ios::app);
        output << "host-a\twindows\twin11-current\tamd-zen3\t12\t32-63\tamd\tbalanced\n";
        output << "host-b\twindows\twin11-current\tintel-12\t20\t16-31\tintel\tbalanced\n";
        output << "host-c\twindows\twin10-current\tamd-zen2\t8\t8-15\tnvidia\tperformance\n";
    }
    constexpr std::array symptoms{
        "cpu_starvation", "disk_stall", "network_interruption", "application_crash",
        "application_hang", "game_stutter", "audio_interruption", "quiet", "ambiguous"};
    constexpr std::array diagnoses{
        "cpu_pressure", "storage_pressure", "network_pressure", "application_crash",
        "application_hang", "unknown", "unknown", "unknown", "unknown"};
    constexpr std::array hosts{"host-a", "host-b", "host-c"};
    std::ofstream sessions(path / "sessions.tsv", std::ios::app);
    std::ofstream incidents(path / "incidents.tsv", std::ios::app);
    std::ofstream annotations(path / "annotations.tsv", std::ios::app);
    const auto key = [](const std::size_t value) {
        std::ostringstream output;
        output << std::hex << std::setw(32) << std::setfill('0') << value;
        return output.str();
    };
    const auto add_incident = [&](const std::size_t key_value,
                                  const std::string& session_id,
                                  const char* split,
                                  const char* symptom,
                                  const char* diagnosis,
                                  const bool visible = true) {
        const auto incident_key = key(key_value);
        const auto contributor = std::string_view{diagnosis} == "unknown" ? "" : "0";
        incidents << incident_key << '\t' << session_id << '\t' << split << '\t'
                  << symptom << "\tconfirmed\t" << (visible ? 1 : 0) << '\t'
                  << diagnosis << '\t' << contributor
                  << "\tunknown\t\t0\tuseful\t2\t0\n";
        for (const auto* annotator : {"annotator-1", "annotator-2"}) {
            annotations << incident_key << '\t' << annotator << '\t' << symptom
                        << "\tconfirmed\t" << (visible ? 1 : 0) << '\t'
                        << diagnosis << '\t' << contributor
                        << "\tunknown\t\tuseful\n";
        }
    };

    std::size_t next_key{1U};
    for (const auto* split : {"calibration", "held_out"}) {
        for (std::size_t index = 0U; index < symptoms.size(); ++index) {
            const auto session_id = std::string{split} + "-session-" +
                                    std::to_string(index);
            const auto quiet = std::string_view{symptoms[index]} == "quiet";
            const auto host_index = quiet ? 0U : index % hosts.size();
            const auto extra_incidents = index == 0U
                ? (std::string_view{split} == "calibration" ? 6U : 2U) : 0U;
            sessions << session_id << '\t' << hosts[host_index] << "\toperator-"
                     << static_cast<char>('a' + host_index) << '\t' << split << '\t'
                     << (quiet ? "quiet" : "natural") << '\t' << symptoms[index]
                     << '\t' << (quiet ? 6'000U : 60U) << '\t'
                     << 1U + extra_incidents << "\t0\t1\n";
            add_incident(next_key++, session_id, split, symptoms[index], diagnoses[index],
                         !quiet);
            for (std::size_t extra = 0U; extra < extra_incidents; ++extra) {
                add_incident(next_key++, session_id, split, symptoms[index], diagnoses[index]);
            }
        }
        for (std::size_t host_index = 1U; host_index < hosts.size(); ++host_index) {
            sessions << split << "-quiet-" << host_index << '\t' << hosts[host_index]
                     << "\toperator-" << static_cast<char>('a' + host_index) << '\t'
                     << split << "\tquiet\tquiet\t6000\t0\t0\t1\n";
        }
    }
}

void write_excluded_incidents(const std::filesystem::path& path,
                              const std::string& held_out_key = {}) {
    std::ofstream output(path, std::ios::trunc);
    output << "incident_key\tsession_id\tsplit\tsymptom\tcertainty\tuser_visible\t"
              "expected_diagnosis\texpected_contributor_ordinal\texpected_context\t"
              "recurrence_family\tdetector_should_capture\tusefulness\tannotator_count\t"
              "disagreement\n";
    if (!held_out_key.empty()) {
        output << held_out_key
               << "\tprior\theld_out\tquiet\tconfirmed\t0\tunknown\t\tunknown\t\t0\tunscored\t2\t0\n";
    }
}

void append_quiet_packet(const std::filesystem::path& path,
                         const std::string_view profile_id = "host-a",
                         const std::string_view gpu_family = "nvidia") {
    std::ofstream hardware(path / "hardware.tsv", std::ios::app);
    hardware << profile_id
             << "\twindows\twin11-current\tamd-zen3\t12\t32-63\t"
             << gpu_family << "\tbalanced\n";
    std::ofstream sessions(path / "sessions.tsv", std::ios::app);
    sessions << "quiet-session\t" << profile_id
             << "\toperator-a\tcalibration\tquiet\tquiet\t3600\t0\t0\t1\n";
}

void append_natural_packet(const std::filesystem::path& path) {
    constexpr std::string_view key{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    std::ofstream hardware(path / "hardware.tsv", std::ios::app);
    hardware << "host-a\twindows\twin11-current\tamd-zen3\t12\t32-63\t"
                "nvidia\tbalanced\n";
    std::ofstream sessions(path / "sessions.tsv", std::ios::app);
    sessions << "natural-session\thost-a\toperator-a\tcalibration\tnatural\t"
                "cpu_starvation\t60\t1\t1\t1\n";
    std::ofstream incidents(path / "incidents.tsv", std::ios::app);
    incidents << key
              << "\tnatural-session\tcalibration\tcpu_starvation\tconfirmed\t1\t"
                 "cpu_pressure\t0\tdesktop\tcompile-family\t1\tuseful\t2\t0\n";
    std::ofstream annotations(path / "annotations.tsv", std::ios::app);
    for (const auto* annotator : {"annotator-1", "annotator-2"}) {
        annotations << key << '\t' << annotator
                    << "\tcpu_starvation\tconfirmed\t1\tcpu_pressure\t0\tdesktop\t"
                       "compile-family\tuseful\n";
    }
}

} // namespace

TEST_CASE("dogfood protocol initializes only a new bounded corpus",
          "[evaluation][dogfood]") {
    TemporaryDirectory temporary;
    auto initialized = evaluation::initialize_dogfood_corpus(
        temporary.path, "local-v1", 2U, 12345U);
    REQUIRE(initialized.has_value());
    auto loaded = evaluation::load_dogfood_corpus(temporary.path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->manifest.corpus_id == "local-v1");
    CHECK_FALSE(loaded->manifest.frozen);
    CHECK(loaded->hardware_profiles.empty());
    CHECK(loaded->sessions.empty());
    CHECK(loaded->incidents.empty());
    CHECK(loaded->annotations.empty());
    CHECK(evaluation::initialize_dogfood_corpus(
              temporary.path, "second", 2U, 12345U)
              .error().code == evaluation::DogfoodCorpusErrorCode::already_exists);
}

TEST_CASE("dogfood protocol requires explicit consent for every collected session",
          "[evaluation][dogfood][consent][privacy]") {
    TemporaryDirectory temporary;
    REQUIRE(evaluation::initialize_dogfood_corpus(
        temporary.path, "consent-v1", 2U, 12345U));
    {
        std::ofstream hardware(temporary.path / "hardware.tsv", std::ios::app);
        hardware << "profile-a\twindows\twin11-current\tx64\t8\t16-31\tunknown\tbalanced\n";
        std::ofstream sessions(temporary.path / "sessions.tsv", std::ios::app);
        sessions << "unconsented-session\tprofile-a\toperator-a\tdevelopment\tquiet\t"
                    "quiet\t60\t0\t0\t0\n";
    }

    const auto loaded = evaluation::load_dogfood_corpus(temporary.path);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == evaluation::DogfoodCorpusErrorCode::invalid_value);
    CHECK(loaded.error().message.find("unconsented-session") != std::string::npos);
    CHECK(loaded.error().message.find("consent_attested=1") != std::string::npos);

    evaluation::DogfoodCorpus in_memory{};
    in_memory.manifest = {"consent-v1", false, 2U, 12345U, 0U};
    in_memory.hardware_profiles.push_back(
        {"profile-a", "windows", "win11-current", "x64", 8U,
         "16-31", "unknown", "balanced"});
    in_memory.sessions.push_back(
        {"unconsented-session", "profile-a", "operator-a",
         evaluation::CorpusSplit::development,
         evaluation::DogfoodSessionKind::quiet,
         evaluation::SymptomClass::quiet, 60.0, 0U, 0U, false});
    const auto readiness = evaluation::assess_dogfood_qualification(in_memory);
    REQUIRE_FALSE(readiness.has_value());
    CHECK(readiness.error().code == evaluation::DogfoodCorpusErrorCode::invalid_value);

    const auto unconsented_fingerprint =
        evaluation::dogfood_annotation_fingerprint(in_memory);
    in_memory.sessions.front().consent_attested = true;
    CHECK(evaluation::dogfood_annotation_fingerprint(in_memory) !=
          unconsented_fingerprint);

    TemporaryDirectory obsolete_header;
    REQUIRE(evaluation::initialize_dogfood_corpus(
        obsolete_header.path, "obsolete-v1", 2U, 12345U));
    {
        std::ofstream hardware(obsolete_header.path / "hardware.tsv", std::ios::app);
        hardware << "profile-a\twindows\twin11-current\tx64\t8\t16-31\tunknown\tbalanced\n";
        std::ofstream sessions(obsolete_header.path / "sessions.tsv", std::ios::trunc);
        sessions << "session_id\thardware_profile_id\toperator_id\tsplit\tkind\tsymptom\t"
                    "duration_seconds\texpected_incidents\tautomatic_captures\n"
                    "old-session\tprofile-a\toperator-a\tdevelopment\tquiet\tquiet\t"
                    "60\t0\t0\n";
    }
    const auto obsolete = evaluation::load_dogfood_corpus(obsolete_header.path);
    REQUIRE_FALSE(obsolete.has_value());
    CHECK(obsolete.error().code == evaluation::DogfoodCorpusErrorCode::invalid_header);
}

TEST_CASE("completed annotation ballot is exact bound and prediction free",
          "[evaluation][dogfood][annotation]") {
    TemporaryDirectory temporary;
    REQUIRE(std::filesystem::create_directories(temporary.path));
    const auto ballot_path = temporary.path / "completed-ballot.tsv";
    constexpr std::string_view key{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    {
        std::ofstream output{ballot_path, std::ios::binary};
        output << "incident_key\tannotator_id\tsymptom\tcertainty\tuser_visible\t"
                  "expected_diagnosis\texpected_contributor_ordinal\texpected_context\t"
                  "recurrence_family\tusefulness\n"
               << key << "\tannotator-1\tcpu_starvation\tprobable\t1\tcpu_pressure\t0\t"
                         "desktop\tcompile-family\tuseful\n";
    }
    const auto ballot = evaluation::load_dogfood_annotation_ballot(
        ballot_path, key, "operator-a");
    REQUIRE(ballot);
    CHECK(ballot->incident_key == key);
    CHECK(ballot->annotator_id == "annotator-1");
    CHECK(ballot->symptom == evaluation::SymptomClass::cpu_starvation);
    CHECK(ballot->certainty == evaluation::TruthCertainty::probable);
    CHECK(ballot->user_visible);
    CHECK(ballot->expected_diagnosis == blackbox::analysis::IncidentType::cpu_pressure);
    REQUIRE(ballot->expected_contributor_ordinal);
    CHECK(*ballot->expected_contributor_ordinal == 0U);
    CHECK(ballot->expected_context == blackbox::analysis::WorkloadContextKind::desktop);
    CHECK(ballot->recurrence_family == "compile-family");
    CHECK(ballot->usefulness == evaluation::UsefulnessRating::useful);

    const auto operator_ballot = evaluation::load_dogfood_annotation_ballot(
        ballot_path, key, "annotator-1");
    REQUIRE_FALSE(operator_ballot);
    CHECK(operator_ballot.error().message.find("invalid completed") != std::string::npos);

    const auto wrong_incident = evaluation::load_dogfood_annotation_ballot(
        ballot_path, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "operator-a");
    REQUIRE_FALSE(wrong_incident);
    CHECK(wrong_incident.error().message.find("binding") != std::string::npos);

    const auto second_path = temporary.path / "completed-ballot-2.tsv";
    const auto write_second = [&](const std::string_view annotator,
                                  const std::string_view diagnosis) {
        std::ofstream output{second_path, std::ios::binary};
        output << "incident_key\tannotator_id\tsymptom\tcertainty\tuser_visible\t"
                  "expected_diagnosis\texpected_contributor_ordinal\texpected_context\t"
                  "recurrence_family\tusefulness\n"
               << key << '\t' << annotator
               << "\tcpu_starvation\tprobable\t1\t" << diagnosis
               << "\t0\tdesktop\tcompile-family\tuseful\n";
    };
    write_second("annotator-2", "cpu_pressure");
    const auto agreement = evaluation::compare_dogfood_annotation_ballots(
        ballot_path, second_path, key, "operator-a");
    REQUIRE(agreement);
    CHECK(agreement->incident_key == key);
    CHECK(agreement->first_annotator_id == "annotator-1");
    CHECK(agreement->second_annotator_id == "annotator-2");
    CHECK_FALSE(agreement->disagreement);

    write_second("annotator-2", "storage_pressure");
    const auto disagreement = evaluation::compare_dogfood_annotation_ballots(
        ballot_path, second_path, key, "operator-a");
    REQUIRE(disagreement);
    CHECK(disagreement->disagreement);

    write_second("annotator-1", "cpu_pressure");
    const auto duplicate_annotator = evaluation::compare_dogfood_annotation_ballots(
        ballot_path, second_path, key, "operator-a");
    REQUIRE_FALSE(duplicate_annotator);
    CHECK(duplicate_annotator.error().code ==
          evaluation::DogfoodCorpusErrorCode::duplicate_id);

    std::ofstream{ballot_path, std::ios::app}
        << key << "\tannotator-2\tcpu_starvation\tprobable\t1\tcpu_pressure\t0\t"
                  "desktop\tcompile-family\tuseful\n";
    const auto multiple = evaluation::load_dogfood_annotation_ballot(
        ballot_path, key, "operator-a");
    REQUIRE_FALSE(multiple);
    CHECK(multiple.error().code == evaluation::DogfoodCorpusErrorCode::invalid_header);

    const auto directory_ballot = evaluation::load_dogfood_annotation_ballot(
        temporary.path, key, "operator-a");
    REQUIRE_FALSE(directory_ballot);
    CHECK(directory_ballot.error().message.find("regular file") != std::string::npos);

    const auto oversized_path = temporary.path / "oversized-ballot.tsv";
    {
        std::ofstream oversized{oversized_path, std::ios::binary};
        oversized << std::string(evaluation::maximum_annotation_ballot_bytes + 1U, 'x');
    }
    const auto oversized = evaluation::load_dogfood_annotation_ballot(
        oversized_path, key, "operator-a");
    REQUIRE_FALSE(oversized);
    CHECK(oversized.error().code == evaluation::DogfoodCorpusErrorCode::limit_exceeded);
}

TEST_CASE("protocol V1 accepts every diagnosis the current pipeline can emit",
          "[evaluation][dogfood][diagnosis][direct-v1]") {
    TemporaryDirectory temporary;
    REQUIRE(std::filesystem::create_directories(temporary.path));
    const auto ballot_path = temporary.path / "diagnosis-ballot.tsv";
    constexpr std::string_view key{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    constexpr std::array diagnoses{
        std::pair{"application_crash",
                  blackbox::analysis::IncidentType::application_crash},
        std::pair{"dns_resolution_timeout",
                  blackbox::analysis::IncidentType::dns_resolution_timeout},
        std::pair{"display_driver_recovery",
                  blackbox::analysis::IncidentType::display_driver_recovery},
        std::pair{"storage_io_retry",
                  blackbox::analysis::IncidentType::storage_io_retry},
    };
    for (const auto& [name, expected] : diagnoses) {
        std::ofstream output{ballot_path, std::ios::binary | std::ios::trunc};
        output << "incident_key\tannotator_id\tsymptom\tcertainty\tuser_visible\t"
                  "expected_diagnosis\texpected_contributor_ordinal\texpected_context\t"
                  "recurrence_family\tusefulness\n"
               << key << "\tannotator-1\tambiguous\tconfirmed\t1\t" << name
               << "\t\tdesktop\t\tuseful\n";
        output.close();
        const auto ballot = evaluation::load_dogfood_annotation_ballot(
            ballot_path, key, "operator-a");
        REQUIRE(ballot.has_value());
        CHECK(ballot->expected_diagnosis == expected);
        CHECK(std::string_view{evaluation::to_string(expected)} == name);
    }
}

TEST_CASE("single-session packets publish a new corpus without mutating the base",
          "[evaluation][dogfood][acquisition]") {
    TemporaryDirectory temporary;
    const auto base = temporary.path / "base";
    const auto packet = temporary.path / "packet";
    const auto output = temporary.path / "merged";
    REQUIRE(evaluation::initialize_dogfood_corpus(base, "campaign-v1", 5U, 123U));
    REQUIRE(evaluation::initialize_dogfood_corpus(packet, "campaign-v1", 5U, 123U));
    append_quiet_packet(packet);

    const evaluation::DogfoodSessionArchiveEvidence no_incidents{};
    REQUIRE(evaluation::merge_dogfood_session_packet(
        base, packet, no_incidents, output));
    const auto original = evaluation::load_dogfood_corpus(base);
    REQUIRE(original.has_value());
    CHECK(original->hardware_profiles.empty());
    CHECK(original->sessions.empty());
    const auto merged = evaluation::load_dogfood_corpus(output);
    REQUIRE(merged.has_value());
    REQUIRE(merged->hardware_profiles.size() == 1U);
    REQUIRE(merged->sessions.size() == 1U);
    CHECK(merged->sessions.front().session_id == "quiet-session");
    CHECK_FALSE(std::filesystem::exists(output.string() + ".partial"));

    const auto occupied = evaluation::merge_dogfood_session_packet(
        base, packet, no_incidents, output);
    REQUIRE_FALSE(occupied.has_value());
    CHECK(occupied.error().code == evaluation::DogfoodCorpusErrorCode::already_exists);

    std::ofstream(packet / "operator-notes.txt") << "must not enter the corpus";
    const auto extra_file = evaluation::merge_dogfood_session_packet(
        base, packet, no_incidents, temporary.path / "extra-file-output");
    REQUIRE_FALSE(extra_file.has_value());
    CHECK(extra_file.error().message.find("exactly five") != std::string::npos);
}

TEST_CASE("session packet merge binds archive keys captures and hardware identity",
          "[evaluation][dogfood][acquisition][provenance]") {
    TemporaryDirectory temporary;
    const auto base = temporary.path / "base";
    const auto first_packet = temporary.path / "first-packet";
    const auto first_output = temporary.path / "first-output";
    REQUIRE(evaluation::initialize_dogfood_corpus(base, "campaign-v1", 5U, 456U));
    REQUIRE(evaluation::initialize_dogfood_corpus(
        first_packet, "campaign-v1", 5U, 456U));
    append_quiet_packet(first_packet);
    REQUIRE(evaluation::merge_dogfood_session_packet(
        base, first_packet, {}, first_output));

    const auto natural_packet = temporary.path / "natural-packet";
    REQUIRE(evaluation::initialize_dogfood_corpus(
        natural_packet, "campaign-v1", 5U, 456U));
    append_natural_packet(natural_packet);
    constexpr std::string_view key{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    evaluation::DogfoodSessionArchiveEvidence evidence{{std::string{key}}, 1U};

    auto wrong_key = evidence;
    wrong_key.incident_keys.front() = std::string(32U, 'b');
    const auto key_rejected = evaluation::merge_dogfood_session_packet(
        first_output, natural_packet, wrong_key, temporary.path / "wrong-key");
    REQUIRE_FALSE(key_rejected.has_value());
    CHECK(key_rejected.error().message.find("archive evidence") != std::string::npos);

    auto wrong_capture_count = evidence;
    wrong_capture_count.automatic_captures = 0U;
    const auto capture_rejected = evaluation::merge_dogfood_session_packet(
        first_output, natural_packet, wrong_capture_count,
        temporary.path / "wrong-capture");
    REQUIRE_FALSE(capture_rejected.has_value());

    const auto second_output = temporary.path / "second-output";
    REQUIRE(evaluation::merge_dogfood_session_packet(
        first_output, natural_packet, evidence, second_output));
    const auto merged = evaluation::load_dogfood_corpus(second_output);
    REQUIRE(merged.has_value());
    CHECK(merged->hardware_profiles.size() == 1U);
    CHECK(merged->sessions.size() == 2U);
    CHECK(merged->incidents.size() == 1U);
    CHECK(merged->annotations.size() == 2U);

    const auto redefined_packet = temporary.path / "redefined-packet";
    REQUIRE(evaluation::initialize_dogfood_corpus(
        redefined_packet, "campaign-v1", 5U, 456U));
    append_quiet_packet(redefined_packet, "host-a", "intel");
    const auto redefined = evaluation::merge_dogfood_session_packet(
        first_output, redefined_packet, {}, temporary.path / "redefined-output");
    REQUIRE_FALSE(redefined.has_value());
    CHECK(redefined.error().message.find("redefines") != std::string::npos);

    const auto duplicate = evaluation::merge_dogfood_session_packet(
        first_output, first_packet, {}, temporary.path / "duplicate-output");
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error().code == evaluation::DogfoodCorpusErrorCode::duplicate_id);
}

TEST_CASE("dogfood corpus freezes complete truth and detects later tampering",
          "[evaluation][dogfood]") {
    TemporaryDirectory temporary;
    REQUIRE(evaluation::initialize_dogfood_corpus(
        temporary.path, "complete-v1", 2U, 54321U));
    append_complete_rows(temporary.path);
    auto collecting = evaluation::load_dogfood_corpus(temporary.path);
    REQUIRE(collecting.has_value());
    REQUIRE(collecting->incidents.size() == 26U);
    REQUIRE(collecting->annotations.size() == 52U);
    auto readiness = evaluation::assess_dogfood_qualification(*collecting);
    REQUIRE(readiness.has_value());
    CHECK(readiness->ready_to_freeze());
    CHECK(readiness->calibration_symptom_coverage.size() ==
          evaluation::dogfood_symptom_class_count);
    CHECK(readiness->calibration_symptom_coverage[
          static_cast<std::size_t>(evaluation::SymptomClass::application_crash)]);
    CHECK(readiness->held_out_symptom_coverage[
          static_cast<std::size_t>(evaluation::SymptomClass::application_crash)]);
    CHECK(readiness->fully_qualified_hardware_profiles == 3U);
    CHECK(readiness->calibration_natural_sessions == 8U);
    CHECK(readiness->held_out_natural_sessions == 8U);
    CHECK(readiness->quiet_exposure_seconds == 36'000.0);
    const auto expected_fingerprint =
        evaluation::dogfood_annotation_fingerprint(*collecting);
    const auto exclusions = temporary.path / "excluded-incidents.tsv";
    write_excluded_incidents(exclusions);
    auto frozen = evaluation::freeze_dogfood_corpus(temporary.path, exclusions);
    REQUIRE(frozen.has_value());
    CHECK(*frozen == expected_fingerprint);
    auto reloaded = evaluation::load_dogfood_corpus(temporary.path);
    REQUIRE(reloaded.has_value());
    CHECK(reloaded->manifest.frozen);
    CHECK(reloaded->manifest.annotation_fingerprint == expected_fingerprint);

    std::ofstream tamper(temporary.path / "sessions.tsv", std::ios::app);
    tamper << "extra\thost-a\toperator-a\theld_out\tquiet\tquiet\t60\t0\t0\t1\n";
    tamper.close();
    auto rejected = evaluation::load_dogfood_corpus(temporary.path);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code ==
          evaluation::DogfoodCorpusErrorCode::fingerprint_mismatch);
}

TEST_CASE("dogfood freeze refuses missing symptom and held-out coverage",
          "[evaluation][dogfood]") {
    TemporaryDirectory temporary;
    REQUIRE(evaluation::initialize_dogfood_corpus(
        temporary.path, "incomplete-v1", 2U, 111U));
    {
        std::ofstream hardware(temporary.path / "hardware.tsv", std::ios::app);
        hardware << "host-a\twindows\twin11\tx64\t8\t16-31\tunknown\tbalanced\n";
        std::ofstream sessions(temporary.path / "sessions.tsv", std::ios::app);
        sessions << "only\thost-a\toperator-a\tcalibration\tcontrolled\t"
                    "cpu_starvation\t60\t1\t0\t1\n";
        std::ofstream incidents(temporary.path / "incidents.tsv", std::ios::app);
        incidents << std::string(32U, 'a')
                  << "\tonly\tcalibration\tcpu_starvation\tconfirmed\t1\t"
                     "cpu_pressure\t0\tunknown\t\t1\tuseful\t1\t0\n";
        std::ofstream annotations(temporary.path / "annotations.tsv", std::ios::app);
        annotations << std::string(32U, 'a')
                    << "\tannotator-1\tcpu_starvation\tconfirmed\t1\t"
                       "cpu_pressure\t0\tunknown\t\tuseful\n";
    }
    const auto exclusions = temporary.path / "excluded-incidents.tsv";
    write_excluded_incidents(exclusions);
    auto frozen = evaluation::freeze_dogfood_corpus(temporary.path, exclusions);
    REQUIRE_FALSE(frozen.has_value());
    CHECK(frozen.error().code ==
          evaluation::DogfoodCorpusErrorCode::incomplete_coverage);
}

TEST_CASE("qualification report requires each evidence profile in both frozen splits",
          "[evaluation][dogfood][readiness]") {
    TemporaryDirectory temporary;
    REQUIRE(evaluation::initialize_dogfood_corpus(
        temporary.path, "distribution-v1", 5U, 444U));
    append_complete_rows(temporary.path);
    auto corpus = evaluation::load_dogfood_corpus(temporary.path);
    REQUIRE(corpus.has_value());
    auto& held_out_quiet = *std::find_if(
        corpus->sessions.begin(), corpus->sessions.end(), [](const auto& session) {
            return session.session_id == "held_out-quiet-2";
        });
    held_out_quiet.hardware_profile_id = "host-a";

    auto readiness = evaluation::assess_dogfood_qualification(*corpus);
    REQUIRE(readiness.has_value());
    CHECK_FALSE(readiness->ready_to_freeze());
    CHECK(readiness->represented_hardware_profiles == 3U);
    CHECK(readiness->fully_qualified_hardware_profiles == 2U);
    const auto host_c = std::find_if(
        readiness->hardware_qualification.begin(),
        readiness->hardware_qualification.end(), [](const auto& profile) {
            return profile.profile_id == "host-c";
        });
    REQUIRE(host_c != readiness->hardware_qualification.end());
    CHECK_FALSE(host_c->fully_qualified());
    CHECK(host_c->held_out_quiet_seconds == 0.0);
    REQUIRE_FALSE(readiness->unmet_requirements.empty());
    CHECK(readiness->unmet_requirements.front().find("three hardware profiles") !=
          std::string::npos);
}

TEST_CASE("annotation ballots prove distinct non-operator annotation",
          "[evaluation][dogfood][annotation]") {
    TemporaryDirectory temporary;
    REQUIRE(evaluation::initialize_dogfood_corpus(
        temporary.path, "annotation-v1", 5U, 555U));
    append_complete_rows(temporary.path);
    auto corpus = evaluation::load_dogfood_corpus(temporary.path);
    REQUIRE(corpus.has_value());
    corpus->annotations.front().annotator_id = "operator-a";

    auto readiness = evaluation::assess_dogfood_qualification(*corpus);
    REQUIRE_FALSE(readiness.has_value());
    CHECK(readiness.error().message.find("operator") != std::string::npos);
    CHECK(readiness.error().message.find("operator-a") != std::string::npos);
    CHECK(readiness.error().message.find(corpus->annotations.front().incident_key) !=
          std::string::npos);
}

TEST_CASE("campaign validation errors identify the exact safe row and mismatch",
          "[evaluation][dogfood][annotation][diagnostics]") {
    TemporaryDirectory temporary;
    REQUIRE(evaluation::initialize_dogfood_corpus(
        temporary.path, "diagnostics-v1", 5U, 556U));
    append_complete_rows(temporary.path);
    const auto complete = evaluation::load_dogfood_corpus(temporary.path);
    REQUIRE(complete);

    auto missing_ballot = *complete;
    const auto missing_key = missing_ballot.annotations.back().incident_key;
    missing_ballot.annotations.pop_back();
    const auto count_result = evaluation::assess_dogfood_qualification(missing_ballot);
    REQUIRE_FALSE(count_result);
    CHECK(count_result.error().message.find(missing_key) != std::string::npos);
    CHECK(count_result.error().message.find("declares 2 ballots but contains 1") !=
          std::string::npos);

    auto wrong_disagreement = *complete;
    wrong_disagreement.incidents.front().disagreement = true;
    const auto disagreement_result =
        evaluation::assess_dogfood_qualification(wrong_disagreement);
    REQUIRE_FALSE(disagreement_result);
    CHECK(disagreement_result.error().message.find(
              wrong_disagreement.incidents.front().incident_key) != std::string::npos);
    CHECK(disagreement_result.error().message.find(
              "declares disagreement=1 but ballots require disagreement=0") !=
          std::string::npos);

    auto wrong_session_count = *complete;
    ++wrong_session_count.sessions.front().expected_incidents;
    const auto session_result =
        evaluation::assess_dogfood_qualification(wrong_session_count);
    REQUIRE_FALSE(session_result);
    CHECK(session_result.error().message.find(
              wrong_session_count.sessions.front().session_id) != std::string::npos);
    CHECK(session_result.error().message.find("declares 8 incidents but contains 7") !=
          std::string::npos);

    auto duplicate_ballot = *complete;
    REQUIRE(duplicate_ballot.annotations.size() >= 2U);
    REQUIRE(duplicate_ballot.annotations[0U].incident_key ==
            duplicate_ballot.annotations[1U].incident_key);
    duplicate_ballot.annotations[1U].annotator_id =
        duplicate_ballot.annotations[0U].annotator_id;
    const auto duplicate_result =
        evaluation::assess_dogfood_qualification(duplicate_ballot);
    REQUIRE_FALSE(duplicate_result);
    CHECK(duplicate_result.error().message.find(
              duplicate_ballot.annotations[0U].incident_key) != std::string::npos);
    CHECK(duplicate_result.error().message.find(
              duplicate_ballot.annotations[0U].annotator_id) != std::string::npos);
}

TEST_CASE("archive map binds one local archive to every represented profile",
          "[evaluation][dogfood][archive-map]") {
    TemporaryDirectory temporary;
    REQUIRE(evaluation::initialize_dogfood_corpus(
        temporary.path, "archive-map-v1", 5U, 666U));
    append_complete_rows(temporary.path);
    auto corpus = evaluation::load_dogfood_corpus(temporary.path);
    REQUIRE(corpus.has_value());
    const auto map_path = temporary.path / "archive-map.tsv";
    const auto archive_directory = temporary.path / "archives";
    REQUIRE(std::filesystem::create_directory(archive_directory));
    for (const auto* name : {"a.sqlite3", "b.sqlite3", "c.sqlite3",
                             "shared.sqlite3"}) {
        std::ofstream archive(archive_directory / name, std::ios::binary);
        REQUIRE(archive.good());
    }
    {
        std::ofstream map(map_path);
        map << "hardware_profile_id\tarchive_path\n"
               "host-a\tarchives/a.sqlite3\n"
               "host-b\tarchives/b.sqlite3\n"
               "host-c\tarchives/c.sqlite3\n";
    }
    auto mapped = evaluation::load_dogfood_archive_map(map_path, *corpus);
    REQUIRE(mapped.has_value());
    REQUIRE(mapped->size() == 3U);
    CHECK(mapped->front().archive_path.is_absolute());
    CHECK(mapped->front().archive_path.filename() == "a.sqlite3");

    {
        std::ofstream map(map_path, std::ios::trunc);
        map << "hardware_profile_id\tarchive_path\n"
               "host-a\tarchives/shared.sqlite3\n"
               "host-b\tarchives/shared.sqlite3\n"
               "host-c\tarchives/c.sqlite3\n";
    }
    mapped = evaluation::load_dogfood_archive_map(map_path, *corpus);
    REQUIRE_FALSE(mapped.has_value());
    CHECK(mapped.error().code == evaluation::DogfoodCorpusErrorCode::duplicate_id);

    {
        std::ofstream map(map_path, std::ios::trunc);
        map << "hardware_profile_id\tarchive_path\n"
               "host-a\tarchives/a.sqlite3\n"
               "host-b\tarchives/b.sqlite3\n";
    }
    mapped = evaluation::load_dogfood_archive_map(map_path, *corpus);
    REQUIRE_FALSE(mapped.has_value());
    CHECK(mapped.error().code == evaluation::DogfoodCorpusErrorCode::missing_reference);

    std::map<std::string, std::string> profile_by_session;
    for (const auto& session : corpus->sessions) {
        profile_by_session.emplace(session.session_id, session.hardware_profile_id);
    }
    std::vector<evaluation::DogfoodIncidentLocation> locations;
    for (const auto& incident : corpus->incidents) {
        if (incident.split == evaluation::CorpusSplit::held_out) {
            locations.push_back({incident.incident_key,
                                 profile_by_session[incident.session_id]});
        }
    }
    CHECK(evaluation::validate_dogfood_incident_provenance(
        *corpus, locations, evaluation::CorpusSplit::held_out));
    const auto original_profile = locations.front().hardware_profile_id;
    locations.front().hardware_profile_id =
        locations.front().hardware_profile_id == "host-a" ? "host-b" : "host-a";
    auto provenance = evaluation::validate_dogfood_incident_provenance(
        *corpus, locations, evaluation::CorpusSplit::held_out);
    REQUIRE_FALSE(provenance.has_value());
    CHECK(provenance.error().code == evaluation::DogfoodCorpusErrorCode::invalid_value);

    locations.front().hardware_profile_id = original_profile;
    locations.pop_back();
    provenance = evaluation::validate_dogfood_incident_provenance(
        *corpus, locations, evaluation::CorpusSplit::held_out);
    REQUIRE_FALSE(provenance.has_value());
    CHECK(provenance.error().code == evaluation::DogfoodCorpusErrorCode::missing_reference);
}

TEST_CASE("dogfood freeze refuses every reused excluded held-out incident",
          "[evaluation][dogfood][held-out][leakage]") {
    TemporaryDirectory temporary;
    REQUIRE(evaluation::initialize_dogfood_corpus(
        temporary.path, "fresh-v1", 5U, 333U));
    append_complete_rows(temporary.path);
    const auto reused_key = std::string(31U, '0') + "1";
    const auto exclusions = temporary.path / "v015-heldout.tsv";
    write_excluded_incidents(exclusions, reused_key);

    const auto frozen = evaluation::freeze_dogfood_corpus(
        temporary.path, exclusions);
    REQUIRE_FALSE(frozen.has_value());
    CHECK(frozen.error().message.find("reuses") != std::string::npos);
}

TEST_CASE("dogfood protocol rejects unknown columns and dangling references",
          "[evaluation][dogfood]") {
    TemporaryDirectory temporary;
    REQUIRE(evaluation::initialize_dogfood_corpus(
        temporary.path, "invalid-v1", 2U, 222U));
    {
        std::ofstream sessions(temporary.path / "sessions.tsv", std::ios::app);
        sessions << "dangling\tmissing\toperator-a\tdevelopment\tquiet\tquiet\t10\t0\t0\t1\n";
    }
    auto loaded = evaluation::load_dogfood_corpus(temporary.path);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code ==
          evaluation::DogfoodCorpusErrorCode::missing_reference);
}
