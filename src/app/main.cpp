#include "app/application.hpp"
#include "core/logger.hpp"

#include <SDL3/SDL_main.h>

#include <charconv>
#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

int main(const int argc, char** argv) {
    try {
        bool start_hidden = false;
        blackbox::app::ApplicationDiagnosticOptions diagnostic_options{};
        bool runtime_seen = false;
        bool report_seen = false;
        bool capture_interval_seen = false;
        constexpr std::string_view diagnostic_prefix{
            "--background-diagnostic-seconds="};
        constexpr std::string_view report_prefix{"--diagnostic-report="};
        constexpr std::string_view capture_interval_prefix{
            "--diagnostic-capture-interval-seconds="};
        const auto invalid_argument = [](const std::string_view message) {
            blackbox::core::Logger::write(blackbox::core::LogLevel::error, message);
            return 2;
        };
        for (int index = 1; index < argc; ++index) {
            if (argv[index] == nullptr) {
                continue;
            }
            const std::string_view argument{argv[index]};
            if (argument == "--background") {
                start_hidden = true;
            } else if (argument.starts_with(diagnostic_prefix)) {
                if (runtime_seen) {
                    return invalid_argument("Diagnostic duration may be supplied only once");
                }
                std::uint32_t seconds = 0U;
                const auto value = argument.substr(diagnostic_prefix.size());
                const auto parsed = std::from_chars(
                    value.data(), value.data() + value.size(), seconds);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                    seconds == 0U || seconds > 7U * 24U * 60U * 60U) {
                    return invalid_argument(
                        "Background diagnostic duration must be 1-604800 seconds");
                }
                runtime_seen = true;
                start_hidden = true;
                diagnostic_options.runtime = std::chrono::seconds{seconds};
            } else if (argument.starts_with(report_prefix)) {
                if (report_seen) {
                    return invalid_argument("Diagnostic report may be supplied only once");
                }
                const auto value = argument.substr(report_prefix.size());
                if (value.empty() || value.size() > 1'024U ||
                    value.find_first_of("\r\n=") != std::string_view::npos) {
                    return invalid_argument("Diagnostic report path is invalid");
                }
                auto path = std::filesystem::path{std::string{value}};
                if (!path.is_absolute() || path.filename().empty() ||
                    path.extension() != ".ini") {
                    return invalid_argument(
                        "Diagnostic report must be an absolute .ini path");
                }
                report_seen = true;
                diagnostic_options.report_path = std::move(path);
            } else if (argument.starts_with(capture_interval_prefix)) {
                if (capture_interval_seen) {
                    return invalid_argument(
                        "Diagnostic capture interval may be supplied only once");
                }
                std::uint32_t seconds = 0U;
                const auto value = argument.substr(capture_interval_prefix.size());
                const auto parsed = std::from_chars(
                    value.data(), value.data() + value.size(), seconds);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                    seconds == 0U || seconds > 24U * 60U * 60U) {
                    return invalid_argument(
                        "Diagnostic capture interval must be 1-86400 seconds");
                }
                capture_interval_seen = true;
                diagnostic_options.capture_interval = std::chrono::seconds{seconds};
            } else {
                return invalid_argument("Unknown BlackBox command-line argument");
            }
        }
        if ((report_seen || capture_interval_seen) && !runtime_seen) {
            return invalid_argument(
                "Diagnostic report and capture interval require a diagnostic duration");
        }
        // Application owns bounded recorder/UI state and scratch buffers; keep
        // that long-lived state off the comparatively small process-entry stack.
        auto application = std::make_unique<blackbox::app::Application>(
            start_hidden, std::move(diagnostic_options));
        const auto initialized = application->initialize();
        if (initialized == blackbox::app::ApplicationInitializationResult::already_running) {
            return 0;
        }
        if (initialized != blackbox::app::ApplicationInitializationResult::ready) {
            return 1;
        }
        return application->run();
    } catch (const std::exception& error) {
        blackbox::core::Logger::write(blackbox::core::LogLevel::error, error.what());
        return 1;
    } catch (...) {
        blackbox::core::Logger::write(
            blackbox::core::LogLevel::error,
            "BlackBox stopped after an unexpected startup failure");
        return 1;
    }
}
