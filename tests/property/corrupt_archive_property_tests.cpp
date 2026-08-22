#include "storage/incident_archive.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace storage = blackbox::storage;

namespace {

class TemporaryCorruptArchive final {
public:
    TemporaryCorruptArchive() {
        static std::atomic<std::uint64_t> sequence{};
        directory = std::filesystem::temp_directory_path() /
                    ("blackbox-corrupt-archive-property-" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch().count()) +
                     "-" + std::to_string(sequence.fetch_add(1U)));
        std::filesystem::create_directories(directory);
        path = directory / "candidate.sqlite3";
    }
    ~TemporaryCorruptArchive() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    std::filesystem::path directory{};
    std::filesystem::path path{};
};

[[nodiscard]] std::vector<std::uint8_t> read_bytes(
    const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("random non-database archives are rejected without byte modification",
          "[property][storage][corruption][format-v1]") {
    TemporaryCorruptArchive temporary;
    std::uint64_t state{0x9E3779B97F4A7C15ULL};
    for (std::size_t example = 0U; example < 128U; ++example) {
        const auto size = 1U + (example * 37U) % 1'024U;
        std::vector<std::uint8_t> bytes(size);
        for (auto& byte : bytes) {
            state ^= state >> 12U;
            state ^= state << 25U;
            state ^= state >> 27U;
            byte = static_cast<std::uint8_t>(state >> 56U);
        }
        bytes.front() = static_cast<std::uint8_t>('X');
        {
            std::ofstream output{temporary.path,
                                 std::ios::binary | std::ios::trunc};
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        }
        storage::SqliteIncidentArchive archive{{temporary.path}};
        const auto opened = archive.open();
        CHECK_FALSE(opened.has_value());
        archive.close();
        CHECK(read_bytes(temporary.path) == bytes);
        std::error_code ignored;
        std::filesystem::remove(temporary.path.string() + "-wal", ignored);
        std::filesystem::remove(temporary.path.string() + "-shm", ignored);
    }
}
