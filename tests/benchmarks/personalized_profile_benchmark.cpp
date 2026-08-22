#include "storage/incident_archive.hpp"
#include "storage/test_incident.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace storage = blackbox::storage;

namespace {

using Clock = std::chrono::steady_clock;

class TemporaryArchive final {
public:
    TemporaryArchive() {
        static std::atomic<std::uint64_t> sequence{};
        path = std::filesystem::temp_directory_path() /
               ("blackbox-profile-benchmark-" +
                std::to_string(Clock::now().time_since_epoch().count()) + "-" +
                std::to_string(sequence.fetch_add(1U)) + ".sqlite3");
    }
    ~TemporaryArchive() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }
    std::filesystem::path path{};
};

[[nodiscard]] double milliseconds(const Clock::duration duration) noexcept {
    return std::chrono::duration<double, std::milli>{duration}.count();
}

[[nodiscard]] double nearest_rank(std::vector<double> values,
                                  const std::size_t numerator) {
    std::sort(values.begin(), values.end());
    const auto rank = (values.size() * numerator + 99U) / 100U;
    return values[(std::max<std::size_t>)(1U, rank) - 1U];
}

struct Result {
    std::size_t processes{};
    double update_average_ms{};
    double update_p95_ms{};
    double query_average_ms{};
    double query_p95_ms{};
    std::uint64_t logical_database_bytes{};
    std::uint64_t identities{};
    std::uint64_t observations{};
    bool all_succeeded{true};
};

[[nodiscard]] Result run(const std::size_t process_count) {
    constexpr std::size_t trials = 10U;
    TemporaryArchive temporary;
    storage::SqliteIncidentArchive archive{{temporary.path}};
    Result result{};
    result.processes = process_count;
    if (!archive.open()) {
        result.all_succeeded = false;
        return result;
    }
    std::vector<storage::ProcessProfileUpdate> updates;
    std::vector<std::string> keys;
    updates.reserve(process_count);
    keys.reserve(process_count);
    for (std::size_t index = 0U; index < process_count; ++index) {
        auto key = "path:c:\\benchmark\\process-" + std::to_string(index) + ".exe";
        keys.push_back(key);
        updates.push_back({std::move(key), "process-" + std::to_string(index) + ".exe",
                           0.1 + static_cast<double>(index % 20U) / 100.0,
                           static_cast<double>((index + 16U) << 20U),
                           static_cast<double>(index + 1U) * 1'024.0,
                           static_cast<double>(index + 1U) * 512.0});
    }
    std::vector<double> update_times;
    std::vector<double> query_times;
    update_times.reserve(trials);
    query_times.reserve(trials);
    for (std::size_t trial = 0U; trial < trials; ++trial) {
        const auto incident_id = archive.store(*storage::test::representative_incident());
        if (!incident_id) {
            result.all_succeeded = false;
            break;
        }
        const auto update_started = Clock::now();
        const auto stored = archive.store_process_profile_updates(*incident_id, updates);
        update_times.push_back(milliseconds(Clock::now() - update_started));
        if (!stored) {
            result.all_succeeded = false;
            break;
        }
        const auto query_incident = archive.store(*storage::test::representative_incident());
        if (!query_incident) {
            result.all_succeeded = false;
            break;
        }
        const auto query_started = Clock::now();
        const auto context = archive.process_profile_context(*query_incident, keys);
        query_times.push_back(milliseconds(Clock::now() - query_started));
        if (!context) {
            result.all_succeeded = false;
            break;
        }
    }
    if (!update_times.empty()) {
        for (const auto value : update_times) result.update_average_ms += value;
        result.update_average_ms /= static_cast<double>(update_times.size());
        result.update_p95_ms = nearest_rank(update_times, 95U);
    }
    if (!query_times.empty()) {
        for (const auto value : query_times) result.query_average_ms += value;
        result.query_average_ms /= static_cast<double>(query_times.size());
        result.query_p95_ms = nearest_rank(query_times, 95U);
    }
    if (const auto size = archive.database_size_bytes()) {
        result.logical_database_bytes = *size;
    } else {
        result.all_succeeded = false;
    }
    if (const auto statistics = archive.process_profile_storage_statistics()) {
        result.identities = statistics->identity_count;
        result.observations = statistics->observation_count;
    } else {
        result.all_succeeded = false;
    }
    return result;
}

} // namespace

int main() {
    std::cout << "processes,update_average_ms,update_p95_ms,query_average_ms,"
                 "query_p95_ms,logical_database_bytes,identities,observations,all_succeeded\n";
    bool all_succeeded = true;
    for (const auto count : {50U, 200U, 500U}) {
        const auto result = run(count);
        all_succeeded = all_succeeded && result.all_succeeded;
        std::cout << result.processes << ',' << std::fixed << std::setprecision(3)
                  << result.update_average_ms << ',' << result.update_p95_ms << ','
                  << result.query_average_ms << ',' << result.query_p95_ms << ','
                  << result.logical_database_bytes << ',' << result.identities << ','
                  << result.observations << ',' << (result.all_succeeded ? 1 : 0) << '\n';
    }
    return all_succeeded ? 0 : 1;
}
