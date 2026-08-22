#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

int main() {
    std::vector<std::byte> memory(32U * 1024U * 1024U, std::byte{0x5A});
    const auto path = std::filesystem::temp_directory_path() /
                      ("blackbox-process-fixture-" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch().count()) +
                       ".bin");
    std::atomic<bool> keep_running{true};
    std::jthread writer{[&] {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        while (keep_running.load(std::memory_order_relaxed)) {
            output.seekp(0);
            output.write(reinterpret_cast<const char*>(memory.data()),
                         static_cast<std::streamsize>(memory.size()));
            output.flush();
        }
    }};

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    volatile std::uint64_t checksum = 0U;
    while (std::chrono::steady_clock::now() < deadline) {
        for (std::uint64_t index = 0U; index < 100'000U; ++index) {
            checksum = checksum + index;
        }
    }
    keep_running.store(false, std::memory_order_relaxed);
    writer.join();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return checksum == 0U ? 1 : 0;
}
