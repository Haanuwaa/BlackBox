#include "app/incident_viewer_service.hpp"
#include "core/filesystem_text.hpp"
#include "ui/incident_summary.hpp"

#include <fstream>

namespace blackbox::app {

bool IncidentViewerService::export_summary(std::shared_ptr<const ui::IncidentViewerContent> content,
                                           std::filesystem::path destination,
                                           const bool include_annotations) {
    Job job{};
    job.type = JobType::summary_export;
    job.export_content = std::move(content);
    job.destination = std::move(destination);
    job.include_annotations = include_annotations;
    return enqueue(std::move(job));
}

bool IncidentViewerService::handle_summary_export(const Job& job) {
    if (!job.export_content || !job.export_content->detail || job.destination.empty()) {
        publish_error("Summary export needs a loaded incident and a new destination file");
        return false;
    }
    const auto text =
        ui::format_incident_summary(*job.export_content->detail, job.include_annotations);
    std::ofstream output{job.destination, std::ios::out | std::ios::binary | std::ios::noreplace};
    if (!output) {
        publish_error("Summary export destination exists or cannot be created");
        return false;
    }
    output << text;
    output.flush();
    if (!output) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(job.destination, ignored);
        publish_error("Summary export could not be written completely");
        return false;
    }
    auto state = *snapshot();
    state.status = "Summary exported to " + core::path_to_utf8(job.destination);
    publish(std::move(state));
    return true;
}
} // namespace blackbox::app
