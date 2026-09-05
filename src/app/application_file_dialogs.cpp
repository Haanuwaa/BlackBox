#include "app/application.hpp"
#include "core/filesystem_text.hpp"
#include "ui/dashboard_settings.hpp"

#include <algorithm>

namespace blackbox::app {

void Application::request_file_dialog(const ui::PathField field) {
    auto kind = FileDialogKind::save_file;
    if (field == ui::PathField::restore) kind = FileDialogKind::open_file;
    if (field == ui::PathField::dataset || field == ui::PathField::support_bundle)
        kind = FileDialogKind::folder;
    auto location = std::string{ui::detail::path_buffer(product_ui_state_, field).data()};
    if (kind == FileDialogKind::folder)
        location = core::path_to_utf8(core::path_from_utf8(location).parent_path());
    file_dialog_service_.show(window_, kind, static_cast<std::uint32_t>(field),
                              std::move(location));
}

void Application::consume_file_dialog() {
    const auto result = file_dialog_service_.take_result();
    if (!result) return;
    if (!result->error.empty()) {
        product_ui_state_.file_dialog_status = "File picker: " + result->error;
        return;
    }
    if (result->path.empty()) {
        product_ui_state_.file_dialog_status = "Selection cancelled; previous path kept.";
        return;
    }
    const auto field = static_cast<ui::PathField>(result->field);
    auto path = result->path;
    if (field == ui::PathField::dataset || field == ui::PathField::support_bundle) {
        path = core::path_to_utf8(core::path_from_utf8(path) / (field == ui::PathField::dataset
                                                                    ? "blackbox-evidence-dataset"
                                                                    : "blackbox-support-bundle"));
    }
    auto& buffer = ui::detail::path_buffer(product_ui_state_, field);
    if (path.size() >= buffer.size()) {
        product_ui_state_.file_dialog_status =
            "Selected path exceeds the 1024-byte UTF-8 limit; previous path kept.";
        return;
    }
    buffer.fill('\0');
    std::copy(path.begin(), path.end(), buffer.begin());
    product_ui_state_.file_dialog_status = "Path selected. Review it, then use the action button.";
}
} // namespace blackbox::app
