#include "ui/incident_summary.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace blackbox::ui {
namespace {
std::string line_text(const std::string& text) {
    auto result = text.substr(0, 4096);
    for (auto& value : result) {
        if (static_cast<unsigned char>(value) < 32U || value == 127) value = ' ';
    }
    return result;
}

void metric(std::ostringstream& out, const char* name, const IncidentPlotSeries& series,
            const char* unit) {
    out << name << ": ";
    std::optional<double> peak;
    for (const auto value : series.values) {
        if (std::isfinite(value) && (!peak || value > *peak)) peak = value;
    }
    if (peak)
        out << "peak " << *peak << ' ' << unit;
    else
        out << "unavailable";
    const auto& counts = series.availability.by_status;
    out << " (available " << counts[0] << ", unsupported " << counts[1] << ", inaccessible "
        << counts[2] << ", temporary " << counts[3] << ")\n";
}
} // namespace

std::string format_incident_summary(const IncidentDetailView& detail,
                                    const bool include_annotations) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(2);
    out << "BlackBox incident summary\nIncident: " << detail.id
        << "\nCreated (UTC): " << line_text(detail.created_utc) << '\n';
    if (include_annotations) {
        out << "Label: " << line_text(detail.label) << "\nNote: " << line_text(detail.note) << '\n';
    }
    out << "\nCoverage (seconds relative to marker)\nRequested: " << detail.requested_start_seconds
        << " to " << detail.requested_end_seconds << "\nRecorded: " << detail.actual_start_seconds
        << " to " << detail.actual_end_seconds << "\nSystem samples: " << detail.system_sample_count
        << "\nProcess samples: " << detail.process_sample_count
        << "\nTriggers: " << detail.manual_trigger_count << " manual, "
        << detail.automatic_trigger_count << " automatic\n\nObserved metrics\n";
    metric(out, "CPU", detail.cpu_percent, "%");
    metric(out, "Memory", detail.memory_percent, "%");
    metric(out, "Storage read", detail.disk_read_mib_per_second, "MiB/s");
    metric(out, "Storage write", detail.disk_write_mib_per_second, "MiB/s");
    metric(out, "Network receive", detail.network_receive_mib_per_second, "MiB/s");
    metric(out, "Network transmit", detail.network_transmit_mib_per_second, "MiB/s");
    const auto& diagnosis = detail.analysis.diagnosis;
    out << "\nExplanation: "
        << (diagnosis.available ? line_text(diagnosis.incident_type) : "Unknown")
        << "\nAnalysis status: " << line_text(detail.analysis.status) << "\nConfidence: "
        << (diagnosis.available ? line_text(diagnosis.confidence) : "Unavailable")
        << "\nEvidence coverage: " << diagnosis.evidence_coverage * 100.0 << "%"
        << "\nBasis: " << line_text(diagnosis.basis)
        << "\nInterpretation: statistical association does not establish cause.\n";
    // Structured counts avoid accidentally exporting identity-bearing analysis text.
    out << "\nPotential contributors: " << detail.analysis.contributors.size()
        << "\nSystem events: " << detail.system_events.size()
        << "\nExecutable paths, process identities, raw events, and raw samples are omitted.\n";
    if (!include_annotations) out << "Labels and notes are omitted.\n";
    return out.str();
}
} // namespace blackbox::ui
