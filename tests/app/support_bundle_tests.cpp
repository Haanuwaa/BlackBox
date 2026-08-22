#include "app/support_bundle.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace app = blackbox::app;

namespace {

class TemporaryBundle final {
public:
    TemporaryBundle() {
        static std::atomic<std::uint64_t> sequence{};
        root = std::filesystem::temp_directory_path() /
               ("blackbox-support-bundle-test-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()) +
                "-" + std::to_string(sequence.fetch_add(1U)));
        std::filesystem::create_directories(root);
    }
    ~TemporaryBundle() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root{};
};

[[nodiscard]] app::SupportBundleRequest request_for(
    const std::filesystem::path& destination) {
    app::SupportBundleRequest request{};
    request.destination = destination;
    request.diagnostics.application_version = "0.17-test";
    request.diagnostics.platform = "Windows";
    request.diagnostics.collector_running = true;
    request.diagnostics.process_lifecycle_enabled = true;
    request.diagnostics.process_lifecycle_observations = 9U;
    request.diagnostics.process_lifecycle_events_recorded = 7U;
    request.diagnostics.collections = 41U;
    request.diagnostics.archive_healthy = true;
    request.diagnostics.archive_schema_version = 1;
    request.diagnostics.stored_incidents = 7U;
    request.diagnostics.previous_crash_dumps = 2U;
    return request;
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("support bundle publishes a privacy-safe direct-v1 directory atomically",
          "[app][support][privacy][transaction]") {
    TemporaryBundle temporary;
    const auto destination = temporary.root / "bundle";
    auto request = request_for(destination);

    const auto result = app::create_support_bundle(request);
    REQUIRE(result.has_value());
    CHECK(result->files == 3U);
    CHECK_FALSE(result->included_crash_dump);
    CHECK(std::filesystem::is_directory(destination));
    CHECK_FALSE(std::filesystem::exists(destination.string() + ".partial"));
    CHECK(std::filesystem::is_regular_file(destination / "manifest.ini"));
    CHECK(std::filesystem::is_regular_file(destination / "diagnostics.ini"));
    CHECK(std::filesystem::is_regular_file(destination / "README.txt"));
    CHECK_FALSE(std::filesystem::exists(destination / "crash.dmp"));

    const auto manifest = read_text(destination / "manifest.ini");
    const auto diagnostics = read_text(destination / "diagnostics.ini");
    const auto readme = read_text(destination / "README.txt");
    CHECK(manifest.find("format=1\n") == 0U);
    CHECK(manifest.find("includes_crash_dump=0") != std::string::npos);
    CHECK(diagnostics.find("format=1\n") == 0U);
    CHECK(diagnostics.find("collections=41") != std::string::npos);
    CHECK(diagnostics.find("process_lifecycle_enabled=1") != std::string::npos);
    CHECK(diagnostics.find("process_lifecycle_observations=9") != std::string::npos);
    CHECK(diagnostics.find("process_lifecycle_events_recorded=7") !=
          std::string::npos);
    CHECK(diagnostics.find("archive_schema_version=1") != std::string::npos);
    CHECK(diagnostics.find(temporary.root.string()) == std::string::npos);
    CHECK(readme.find("never uploaded automatically") != std::string::npos);
    CHECK(readme.find("excludes the") != std::string::npos);

    const auto repeated = app::create_support_bundle(request);
    REQUIRE_FALSE(repeated.has_value());
    CHECK(repeated.error().code == app::SupportBundleErrorCode::destination_exists);
}

TEST_CASE("support bundle includes only an explicitly consented bounded crash dump",
          "[app][support][crash][privacy]") {
    TemporaryBundle temporary;
    const auto crash = temporary.root / "private-source-name.dmp";
    {
        std::ofstream output{crash, std::ios::binary};
        output << "MDMP\0bounded fixture";
    }
    auto request = request_for(temporary.root / "with-crash");
    request.consented_crash_dump = crash;
    request.crash_dump_disclosure_confirmed = true;

    const auto result = app::create_support_bundle(request);
    REQUIRE(result.has_value());
    CHECK(result->files == 4U);
    CHECK(result->included_crash_dump);
    CHECK(read_text(result->destination / "crash.dmp") == read_text(crash));
    const auto manifest = read_text(result->destination / "manifest.ini");
    CHECK(manifest.find("includes_crash_dump=1") != std::string::npos);
    CHECK(manifest.find("crash_dump_fingerprint=") != std::string::npos);
    CHECK(manifest.find("private-source-name") == std::string::npos);
    CHECK(read_text(result->destination / "diagnostics.ini").find(
              temporary.root.string()) == std::string::npos);

    auto missing = request_for(temporary.root / "missing-crash");
    missing.consented_crash_dump = temporary.root / "missing.dmp";
    missing.crash_dump_disclosure_confirmed = true;
    const auto rejected = app::create_support_bundle(missing);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == app::SupportBundleErrorCode::crash_dump_invalid);
    CHECK_FALSE(std::filesystem::exists(missing.destination));
    CHECK_FALSE(std::filesystem::exists(missing.destination.string() + ".partial"));

    auto unconfirmed = request_for(temporary.root / "unconfirmed-crash");
    unconfirmed.consented_crash_dump = crash;
    const auto consent_rejected = app::create_support_bundle(unconfirmed);
    REQUIRE_FALSE(consent_rejected.has_value());
    CHECK(consent_rejected.error().code ==
          app::SupportBundleErrorCode::invalid_request);
    CHECK_FALSE(std::filesystem::exists(unconfirmed.destination));
}

TEST_CASE("support bundle refuses unsafe fields and occupied staging",
          "[app][support][validation]") {
    TemporaryBundle temporary;
    auto unsafe = request_for(temporary.root / "unsafe");
    unsafe.diagnostics.platform = "Windows\narchive_path=secret";
    const auto rejected = app::create_support_bundle(unsafe);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == app::SupportBundleErrorCode::invalid_request);

    auto occupied = request_for(temporary.root / "occupied");
    std::filesystem::create_directory(occupied.destination.string() + ".partial");
    const auto collision = app::create_support_bundle(occupied);
    REQUIRE_FALSE(collision.has_value());
    CHECK(collision.error().code == app::SupportBundleErrorCode::staging_exists);
}

TEST_CASE("support bundle service executes one bounded background request",
          "[app][support][worker]") {
    TemporaryBundle temporary;
    app::SupportBundleService service;
    service.start();
    service.create(request_for(temporary.root / "asynchronous"));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (service.snapshot()->busy && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const auto state = service.snapshot();
    CHECK_FALSE(state->busy);
    CHECK(state->status.find("created locally") != std::string::npos);
    CHECK(std::filesystem::is_directory(temporary.root / "asynchronous"));
    service.stop();
    CHECK_FALSE(service.snapshot()->running);
}
