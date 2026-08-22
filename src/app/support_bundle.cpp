#include "app/support_bundle.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>
#include <utility>

namespace blackbox::app {
namespace {

constexpr std::string_view privacy_readme =
    "BlackBox support bundle - direct format v1\n"
    "\n"
    "This bundle is created locally and is never uploaded automatically.\n"
    "diagnostics.ini contains bounded counters, feature/privacy switches, archive\n"
    "health totals, and product/platform versions. It deliberately excludes the\n"
    "incident archive, telemetry samples, process rows, executable names or paths,\n"
    "annotations, settings values, hotkeys, usernames, and absolute local paths.\n"
    "\n"
    "If crash.dmp is present, the user explicitly chose to include it. A minidump\n"
    "can contain stack memory and module paths and must be reviewed and shared as\n"
    "potentially personal data. Delete it before sharing if that is not acceptable.\n";

[[nodiscard]] SupportBundleError error(const SupportBundleErrorCode code,
                                       std::string message) {
    return SupportBundleError{code, std::move(message)};
}

[[nodiscard]] bool safe_identifier(const std::string_view value) noexcept {
    if (value.empty() || value.size() > 64U) return false;
    return std::ranges::all_of(value, [](const unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == ' ' ||
               character == '.' || character == '_' || character == '-';
    });
}

[[nodiscard]] std::filesystem::path staging_path(
    const std::filesystem::path& destination) {
    auto result = destination;
    result += ".partial";
    return result;
}

[[nodiscard]] bool write_file(const std::filesystem::path& path,
                              const std::string_view contents) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    return output.good();
}

[[nodiscard]] std::expected<std::uint64_t, SupportBundleError> fingerprint_file(
    const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::unexpected{error(SupportBundleErrorCode::cannot_write,
                                     "support artifact cannot be read back")};
    }
    std::uint64_t hash{1469598103934665603ULL};
    // Keep support-bundle creation off the recorder/UI thread's limited stack.
    // This function already reports allocation failures through the outer
    // create_support_bundle exception boundary.
    std::vector<char> buffer(16U * 1024U);
    std::uintmax_t total{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count < 0) {
            return std::unexpected{error(SupportBundleErrorCode::cannot_write,
                                         "support artifact read-back failed")};
        }
        total += static_cast<std::uintmax_t>(count);
        if (total > maximum_support_crash_dump_bytes) {
            return std::unexpected{error(SupportBundleErrorCode::crash_dump_too_large,
                                         "support artifact exceeds the 64 MiB bound")};
        }
        for (std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
            hash *= 1099511628211ULL;
        }
    }
    if (!input.eof() || total == 0U) {
        return std::unexpected{error(SupportBundleErrorCode::cannot_write,
                                     "support artifact is empty or unreadable")};
    }
    return hash;
}

void append_boolean(std::ostringstream& output, const std::string_view name,
                    const bool value) {
    output << name << '=' << (value ? 1 : 0) << '\n';
}

template <typename Integer>
void append_integer(std::ostringstream& output, const std::string_view name,
                    const Integer value) {
    output << name << '=' << value << '\n';
}

