#include "platform/posix/posix_crash_diagnostics.hpp"

#include <csignal>
#include <filesystem>
#include <sys/resource.h>

int main(const int argc, char** argv) {
    if (argc != 2) return 2;
    rlimit core_limit{};
    core_limit.rlim_cur = 0U;
    core_limit.rlim_max = 0U;
    static_cast<void>(setrlimit(RLIMIT_CORE, &core_limit));

    blackbox::platform::posix::PosixCrashDiagnostics diagnostics{
        std::filesystem::path{argv[1]}};
    if (!diagnostics.install()) return 3;
    static_cast<void>(::raise(SIGSEGV));
    return 4;
}
