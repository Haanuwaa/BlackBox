#include "platform/windows/crash_dump_publication.hpp"
#include "platform/windows/windows_crash_diagnostics.hpp"

#include <catch2/catch_test_macros.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace platform = blackbox::platform;

namespace {

class TemporaryCrashDirectory final {
public:
    TemporaryCrashDirectory() {
        static std::atomic<std::uint64_t> sequence{};
        path = std::filesystem::temp_directory_path() /
               ("blackbox-crash-diagnostics-test-" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch().count()) +
                "-" + std::to_string(sequence.fetch_add(1U)));
        std::filesystem::create_directories(path);
    }
    ~TemporaryCrashDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path{};
};

[[nodiscard]] std::filesystem::path sibling_executable(
    const std::wstring_view name) {
    std::wstring buffer(32'768U, L'\0');
    const auto length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0U || length >= buffer.size()) return {};
    buffer.resize(length);
    return std::filesystem::path{buffer}.parent_path() / name;
}

[[nodiscard]] std::vector<std::filesystem::path> completed_dumps(
    const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> result;
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        if (entry.is_regular_file() && entry.path().extension() == ".dmp") {
            result.push_back(entry.path());
        }
    }
    return result;
}

} // namespace

TEST_CASE("Windows crash diagnostics inventories evidence and cleans normal staging",
          "[platform][windows][crash]") {
    TemporaryCrashDirectory temporary;
    const auto prior = temporary.path / "prior.dmp";
    {
        std::ofstream output{prior, std::ios::binary};
        output << "MDMPfixture";
    }
    {
        blackbox::platform::windows::WindowsCrashDiagnostics diagnostics{
            temporary.path};
        REQUIRE(diagnostics.install());
        const auto state = diagnostics.snapshot();
        CHECK(state.available);
        CHECK(state.armed);
        CHECK(state.completed_dumps == 1U);
        CHECK(state.latest_dump == prior);
    }
    for (const auto& entry : std::filesystem::directory_iterator{temporary.path}) {
        CHECK(entry.path().extension() != ".partial");
    }
}

TEST_CASE("Windows crash dump publication tolerates bounded file contention",
          "[platform][windows][crash]") {
    TemporaryCrashDirectory temporary;
    const auto pending = temporary.path / "contended.dmp.partial";
    const auto completed = temporary.path / "contended.dmp";
    {
        std::ofstream output{pending, std::ios::binary};
        output << "MDMPfixture";
    }

    const auto blocker = CreateFileW(
        pending.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(blocker != INVALID_HANDLE_VALUE);
    std::jthread release_blocker{[blocker] {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        static_cast<void>(CloseHandle(blocker));
    }};

    const auto started = std::chrono::steady_clock::now();
    CHECK(blackbox::platform::windows::detail::publish_completed_dump(
        pending.c_str(), completed.c_str()));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed >= std::chrono::milliseconds{50});
    CHECK(elapsed < std::chrono::seconds{1});
    CHECK_FALSE(std::filesystem::exists(pending));
    CHECK(std::filesystem::is_regular_file(completed));
}

TEST_CASE("Windows unhandled exception probe writes a bounded minidump",
          "[platform][windows][crash][integration]") {
    TemporaryCrashDirectory temporary;
    const auto probe = sibling_executable(L"blackbox_crash_probe.exe");
    REQUIRE(std::filesystem::is_regular_file(probe));

    std::vector<std::filesystem::path> dumps;
    for (std::size_t attempt = 0U; attempt < 3U && dumps.empty(); ++attempt) {
        std::wstring command = L"\"" + probe.wstring() + L"\" \"" +
                               temporary.path.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
        const auto waited = WaitForSingleObject(process.hProcess, 20'000U);
        DWORD exit_code{};
        REQUIRE(waited == WAIT_OBJECT_0);
        REQUIRE(GetExitCodeProcess(process.hProcess, &exit_code));
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CHECK(exit_code != 0U);
        dumps = completed_dumps(temporary.path);
        if (dumps.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }

    std::uintmax_t partial_bytes = 0U;
    std::size_t partial_count = 0U;
    for (const auto& entry : std::filesystem::directory_iterator{temporary.path}) {
        if (entry.is_regular_file() && entry.path().extension() == ".partial") {
            ++partial_count;
            partial_bytes += entry.file_size();
        }
    }
    CAPTURE(partial_count, partial_bytes);
    REQUIRE(dumps.size() == 1U);
    const auto size = std::filesystem::file_size(dumps.front());
    CHECK(size > 4U);
    CHECK(size <= (64ULL << 20U));
    std::array<char, 4U> signature{};
    std::ifstream input{dumps.front(), std::ios::binary};
    input.read(signature.data(), static_cast<std::streamsize>(signature.size()));
    CHECK(signature[0] == 'M');
    CHECK(signature[1] == 'D');
    CHECK(signature[2] == 'M');
    CHECK(signature[3] == 'P');
    for (const auto& entry : std::filesystem::directory_iterator{temporary.path}) {
        CHECK(entry.path().extension() != ".partial");
    }
}