[[nodiscard]] std::string diagnostics_text(const SupportDiagnostics& value) {
    std::ostringstream output;
    output << "format=" << support_bundle_format_version << '\n'
           << "application_version=" << value.application_version << '\n'
           << "platform=" << value.platform << '\n';
    append_boolean(output, "collector_running", value.collector_running);
    append_boolean(output, "automatic_detection_enabled",
                   value.automatic_detection_enabled);
    append_boolean(output, "process_path_collection_enabled",
                   value.process_path_collection_enabled);
    append_boolean(output, "foreground_identity_enabled",
                   value.foreground_identity_enabled);
    append_boolean(output, "process_lifecycle_enabled",
                   value.process_lifecycle_enabled);
    append_boolean(output, "power_and_device_events_enabled",
                   value.power_and_device_events_enabled);
    append_boolean(output, "audio_device_events_enabled",
                   value.audio_device_events_enabled);
    append_boolean(output, "windows_event_evidence_enabled",
                   value.windows_event_evidence_enabled);
    append_integer(output, "collections", value.collections);
    append_integer(output, "partial_samples", value.partial_samples);
    append_integer(output, "failed_samples", value.failed_samples);
    append_integer(output, "dropped_samples", value.dropped_samples);
    append_integer(output, "deadline_misses", value.deadline_misses);
    append_integer(output, "resume_events", value.resume_events);
    append_integer(output, "provider_recoveries", value.provider_recoveries);
    append_integer(output, "collector_worker_failures",
                   value.collector_worker_failures);
    append_integer(output, "incident_captures_started",
                   value.incident_captures_started);
    append_integer(output, "incidents_completed", value.incidents_completed);
    append_integer(output, "incident_snapshot_failures",
                   value.incident_snapshot_failures);
    append_integer(output, "incident_queue_rejections",
                   value.incident_queue_rejections);
    append_integer(output, "automatic_detector_triggers",
                   value.automatic_detector_triggers);
    append_integer(output, "system_events_recorded", value.system_events_recorded);
    append_integer(output, "system_events_dropped", value.system_events_dropped);
    append_integer(output, "process_lifecycle_observations",
                   value.process_lifecycle_observations);
    append_integer(output, "process_lifecycle_events_recorded",
                   value.process_lifecycle_events_recorded);
    append_boolean(output, "storage_enabled", value.storage_enabled);
    append_boolean(output, "storage_writer_running", value.storage_writer_running);
    append_boolean(output, "archive_healthy", value.archive_healthy);
    append_integer(output, "archive_schema_version", value.archive_schema_version);
    append_integer(output, "stored_incidents", value.stored_incidents);
    append_integer(output, "archive_database_bytes", value.archive_database_bytes);
    append_integer(output, "archive_maximum_bytes", value.archive_maximum_bytes);
    append_integer(output, "storage_write_attempts", value.storage_write_attempts);
    append_integer(output, "storage_retry_attempts", value.storage_retry_attempts);
    append_integer(output, "storage_retry_exhausted", value.storage_retry_exhausted);
    append_integer(output, "storage_write_successes", value.storage_write_successes);
    append_integer(output, "storage_write_failures", value.storage_write_failures);
    append_boolean(output, "recoverable_incident_available",
                   value.recoverable_incident_available);
    append_integer(output, "previous_crash_dumps", value.previous_crash_dumps);
    return output.str();
}

[[nodiscard]] std::expected<void, SupportBundleError> validate_crash_source(
    const std::filesystem::path& path) {
    std::error_code filesystem_error;
    if (path.empty() || !path.is_absolute()) {
        return std::unexpected{error(SupportBundleErrorCode::crash_dump_invalid,
                                     "consented crash dump path must be absolute")};
    }
    const auto status = std::filesystem::symlink_status(path, filesystem_error);
    if (filesystem_error || status.type() != std::filesystem::file_type::regular) {
        return std::unexpected{error(SupportBundleErrorCode::crash_dump_invalid,
                                     "consented crash dump must be a regular non-link file")};
    }
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || size == 0U) {
        return std::unexpected{error(SupportBundleErrorCode::crash_dump_invalid,
                                     "consented crash dump is empty or unreadable")};
    }
    if (size > maximum_support_crash_dump_bytes) {
        return std::unexpected{error(SupportBundleErrorCode::crash_dump_too_large,
                                     "consented crash dump exceeds the 64 MiB bound")};
    }
    return {};
}

} // namespace

