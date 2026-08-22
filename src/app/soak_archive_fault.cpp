#include <sqlite3.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <thread>

int main(const int argc, char** argv) {
    if (argc != 3 || argv[1] == nullptr || argv[2] == nullptr) {
        std::cerr << "usage: blackbox_soak_archive_fault <archive> <hold-seconds>\n";
        return 2;
    }
    std::uint32_t hold_seconds{};
    const std::string_view duration{argv[2]};
    const auto parsed = std::from_chars(duration.data(), duration.data() + duration.size(),
                                        hold_seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != duration.data() + duration.size() ||
        hold_seconds == 0U || hold_seconds > 600U) {
        std::cerr << "hold-seconds must be 1-600\n";
        return 2;
    }

    sqlite3* database = nullptr;
    if (sqlite3_open_v2(argv[1], &database, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
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
