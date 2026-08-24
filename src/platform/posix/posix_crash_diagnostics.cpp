#include "platform/posix/posix_crash_diagnostics.hpp"

#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits.h>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <time.h>
#include <unistd.h>
#include <utility>

namespace blackbox::platform::posix {
namespace {

constexpr std::array handled_signals{SIGABRT, SIGBUS, SIGFPE,
                                     SIGILL, SIGSEGV, SIGTRAP};
constexpr std::string_view evidence_magic{"BBCRASH1"};
constexpr std::size_t header_bytes = 24U;
constexpr std::size_t event_bytes = 40U;
constexpr std::size_t evidence_bytes = header_bytes + event_bytes;

void write_little_endian(unsigned char* const destination,
                         const std::uint64_t value,
                         const std::size_t bytes) noexcept {
    for (std::size_t index = 0U; index < bytes; ++index) {
        destination[index] = static_cast<unsigned char>(value >> (index * 8U));
    }
}

[[nodiscard]] std::uint64_t read_little_endian(
    const unsigned char* const source,
    const std::size_t bytes) noexcept {
    std::uint64_t value{};
    for (std::size_t index = 0U; index < bytes; ++index) {
        value |= static_cast<std::uint64_t>(source[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] bool write_all(const int descriptor,
                             const unsigned char* contents,
                             const std::size_t size) noexcept {
    std::size_t offset{};
    while (offset < size) {
        const auto written = ::write(descriptor, contents + offset, size - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

struct SignalState {
    volatile sig_atomic_t descriptor{-1};
    volatile sig_atomic_t handling{};
    char pending_path[PATH_MAX]{};
    char completed_path[PATH_MAX]{};
    unsigned char event[event_bytes]{};
    std::array<struct sigaction, handled_signals.size()> previous_actions{};
    std::size_t installed_actions{};
};

SignalState signal_state{};
std::mutex installation_mutex{};
const void* installed_owner{};

void crash_signal_handler(const int signal_number,
                          siginfo_t* const information,
                          void*) noexcept {
    const int saved_errno = errno;
    if (signal_state.handling != 0 || signal_state.descriptor < 0) {
        _exit(128 + signal_number);
    }
    signal_state.handling = 1;

    auto& event = signal_state.event;
    write_little_endian(event,
                        static_cast<std::uint32_t>(signal_number), 4U);
    write_little_endian(
        event + 4U,
        information == nullptr
            ? 0U
            : static_cast<std::uint32_t>(information->si_code),
        4U);
    write_little_endian(event + 8U,
                        static_cast<std::uint32_t>(::getpid()), 4U);

    timespec timestamp{};
    if (clock_gettime(CLOCK_REALTIME, &timestamp) == 0) {
        write_little_endian(event + 16U,
                            static_cast<std::uint64_t>(timestamp.tv_sec), 8U);
        write_little_endian(event + 24U,
                            static_cast<std::uint64_t>(timestamp.tv_nsec), 8U);
    }
    write_little_endian(
        event + 32U,
        information == nullptr
            ? 0U
            : static_cast<std::uint64_t>(
                  reinterpret_cast<std::uintptr_t>(information->si_addr)),
        8U);

    const int descriptor = signal_state.descriptor;
    const bool wrote = write_all(descriptor, event, sizeof(event));
    const bool flushed = wrote && ::fsync(descriptor) == 0;
    static_cast<void>(::close(descriptor));
    signal_state.descriptor = -1;
    if (flushed) {
        static_cast<void>(::rename(signal_state.pending_path,
                                   signal_state.completed_path));
    }

    errno = saved_errno;
    if (::kill(::getpid(), signal_number) != 0) {
        _exit(128 + signal_number);
    }
}

[[nodiscard]] bool plain_directory(const std::filesystem::path& path) noexcept {
    std::error_code error{};
    const auto status = std::filesystem::symlink_status(path, error);
    return !error && status.type() == std::filesystem::file_type::directory;
}

} // namespace

struct PosixCrashDiagnostics::Impl {
    explicit Impl(std::filesystem::path value) : directory{std::move(value)} {}

    void uninstall() noexcept {
        const std::scoped_lock lock{installation_mutex};
        const bool owns_handlers = installed_owner == this;
        if (owns_handlers) {
            for (std::size_t index = signal_state.installed_actions; index > 0U;
                 --index) {
                static_cast<void>(sigaction(
                    handled_signals[index - 1U],
                    &signal_state.previous_actions[index - 1U], nullptr));
            }
            signal_state.installed_actions = 0U;
            installed_owner = nullptr;
        }
        if ((owns_handlers || armed) && signal_state.descriptor >= 0) {
            static_cast<void>(::close(signal_state.descriptor));
            signal_state.descriptor = -1;
        }
        if (armed && !pending_path.empty()) {
            static_cast<void>(::unlink(pending_path.c_str()));
        }
        armed = false;
        installed = false;
    }

    [[nodiscard]] bool arm_file() {
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto process = static_cast<std::uint32_t>(::getpid());
        for (unsigned sequence = 0U; sequence < 1'000U; ++sequence) {
            const auto name = "crash-" + std::to_string(now) + "-pid" +
                              std::to_string(process) + "-" +
                              std::to_string(sequence) + ".crash";
            completed_path = (directory / name).string();
            pending_path = completed_path + ".partial";
            if (pending_path.size() >= sizeof(signal_state.pending_path) ||
                completed_path.size() >= sizeof(signal_state.completed_path)) {
                return false;
            }
            const int descriptor = ::open(
                pending_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_SYNC,
                S_IRUSR | S_IWUSR);
            if (descriptor < 0) {
                if (errno == EEXIST) continue;
                return false;
            }

            std::array<unsigned char, header_bytes> header{};
            for (std::size_t index = 0U; index < evidence_magic.size(); ++index) {
                header[index] = static_cast<unsigned char>(evidence_magic[index]);
            }
            write_little_endian(header.data() + 8U,
                                crash_evidence_format_version, 4U);
            write_little_endian(header.data() + 12U, header_bytes, 4U);
            write_little_endian(header.data() + 16U, event_bytes, 4U);
            if (!write_all(descriptor, header.data(), header.size()) ||
                ::fsync(descriptor) != 0) {
                static_cast<void>(::close(descriptor));
                static_cast<void>(::unlink(pending_path.c_str()));
                return false;
            }

            std::memcpy(signal_state.pending_path, pending_path.c_str(),
                        pending_path.size() + 1U);
            std::memcpy(signal_state.completed_path, completed_path.c_str(),
                        completed_path.size() + 1U);
            std::memset(signal_state.event, 0, sizeof(signal_state.event));
            signal_state.handling = 0;
            signal_state.descriptor = descriptor;
            armed = true;
            return true;
        }
        return false;
    }

    std::filesystem::path directory{};
    std::string pending_path{};
    std::string completed_path{};
    std::string failure{"Crash diagnostics are not installed"};
    bool armed{};
    bool installed{};
};

PosixCrashDiagnostics::PosixCrashDiagnostics(std::filesystem::path directory)
    : impl_{std::make_unique<Impl>(std::move(directory))} {}

PosixCrashDiagnostics::~PosixCrashDiagnostics() { impl_->uninstall(); }

bool PosixCrashDiagnostics::install() noexcept {
    try {
        if (impl_->installed) return true;
        if (impl_->directory.empty() || !impl_->directory.is_absolute()) {
            impl_->failure = "Crash directory is not an absolute path";
            return false;
        }
        std::error_code filesystem_error{};
        std::filesystem::create_directories(impl_->directory, filesystem_error);
        if (filesystem_error || !plain_directory(impl_->directory)) {
            impl_->failure = "Crash directory cannot be created safely";
            return false;
        }

        const std::scoped_lock lock{installation_mutex};
        if (installed_owner != nullptr) {
            impl_->failure = "Another crash diagnostic handler is already installed";
            return false;
        }
        if (!impl_->arm_file()) {
            impl_->failure = "Crash evidence staging file cannot be created";
            return false;
        }

        struct sigaction action {};
        action.sa_sigaction = &crash_signal_handler;
        sigfillset(&action.sa_mask);
        action.sa_flags = SA_SIGINFO | SA_RESETHAND;
        signal_state.installed_actions = 0U;
        for (std::size_t index = 0U; index < handled_signals.size(); ++index) {
            if (sigaction(handled_signals[index], &action,
                          &signal_state.previous_actions[index]) != 0) {
                for (std::size_t rollback = index; rollback > 0U; --rollback) {
                    static_cast<void>(sigaction(
                        handled_signals[rollback - 1U],
                        &signal_state.previous_actions[rollback - 1U], nullptr));
                }
                signal_state.installed_actions = 0U;
                static_cast<void>(::close(signal_state.descriptor));
                signal_state.descriptor = -1;
                static_cast<void>(::unlink(impl_->pending_path.c_str()));
                impl_->armed = false;
                impl_->failure = "Crash signal handlers cannot be installed";
                return false;
            }
            signal_state.installed_actions = index + 1U;
        }
        installed_owner = impl_.get();
        impl_->installed = true;
        impl_->failure.clear();
        return true;
    } catch (...) {
        impl_->uninstall();
        impl_->failure = "Unknown crash diagnostic installation failure";
        return false;
    }
}

std::optional<PosixCrashEvidence> read_crash_evidence(
    const std::filesystem::path& path) noexcept {
    try {
        std::error_code error{};
        const auto status = std::filesystem::symlink_status(path, error);
        if (error || status.type() != std::filesystem::file_type::regular ||
            std::filesystem::file_size(path, error) != evidence_bytes || error) {
            return std::nullopt;
        }
        std::array<unsigned char, evidence_bytes> bytes{};
        std::ifstream input{path, std::ios::binary};
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!input || input.peek() != std::ifstream::traits_type::eof()) {
            return std::nullopt;
        }
        for (std::size_t index = 0U; index < evidence_magic.size(); ++index) {
            if (bytes[index] != static_cast<unsigned char>(evidence_magic[index])) {
                return std::nullopt;
            }
        }
        if (read_little_endian(bytes.data() + 8U, 4U) !=
                crash_evidence_format_version ||
            read_little_endian(bytes.data() + 12U, 4U) != header_bytes ||
            read_little_endian(bytes.data() + 16U, 4U) != event_bytes ||
            read_little_endian(bytes.data() + 20U, 4U) != 0U ||
            read_little_endian(bytes.data() + header_bytes + 12U, 4U) != 0U) {
            return std::nullopt;
        }
        const auto signal_bits = static_cast<std::uint32_t>(
            read_little_endian(bytes.data() + header_bytes, 4U));
        const auto code_bits = static_cast<std::uint32_t>(
            read_little_endian(bytes.data() + header_bytes + 4U, 4U));
        PosixCrashEvidence result{};
        result.signal_number = std::bit_cast<std::int32_t>(signal_bits);
        result.signal_code = std::bit_cast<std::int32_t>(code_bits);
        result.process_id = static_cast<std::uint32_t>(
            read_little_endian(bytes.data() + header_bytes + 8U, 4U));
        result.unix_seconds = std::bit_cast<std::int64_t>(
            read_little_endian(bytes.data() + header_bytes + 16U, 8U));
        result.unix_nanoseconds = std::bit_cast<std::int64_t>(
            read_little_endian(bytes.data() + header_bytes + 24U, 8U));
        result.fault_address =
            read_little_endian(bytes.data() + header_bytes + 32U, 8U);
        if (result.signal_number <= 0 || result.process_id == 0U ||
            result.unix_seconds < 0 || result.unix_nanoseconds < 0 ||
            result.unix_nanoseconds >= 1'000'000'000LL) {
            return std::nullopt;
        }
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

CrashDiagnosticsSnapshot PosixCrashDiagnostics::snapshot() const noexcept {
    CrashDiagnosticsSnapshot result{};
    result.available = true;
    result.armed = impl_->installed && signal_state.descriptor >= 0;
    try {
        std::error_code filesystem_error{};
        auto newest_time = std::filesystem::file_time_type::min();
        if (plain_directory(impl_->directory)) {
            for (std::filesystem::directory_iterator iterator{impl_->directory,
                                                               filesystem_error}, end;
                 iterator != end && !filesystem_error;
                 iterator.increment(filesystem_error)) {
                if (iterator->path().extension() != ".crash" ||
                    !read_crash_evidence(iterator->path()).has_value()) {
                    continue;
                }
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
            result.status = "Crash evidence inventory is temporarily unavailable";
        } else if (!impl_->installed) {
            result.status = impl_->failure;
        } else if (result.completed_evidence == 0U) {
            result.status = "Crash diagnostics armed; no previous crash evidence";
        } else {
            result.status = "Previous crash evidence is available locally";
        }
    } catch (...) {
        result.status = "Crash evidence inventory failed unexpectedly";
    }
    return result;
}

} // namespace blackbox::platform::posix
