#include "storage/incident_archive.hpp"
#include "storage/test_incident.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace storage = blackbox::storage;

namespace {

[[nodiscard]] bool store_fixture(storage::SqliteIncidentArchive& archive,
                                 const blackbox::core::IncidentSnapshot& incident,
                                 const std::string& label,
                                 const std::string& note) {
    const auto stored = archive.store(incident);
    if (!stored.has_value()) {
        std::cerr << "store failed: " << stored.error().message << '\n';
        return false;
    }
    const auto annotated = archive.update_annotation(*stored, {label, note});
    if (!annotated.has_value()) {
        std::cerr << "annotation failed: " << annotated.error().message << '\n';
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const auto output = std::filesystem::absolute(
        argc > 1 ? std::filesystem::path{argv[1]}
                 : std::filesystem::path{"representative-incidents.sqlite3"});
    std::error_code filesystem_error;
    std::filesystem::remove(output, filesystem_error);
    std::filesystem::remove(output.string() + "-wal", filesystem_error);
    std::filesystem::remove(output.string() + "-shm", filesystem_error);

    storage::SqliteIncidentArchive archive{{output}};
    const auto opened = archive.open();
    if (!opened.has_value()) {
        std::cerr << "open failed: " << opened.error().message << '\n';
        return 1;
    }

    if (!store_fixture(archive, *storage::test::representative_incident(),
                       "Unavailable metrics",
                       "Exercises unsupported, inaccessible, and temporary states.") ||
        !store_fixture(archive, *storage::test::scaled_incident(50U, 150U),
                       "Typical workstation",
                       "150 seconds with 50 processes per frame.") ||
        !store_fixture(archive, *storage::test::scaled_incident(500U, 150U),
                       "High process scale",
                       "150 seconds with 500 processes per frame.")) {
        return 1;
    }

    const auto count = archive.incident_count();
    const auto size = archive.database_size_bytes();
    if (!count.has_value() || !size.has_value() || *count != 3U) {
        std::cerr << "fixture verification failed\n";
        return 1;
    }
    std::cout << "fixture_path=" << output.string() << '\n'
              << "incidents=" << *count << '\n'
              << "database_bytes=" << *size << '\n';
    return 0;
}
