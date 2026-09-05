#include <sqlite3.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

// Console tools receive the native wide command line on Windows. SQLite paths
// are UTF-8; the ordinary narrow CRT argv uses the machine's legacy code page.
#if defined(_WIN32)
int wmain(const int argc, wchar_t** argv) {
#else
int main(const int argc, char** argv) {
#endif
    if (argc != 3 || argv[1] == nullptr || argv[2] == nullptr) {
        std::cerr << "usage: blackbox_soak_archive_fault <archive> <hold-seconds>\n";
        return 2;
    }
    std::uint32_t hold_seconds{};
#if defined(_WIN32)
    const auto native_archive = std::filesystem::path{argv[1]}.u8string();
    const std::string archive_path{native_archive.begin(), native_archive.end()};
    const auto native_duration = std::filesystem::path{argv[2]}.u8string();
    const std::string duration_text{native_duration.begin(), native_duration.end()};
    const std::string_view duration{duration_text};
#else
    const std::string archive_path{argv[1]};
    const std::string_view duration{argv[2]};
#endif
    const auto parsed = std::from_chars(duration.data(), duration.data() + duration.size(),
                                        hold_seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != duration.data() + duration.size() ||
        hold_seconds == 0U || hold_seconds > 600U) {
        std::cerr << "hold-seconds must be 1-600\n";
        return 2;
    }

    sqlite3* database = nullptr;
    if (sqlite3_open_v2(archive_path.c_str(), &database, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (database != nullptr) sqlite3_close(database);
        std::cerr << "cannot open the existing isolated archive\n";
        return 1;
    }
    static_cast<void>(sqlite3_busy_timeout(database, 0));
    char* message = nullptr;
    const int begun = sqlite3_exec(database, "BEGIN IMMEDIATE", nullptr, nullptr, &message);
    if (message != nullptr) sqlite3_free(message);
    if (begun != SQLITE_OK) {
        sqlite3_close(database);
        std::cerr << "cannot acquire the isolated archive writer lock\n";
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::seconds{hold_seconds});
    const int rolled_back = sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(database);
    if (rolled_back != SQLITE_OK) {
        std::cerr << "cannot release the isolated archive writer lock\n";
        return 1;
    }
    return 0;
}