std::expected<SupportBundleResult, SupportBundleError> create_support_bundle(
    const SupportBundleRequest& request) noexcept {
    try {
        if (request.destination.empty() || !request.destination.is_absolute() ||
            request.destination.filename().empty() ||
            !safe_identifier(request.diagnostics.application_version) ||
            !safe_identifier(request.diagnostics.platform)) {
            return std::unexpected{error(
                SupportBundleErrorCode::invalid_request,
                "support bundle requires a new absolute destination and safe version fields")};
        }
        const auto parent = request.destination.parent_path();
        std::error_code filesystem_error;
        if (parent.empty() || !std::filesystem::is_directory(parent, filesystem_error) ||
            filesystem_error) {
            return std::unexpected{error(SupportBundleErrorCode::invalid_request,
                                         "support bundle parent directory must exist")};
        }
        if (std::filesystem::exists(request.destination, filesystem_error)) {
            return std::unexpected{error(SupportBundleErrorCode::destination_exists,
                                         "support bundle destination already exists")};
        }
        if (filesystem_error) {
            return std::unexpected{error(SupportBundleErrorCode::cannot_write,
                                         "support bundle destination cannot be inspected")};
        }
        const auto staging = staging_path(request.destination);
        if (std::filesystem::exists(staging, filesystem_error)) {
            return std::unexpected{error(SupportBundleErrorCode::staging_exists,
                                         "support bundle staging directory already exists")};
        }
        if (filesystem_error) {
            return std::unexpected{error(SupportBundleErrorCode::cannot_write,
                                         "support bundle staging path cannot be inspected")};
        }
        if (request.consented_crash_dump.has_value() !=
            request.crash_dump_disclosure_confirmed) {
            return std::unexpected{error(
                SupportBundleErrorCode::invalid_request,
                "raw crash dump inclusion requires matching explicit disclosure consent")};
        }
        if (request.consented_crash_dump) {
            if (const auto valid = validate_crash_source(*request.consented_crash_dump);
                !valid) {
                return std::unexpected{valid.error()};
            }
        }

        if (!std::filesystem::create_directory(staging, filesystem_error) ||
            filesystem_error) {
            return std::unexpected{error(SupportBundleErrorCode::cannot_write,
                                         "support bundle staging directory cannot be created")};
        }
        const auto fail = [&staging](SupportBundleError failure) {
            // A failed partial directory is preserved for inspection and can never be
            // mistaken for a published support bundle.
            return std::expected<SupportBundleResult, SupportBundleError>{
                std::unexpected{std::move(failure)}};
        };

        const auto diagnostics = diagnostics_text(request.diagnostics);
        if (!write_file(staging / "diagnostics.ini", diagnostics) ||
            !write_file(staging / "README.txt", privacy_readme)) {
            return fail(error(SupportBundleErrorCode::cannot_write,
                              "support bundle text artifacts cannot be written"));
        }

        bool included_crash_dump = false;
        if (request.consented_crash_dump) {
            std::filesystem::copy_file(*request.consented_crash_dump,
                                       staging / "crash.dmp",
                                       std::filesystem::copy_options::none,
                                       filesystem_error);
            if (filesystem_error) {
                return fail(error(SupportBundleErrorCode::cannot_write,
                                  "consented crash dump cannot be copied"));
            }
            included_crash_dump = true;
        }

        const auto diagnostics_fingerprint = fingerprint_file(staging / "diagnostics.ini");
        const auto readme_fingerprint = fingerprint_file(staging / "README.txt");
        if (!diagnostics_fingerprint) return fail(diagnostics_fingerprint.error());
        if (!readme_fingerprint) return fail(readme_fingerprint.error());
        std::optional<std::uint64_t> crash_fingerprint;
        if (included_crash_dump) {
            const auto fingerprint = fingerprint_file(staging / "crash.dmp");
            if (!fingerprint) return fail(fingerprint.error());
            crash_fingerprint = *fingerprint;
        }

        const auto generated = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::ostringstream manifest;
        manifest << "format=" << support_bundle_format_version << '\n'
                 << "generated_unix_milliseconds=" << generated << '\n'
                 << "file_count=" << (included_crash_dump ? 4U : 3U) << '\n'
                 << "includes_crash_dump=" << (included_crash_dump ? 1 : 0) << '\n'
                 << "diagnostics_fingerprint=" << *diagnostics_fingerprint << '\n'
                 << "readme_fingerprint=" << *readme_fingerprint << '\n';
        if (crash_fingerprint) {
            manifest << "crash_dump_fingerprint=" << *crash_fingerprint << '\n';
        }
        if (!write_file(staging / "manifest.ini", manifest.str())) {
            return fail(error(SupportBundleErrorCode::cannot_write,
                              "support bundle manifest cannot be written"));
        }

        const std::set<std::string> expected = included_crash_dump
            ? std::set<std::string>{"README.txt", "crash.dmp", "diagnostics.ini",
                                    "manifest.ini"}
            : std::set<std::string>{"README.txt", "diagnostics.ini", "manifest.ini"};
        std::set<std::string> observed;
        std::uint64_t bytes{};
        for (std::filesystem::directory_iterator iterator{staging, filesystem_error}, end;
             iterator != end && !filesystem_error; iterator.increment(filesystem_error)) {
            const auto status = iterator->symlink_status(filesystem_error);
            if (filesystem_error || status.type() != std::filesystem::file_type::regular) break;
            const auto name = iterator->path().filename().string();
            const auto size = iterator->file_size(filesystem_error);
            if (filesystem_error || size == 0U ||
                size > maximum_support_crash_dump_bytes || !observed.insert(name).second) {
                filesystem_error = std::make_error_code(std::errc::invalid_argument);
                break;
            }
            if (bytes > std::numeric_limits<std::uint64_t>::max() - size) {
                filesystem_error = std::make_error_code(std::errc::value_too_large);
                break;
            }
            bytes += static_cast<std::uint64_t>(size);
        }
        if (filesystem_error || observed != expected) {
            return fail(error(SupportBundleErrorCode::cannot_write,
                              "support bundle staging content failed exact validation"));
        }

        std::filesystem::rename(staging, request.destination, filesystem_error);
        if (filesystem_error) {
            return fail(error(SupportBundleErrorCode::cannot_publish,
                              "support bundle cannot be atomically published"));
        }
        return SupportBundleResult{request.destination,
                                   static_cast<std::uint64_t>(expected.size()), bytes,
                                   included_crash_dump};
    } catch (const std::exception& exception) {
        return std::unexpected{error(SupportBundleErrorCode::cannot_write,
                                     exception.what())};
    } catch (...) {
        return std::unexpected{error(SupportBundleErrorCode::cannot_write,
                                     "unknown support bundle failure")};
    }
}

