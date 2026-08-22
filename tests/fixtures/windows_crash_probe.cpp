#include "platform/windows/windows_crash_diagnostics.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <filesystem>

int main(const int argc, char** argv) {
    if (argc != 2 || argv[1] == nullptr) return 2;
    blackbox::platform::windows::WindowsCrashDiagnostics diagnostics{
        std::filesystem::absolute(argv[1])};
    if (!diagnostics.install()) return 3;
    RaiseException(0xE0424242UL, EXCEPTION_NONCONTINUABLE, 0U, nullptr);
    return 4;
}
