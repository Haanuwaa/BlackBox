#pragma once

#include "ui/incident_viewer.hpp"
#include <string>

namespace blackbox::ui {
[[nodiscard]] std::string format_incident_summary(const IncidentDetailView& detail,
                                                  bool include_annotations = false);
}