SupportBundleService::~SupportBundleService() { stop(); }

void SupportBundleService::start() {
    const std::scoped_lock lock{mutex_};
    if (worker_.joinable()) return;
    worker_ = std::jthread{[this](const std::stop_token token) { run(token); }};
    auto next = std::make_shared<SupportBundleServiceSnapshot>(*snapshot_);
    next->running = true;
    next->status = "Ready to create a privacy-safe local support bundle";
    next->generation = ++generation_;
    snapshot_ = std::move(next);
}

void SupportBundleService::stop() noexcept {
    std::jthread worker;
    {
        const std::scoped_lock lock{mutex_};
        if (!worker_.joinable()) return;
        worker_.request_stop();
        available_.notify_all();
        worker = std::move(worker_);
    }
    if (worker.joinable()) worker.join();
    publish(false, "Support bundle service stopped");
}

void SupportBundleService::create(SupportBundleRequest request) {
    const std::scoped_lock lock{mutex_};
    if (!worker_.joinable()) {
        auto next = std::make_shared<SupportBundleServiceSnapshot>(*snapshot_);
        next->status = "Support bundle service is not running";
        next->generation = ++generation_;
        snapshot_ = std::move(next);
        return;
    }
    if (pending_ || snapshot_->busy) {
        auto next = std::make_shared<SupportBundleServiceSnapshot>(*snapshot_);
        next->status = "A support bundle is already in progress";
        next->generation = ++generation_;
        snapshot_ = std::move(next);
        return;
    }
    pending_ = std::move(request);
    auto next = std::make_shared<SupportBundleServiceSnapshot>(*snapshot_);
    next->busy = true;
    next->status = "Creating support bundle";
    next->generation = ++generation_;
    snapshot_ = std::move(next);
    available_.notify_one();
}

std::shared_ptr<const SupportBundleServiceSnapshot>
SupportBundleService::snapshot() const {
    const std::scoped_lock lock{mutex_};
    return snapshot_;
}

void SupportBundleService::run(const std::stop_token token) noexcept {
    while (!token.stop_requested()) {
        SupportBundleRequest request;
        {
            std::unique_lock lock{mutex_};
            static_cast<void>(available_.wait(lock, token,
                                              [this] { return pending_.has_value(); }));
            if (!pending_) continue;
            request = std::move(*pending_);
            pending_.reset();
        }
        const auto result = create_support_bundle(request);
        if (result) {
            publish(false, "Support bundle created locally with " +
                               std::to_string(result->files) + " files");
        } else {
            publish(false, "Support bundle failed: " + result.error().message);
        }
    }
}

void SupportBundleService::publish(const bool busy, std::string status) {
    const std::scoped_lock lock{mutex_};
    auto next = std::make_shared<SupportBundleServiceSnapshot>(*snapshot_);
    next->running = worker_.joinable();
    next->busy = busy;
    next->status = std::move(status);
    next->generation = ++generation_;
    snapshot_ = std::move(next);
}

} // namespace blackbox::app
