#include "storage/incident_dataset.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

int fail(const blackbox::storage::StorageError& error) {
    std::cerr << "BlackBox dataset operation failed: " << error.message << '\n';
    return 1;
}

template <typename Integer>
bool parse_integer(const std::string_view text, Integer& output) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

void print_maintenance(const blackbox::storage::ArchiveMaintenanceResult& result) {
    std::cout << "Deleted " << result.incidents_deleted << " incidents; "
              << result.incidents_remaining << " remain; logical archive size "
              << result.database_size_bytes << " bytes.\n";
}

} // namespace

int main(const int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: blackbox_dataset_tool export <archive.sqlite3> <new-directory>\n"
                     "       blackbox_dataset_tool import-classifications "
                     "<archive.sqlite3> <dataset-directory>\n"
                     "       blackbox_dataset_tool keep-newest <archive.sqlite3> <count>\n"
                     "       blackbox_dataset_tool prune-before <archive.sqlite3> <utc-ms>\n"
                     "       blackbox_dataset_tool purge-all <archive.sqlite3> DELETE-ALL\n";
        return 2;
    }
    const auto command = std::string_view{argv[1]};
    if (command == "purge-all" && std::string_view{argv[3]} != "DELETE-ALL") {
        std::cerr << "purge-all requires the exact confirmation token DELETE-ALL.\n";
        return 2;
    }
    blackbox::storage::SqliteIncidentArchive archive{
        blackbox::storage::ArchiveConfiguration{.path = std::filesystem::path{argv[2]}}};
    if (auto opened = archive.open(); !opened) return fail(opened.error());
    if (command == "export") {
        auto result = blackbox::storage::export_incident_dataset(
            archive, std::filesystem::path{argv[3]});
        if (!result) return fail(result.error());
        std::cout << "Exported " << result->incidents << " incidents, "
                  << result->system_samples << " system samples, and "
                  << result->process_samples << " privacy-redacted process samples.\n";
        return 0;
    }
    if (command == "import-classifications") {
        auto result = blackbox::storage::import_incident_dataset_classifications(
            archive, std::filesystem::path{argv[3]});
        if (!result) return fail(result.error());
        std::cout << "Matched " << result->incidents << " incidents and updated "
                  << result->classifications_updated << " classifications.\n";
        return 0;
    }
    if (command == "keep-newest") {
        std::size_t count{};
        if (!parse_integer(std::string_view{argv[3]}, count) || count == 0U) {
            std::cerr << "keep-newest requires a positive incident count.\n";
            return 2;
        }
        auto result = archive.apply_retention(
            blackbox::storage::ArchiveRetentionPolicy{
                .maximum_incidents = count, .compact_after_delete = true});
        if (!result) return fail(result.error());
        print_maintenance(*result);
        return 0;
    }
    if (command == "prune-before") {
        std::int64_t cutoff{};
        if (!parse_integer(std::string_view{argv[3]}, cutoff) || cutoff <= 0) {
            std::cerr << "prune-before requires a positive UTC epoch-millisecond cutoff.\n";
            return 2;
        }
        auto result = archive.apply_retention(
            blackbox::storage::ArchiveRetentionPolicy{
                .delete_before_utc_milliseconds = cutoff,
                .compact_after_delete = true});
        if (!result) return fail(result.error());
        print_maintenance(*result);
        return 0;
    }
    if (command == "purge-all") {
        auto result = archive.purge_all_incidents();
        if (!result) return fail(result.error());
        print_maintenance(*result);
        return 0;
    }
    std::cerr << "Unknown dataset operation: " << command << '\n';
    return 2;
}
