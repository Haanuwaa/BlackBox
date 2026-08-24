#include "platform/windows/windows_crash_diagnostics.hpp"
#include "platform/windows/crash_dump_publication.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

#include <array>
#include <atomic>
#include <chrono>
#include <system_error>
#include <utility>

namespace blackbox::platform::windows {

bool detail::publish_completed_dump(const wchar_t* const pending_path,
                                    const wchar_t* const completed_path) noexcept {
    if (pending_path == nullptr || completed_path == nullptr ||
        *pending_path == L'\0' || *completed_path == L'\0') {
        return false;
    }

    constexpr unsigned maximum_attempts = 51U;
    constexpr DWORD retry_delay_milliseconds = 10U;
    for (unsigned attempt = 0U; attempt < maximum_attempts; ++attempt) {
        if (MoveFileExW(pending_path, completed_path, MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        const auto error = GetLastError();
        const auto transient = error == ERROR_ACCESS_DENIED ||
                               error == ERROR_SHARING_VIOLATION ||
                               error == ERROR_LOCK_VIOLATION;
        if (!transient || attempt + 1U == maximum_attempts) return false;
        Sleep(retry_delay_milliseconds);
    }
    return false;
}

struct WindowsCrashDiagnostics::Impl final {
    explicit Impl(std::filesystem::path output_directory)
        : directory{std::move(output_directory)} {}

    static LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* pointers) noexcept {
        auto* state = active.load(std::memory_order_acquire);
        if (state == nullptr ||
            state->handling.exchange(true, std::memory_order_acq_rel) ||
            state->dump_file == INVALID_HANDLE_VALUE) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        MINIDUMP_EXCEPTION_INFORMATION exception_information{};
        exception_information.ThreadId = GetCurrentThreadId();
        exception_information.ExceptionPointers = pointers;
        exception_information.ClientPointers = FALSE;
        constexpr unsigned maximum_dump_attempts = 6U;
        constexpr DWORD dump_retry_delay_milliseconds = 100U;
        BOOL wrote = FALSE;
        for (unsigned attempt = 0U; attempt < maximum_dump_attempts; ++attempt) {
            if (attempt != 0U) {
                LARGE_INTEGER beginning{};
                if (!SetFilePointerEx(state->dump_file, beginning, nullptr, FILE_BEGIN) ||
                    !SetEndOfFile(state->dump_file)) {
                    break;
                }
            }
            wrote = MiniDumpWriteDump(
                GetCurrentProcess(), GetCurrentProcessId(), state->dump_file,
                MiniDumpNormal, pointers == nullptr ? nullptr : &exception_information,
                nullptr, nullptr);
            if (wrote != FALSE) break;
            if (attempt + 1U != maximum_dump_attempts) {
                Sleep(dump_retry_delay_milliseconds);
            }
        }
        static_cast<void>(FlushFileBuffers(state->dump_file));
        static_cast<void>(CloseHandle(state->dump_file));
        state->dump_file = INVALID_HANDLE_VALUE;
        if (wrote != FALSE) {
            static_cast<void>(detail::publish_completed_dump(
                state->pending_path.c_str(), state->completed_path.c_str()));
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }

    void uninstall() noexcept {
        auto* expected = this;
        if (active.compare_exchange_strong(expected, nullptr,
                                           std::memory_order_acq_rel)) {
            static_cast<void>(SetUnhandledExceptionFilter(previous_filter));
        }
        if (dump_file != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(dump_file));
            dump_file = INVALID_HANDLE_VALUE;
        }
        if (!pending_path.empty()) {
            static_cast<void>(DeleteFileW(pending_path.c_str()));
        }
        installed = false;
    }

    [[nodiscard]] bool arm_file() noexcept {
        SYSTEMTIME utc{};
        GetSystemTime(&utc);
        const auto process = GetCurrentProcessId();
        for (unsigned sequence = 0U; sequence < 1'000U; ++sequence) {
            std::array<wchar_t, 128U> filename{};
            const auto length = swprintf_s(
                filename.data(), filename.size(),
                L"crash-%04hu%02hu%02huT%02hu%02hu%02hu.%03huZ-pid%lu-%03u.dmp",
                utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute,
                utc.wSecond, utc.wMilliseconds, static_cast<unsigned long>(process),
                sequence);
            if (length <= 0) return false;
            completed_path = directory / filename.data();
            pending_path = completed_path;
            pending_path += L".partial";
            dump_file = CreateFileW(
                pending_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
            if (dump_file != INVALID_HANDLE_VALUE) return true;
            if (GetLastError() != ERROR_FILE_EXISTS &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                return false;
            }
        }
        return false;
    }

    std::filesystem::path directory{};
    std::filesystem::path pending_path{};
    std::filesystem::path completed_path{};
    HANDLE dump_file{INVALID_HANDLE_VALUE};
    LPTOP_LEVEL_EXCEPTION_FILTER previous_filter{};
    std::atomic_bool handling{};
    bool installed{};
    std::string failure{"Crash diagnostics are not installed"};
    static std::atomic<Impl*> active;
};

std::atomic<WindowsCrashDiagnostics::Impl*> WindowsCrashDiagnostics::Impl::active{};

WindowsCrashDiagnostics::WindowsCrashDiagnostics(std::filesystem::path directory)
    : impl_{std::make_unique<Impl>(std::move(directory))} {}

WindowsCrashDiagnostics::~WindowsCrashDiagnostics() { impl_->uninstall(); }

bool WindowsCrashDiagnostics::install() noexcept {
    try {
        if (impl_->installed) return true;
        if (impl_->directory.empty() || !impl_->directory.is_absolute()) {
            impl_->failure = "Crash directory is not an absolute path";
            return false;
        }
        std::error_code filesystem_error;
        std::filesystem::create_directories(impl_->directory, filesystem_error);
        if (filesystem_error ||
            !std::filesystem::is_directory(impl_->directory, filesystem_error) ||
            filesystem_error) {
            impl_->failure = "Crash directory cannot be created";
            return false;
        }
        Impl* expected = nullptr;
        if (!Impl::active.compare_exchange_strong(expected, impl_.get(),
                                                  std::memory_order_acq_rel)) {
            impl_->failure = "Another crash diagnostic handler is already installed";
            return false;
        }
        if (!impl_->arm_file()) {
            Impl::active.store(nullptr, std::memory_order_release);
            impl_->failure = "Crash dump staging file cannot be created";
            return false;
        }
        impl_->previous_filter = SetUnhandledExceptionFilter(
            &Impl::unhandled_exception_filter);
        impl_->installed = true;
        impl_->failure.clear();
        return true;
    } catch (...) {
        impl_->uninstall();
        impl_->failure = "Unknown crash diagnostic installation failure";
        return false;
    }
}

CrashDiagnosticsSnapshot WindowsCrashDiagnostics::snapshot() const noexcept {
    CrashDiagnosticsSnapshot result{};
    result.available = true;
    result.armed = impl_->installed && impl_->dump_file != INVALID_HANDLE_VALUE;
    try {
        std::error_code filesystem_error;
        auto newest_time = std::filesystem::file_time_type::min();
        if (std::filesystem::is_directory(impl_->directory, filesystem_error) &&
            !filesystem_error) {
            for (std::filesystem::directory_iterator iterator{impl_->directory,
                                                               filesystem_error}, end;
                 iterator != end && !filesystem_error;
                 iterator.increment(filesystem_error)) {
                const auto status = iterator->symlink_status(filesystem_error);
                if (filesystem_error) break;
                if (status.type() != std::filesystem::file_type::regular ||
                    iterator->path().extension() != ".dmp") {
                    continue;
                }
                const auto size = iterator->file_size(filesystem_error);
                if (filesystem_error) break;
                if (size == 0U) continue;
                if (result.completed_evidence != UINT64_MAX) {
                    ++result.completed_evidence;
                }
                const auto changed = iterator->last_write_time(filesystem_error);
                if (filesystem_error) break;
                if (result.latest_evidence.empty() || changed > newest_time) {
                    newest_time = changed;
                    result.latest_evidence = iterator->path();
                }
            }
        }
        if (filesystem_error) {
            result.status = "Crash dump inventory is temporarily unavailable";
        } else if (!impl_->installed) {
            result.status = impl_->failure;
        } else if (result.completed_evidence == 0U) {
            result.status = "Crash diagnostics armed; no previous crash dump";
        } else {
            result.status = "Previous crash evidence is available locally";
        }
    } catch (...) {
        result.status = "Crash dump inventory failed unexpectedly";
    }
    return result;
}

} // namespace blackbox::platform::windows
