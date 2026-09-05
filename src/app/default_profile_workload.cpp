// Development-only, bounded CPU workload for the real default-detector rehearsal.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

int main() {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    std::atomic<std::uint64_t> sink{};
    const auto count = std::clamp(std::thread::hardware_concurrency(), 1U, 64U);
    std::vector<std::jthread> workers;
    workers.reserve(count);
    for (unsigned index = 0; index < count; ++index) {
        workers.emplace_back([&, index] {
            std::uint64_t value = index + 1U;
            while (std::chrono::steady_clock::now() < deadline) {
                for (unsigned step = 0; step < 10000U; ++step)
                    value = value * 6364136223846793005ULL + 1442695040888963407ULL;
                sink.fetch_xor(value, std::memory_order_relaxed);
            }
        });
    }
    return 0;
}
