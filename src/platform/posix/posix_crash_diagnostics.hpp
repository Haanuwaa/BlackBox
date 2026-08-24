#pragma once

#include "platform/crash_diagnostics.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace blackbox::platform::posix {

inline constexpr std::uint32_t crash_evidence_format_version = 1U;

struct PosixCrashEvidence {
    std::int32_t signal_number{};
    std::int32_t signal_code{};
    std::uint32_t process_id{};
    std::int64_t unix_seconds{};
    std::int64_t unix_nanoseconds{};
    std::uint64_t fault_address{};
    friend constexpr bool operator==(const PosixCrashEvidence&,
                                     const PosixCrashEvidence&) = default;
};

[[nodiscard]] std::optional<PosixCrashEvidence> read_crash_evidence(
    const std::filesystem::path& path) noexcept;

class PosixCrashDiagnostics final : public ICrashDiagnostics {
public:
    explicit PosixCrashDiagnostics(std::filesystem::path directory);
    ~PosixCrashDiagnostics() override;

    PosixCrashDiagnostics(const PosixCrashDiagnostics&) = delete;
    PosixCrashDiagnostics& operator=(const PosixCrashDiagnostics&) = delete;
    PosixCrashDiagnostics(PosixCrashDiagnostics&&) = delete;
    PosixCrashDiagnostics& operator=(PosixCrashDiagnostics&&) = delete;

    [[nodiscard]] bool install() noexcept override;
    [[nodiscard]] CrashDiagnosticsSnapshot snapshot() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

} // namespace blackbox::platform::posix
