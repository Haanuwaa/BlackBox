#include "platform/posix/posix_crash_diagnostics.hpp"

#include <catch2/catch_test_macros.hpp>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace posix_platform = blackbox::platform::posix;

namespace {

class TemporaryCrashDirectory final {
public:
    TemporaryCrashDirectory() {
        static std::atomic<std::uint64_t> sequence{};
        path = std::filesystem::temp_directory_path() /
               ("blackbox-posix-crash-test-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()) +
                "-" + std::to_string(sequence.fetch_add(1U)));
        std::filesystem::create_directories(path);
    }
    ~TemporaryCrashDirectory() {
        std::error_code ignored{};
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path{};
};

[[nodiscard]] std::filesystem::path current_executable() {
#if defined(__APPLE__)
    std::uint32_t size{1024U};
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.resize(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    }
    return std::filesystem::weakly_canonical(buffer.data());
#else
    std::array<char, 4096U> buffer{};
    const auto size = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1U);
    if (size <= 0 || static_cast<std::size_t>(size) >= buffer.size()) return {};
    return std::filesystem::path{
        std::string{buffer.data(), static_cast<std::size_t>(size)}};
#endif
}

[[nodiscard]] std::vector<std::filesystem::path> completed_evidence(
    const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> result{};
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        if (entry.is_regular_file() && entry.path().extension() == ".crash") {
            result.push_back(entry.path());
        }
    }
    return result;
}

} // namespace

TEST_CASE("POSIX crash diagnostics clean unused staging",
          "[platform][posix][crash]") {
    TemporaryCrashDirectory temporary;
    {
        posix_platform::PosixCrashDiagnostics diagnostics{temporary.path};
        REQUIRE(diagnostics.install());
        const auto state = diagnostics.snapshot();
        CHECK(state.available);
        CHECK(state.armed);
        CHECK(state.completed_evidence == 0U);
    }
    CHECK(std::filesystem::is_empty(temporary.path));
}

TEST_CASE("POSIX crash probe publishes one canonical bounded signal record",
          "[platform][posix][crash][integration]") {
    TemporaryCrashDirectory temporary;
    const auto probe = current_executable().parent_path() /
                       "blackbox_posix_crash_probe";
    REQUIRE(std::filesystem::is_regular_file(probe));

    const auto child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        ::execl(probe.c_str(), probe.c_str(), temporary.path.c_str(),
                static_cast<char*>(nullptr));
        _exit(127);
    }

    int status{};
    bool completed{};
    for (unsigned attempt = 0U; attempt < 2'000U; ++attempt) {
        const auto waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) {
            completed = true;
            break;
        }
        REQUIRE(waited >= 0);
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if (!completed) {
        static_cast<void>(::kill(child, SIGKILL));
        static_cast<void>(::waitpid(child, &status, 0));
    }
    REQUIRE(completed);
    REQUIRE(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGSEGV);

    const auto records = completed_evidence(temporary.path);
    REQUIRE(records.size() == 1U);
    const auto evidence = posix_platform::read_crash_evidence(records.front());
    REQUIRE(evidence.has_value());
    CHECK(evidence->signal_number == SIGSEGV);
    CHECK(evidence->process_id == static_cast<std::uint32_t>(child));
    CHECK(evidence->unix_nanoseconds >= 0);
    CHECK(evidence->unix_nanoseconds < 1'000'000'000LL);

    {
        std::ofstream corrupt{records.front(), std::ios::binary | std::ios::app};
        corrupt.put('\0');
    }
    CHECK_FALSE(posix_platform::read_crash_evidence(records.front()).has_value());
}
