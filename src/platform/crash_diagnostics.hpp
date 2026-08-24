#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace blackbox::platform {

struct CrashDiagnosticsSnapshot {
    bool available{};
    bool armed{};
    std::uint64_t completed_evidence{};
    std::filesystem::path latest_evidence{};
    std::string status{"Crash diagnostics unavailable on this platform"};
};

class ICrashDiagnostics {
public:
    virtual ~ICrashDiagnostics() = default;
    [[nodiscard]] virtual bool install() noexcept = 0;
    [[nodiscard]] virtual CrashDiagnosticsSnapshot snapshot() const noexcept = 0;
};

} // namespace blackbox::platform
