#pragma once

#include "platform/crash_diagnostics.hpp"

#include <filesystem>
#include <memory>

namespace blackbox::platform::windows {

class WindowsCrashDiagnostics final : public ICrashDiagnostics {
public:
    explicit WindowsCrashDiagnostics(std::filesystem::path directory);
    ~WindowsCrashDiagnostics() override;
    WindowsCrashDiagnostics(const WindowsCrashDiagnostics&) = delete;
    WindowsCrashDiagnostics& operator=(const WindowsCrashDiagnostics&) = delete;
    WindowsCrashDiagnostics(WindowsCrashDiagnostics&&) = delete;
    WindowsCrashDiagnostics& operator=(WindowsCrashDiagnostics&&) = delete;

    [[nodiscard]] bool install() noexcept override;
    [[nodiscard]] CrashDiagnosticsSnapshot snapshot() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace blackbox::platform::windows
