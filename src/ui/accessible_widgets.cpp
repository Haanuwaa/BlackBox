#include "ui/accessible_widgets.hpp"
#include "ui/product_font.hpp"
#include <imgui_internal.h>
#include <charconv>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <optional>

namespace blackbox::ui::accessible {
namespace {
using core::AccessibleAction;
using core::AccessibleRole;
thread_local core::AccessibilityTree* tree{};
thread_local std::vector<core::AccessibleNode> nodes;
thread_local std::deque<core::AccessibleRequest> requests;
thread_local std::string previous_text;

std::string name_for(const char* label) {
    std::string name{label};
    if (const auto hash = name.find("##"); hash != std::string::npos) {
        if (hash == 0U) name = previous_text.empty() ? name.substr(2) : previous_text;
        else name.resize(hash);
    }
    if (name.size() > 512U) {
        std::size_t end = 512U;
        while (end != 0U && (static_cast<unsigned char>(name[end]) & 0xC0U) == 0x80U) --end;
        name.resize(end);
    }
    return name;
}
std::optional<core::AccessibleRequest> take(const char* label) {
    if (!tree) return {};
    const auto id = ImGui::GetID(label);
    std::optional<core::AccessibleRequest> action;
    for (auto current = requests.begin(); current != requests.end();) {
        if (current->id != id || current->name != name_for(label)) { ++current; continue; }
        auto request = std::move(*current);
        current = requests.erase(current);
        auto* modal = ImGui::GetTopMostPopupModal();
        if (modal && !ImGui::IsWindowWithinBeginStackOf(ImGui::GetCurrentWindow(), modal)) continue;
        if ((ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0) continue;
        if (request.action == AccessibleAction::focus) {
            ImGui::SetScrollHereY();
            ImGui::SetKeyboardFocusHere();
            ImGui::SetFocusID(id, ImGui::GetCurrentWindow());
            ImGui::FocusWindow(ImGui::GetCurrentWindow());
        } else {
            // UIA commonly sends focus followed by invoke in one frame. Focus
            // must not consume the widget's actual action. Repeated value or
            // invoke requests coalesce to the most recent action for this frame.
            if (action && action->action == AccessibleAction::toggle && request.action == AccessibleAction::toggle)
                action.reset();
            else action = std::move(request);
        }
    }
    return action;
}
void record(const char* label, AccessibleRole role, std::string value = {},
            bool selected = false, bool expanded = false, bool read_only = false) {
    if (!tree || nodes.size() >= 2048U) return;
    const auto minimum = ImGui::GetItemRectMin();
    const auto maximum = ImGui::GetItemRectMax();
    auto id = ImGui::GetItemID();
    if (id == 0U) id = ImGui::GetID(static_cast<int>(nodes.size()) + 0x40000000);
    const bool modal_blocked = ImGui::GetTopMostPopupModal() != nullptr &&
        !ImGui::IsWindowWithinBeginStackOf(ImGui::GetCurrentWindow(), ImGui::GetTopMostPopupModal());
    nodes.push_back({id, role, name_for(label), std::move(value), minimum.x, minimum.y,
        maximum.x - minimum.x, maximum.y - minimum.y,
        !modal_blocked && (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) == 0,
        ImGui::IsItemFocused() || ImGui::IsItemActive(), !ImGui::IsItemVisible(),
        selected, expanded, read_only});
}
bool edit(const std::optional<core::AccessibleRequest>& request, char* buffer,
          std::size_t capacity, ImGuiInputTextFlags flags) {
    if (!request || request->action != AccessibleAction::set_value || capacity == 0U ||
        request->value.size() >= capacity || (flags & ImGuiInputTextFlags_ReadOnly) != 0) return false;
    ImGui::ClearActiveID();
    std::memcpy(buffer, request->value.c_str(), request->value.size() + 1U);
    return true;
}
void text(const char* format, va_list arguments, int style, const ImVec4& color = {}) {
    va_list copy;
    va_copy(copy, arguments);
    switch (style) {
    case 1: ImGui::TextWrappedV(format, arguments); break;
    case 2: ImGui::TextDisabledV(format, arguments); break;
    case 3: ImGui::TextColoredV(color, format, arguments); break;
    default: ImGui::TextV(format, arguments); break;
    }
    if (tree || product_font_coverage_probe_needed()) {
        char buffer[4096];
        std::vsnprintf(buffer, sizeof(buffer), format, copy);
        buffer[sizeof(buffer) - 1U] = '\0';
        request_product_font_coverage(buffer);
        record(buffer, AccessibleRole::text);
        if (tree) previous_text = buffer;
    }
    va_end(copy);
}
}
void begin_frame(core::AccessibilityTree* target) {
    tree = target && target->enabled() ? target : nullptr;
    nodes.clear(); previous_text.clear(); requests.clear();
    if (tree) requests = tree->take_requests();
}
void end_frame() { if (tree) tree->publish(std::move(nodes)); tree = nullptr; requests.clear(); }
bool Button(const char* label, const ImVec2& size) {
    request_product_font_coverage(label);
    const auto request = take(label);
    const bool clicked = ImGui::Button(label, size);
    record(label, AccessibleRole::button);
    return clicked || (request && request->action == AccessibleAction::invoke);
}
bool Checkbox(const char* label, bool* value) {
    request_product_font_coverage(label);
    const auto request = take(label);
    bool changed = ImGui::Checkbox(label, value);
    if (request && request->action == AccessibleAction::toggle) { *value = !*value; changed = true; }
    record(label, AccessibleRole::checkbox, *value ? "On" : "Off", *value);
    return changed;
}
bool Selectable(const char* label, bool selected, ImGuiSelectableFlags flags, const ImVec2& size) {
    request_product_font_coverage(label);
    const auto request = take(label);
    const bool clicked = ImGui::Selectable(label, selected, flags, size);
    record(label, AccessibleRole::item, {}, selected);
    return clicked || (request && (request->action == AccessibleAction::invoke || request->action == AccessibleAction::select));
}
bool CollapsingHeader(const char* label, ImGuiTreeNodeFlags flags) {
    request_product_font_coverage(label);
    const auto request = take(label);
    if (request && (request->action == AccessibleAction::expand || request->action == AccessibleAction::collapse))
        ImGui::SetNextItemOpen(request->action == AccessibleAction::expand, ImGuiCond_Always);
    const bool open = ImGui::CollapsingHeader(label, flags);
    record(label, AccessibleRole::disclosure, {}, false, open);
    return open;
}
bool Combo(const char* label, int* current, const char* const items[], int count, int height) {
    request_product_font_coverage(label);
    const auto request = take(label);
    bool changed = false;
    if (request && request->action == AccessibleAction::set_value)
        for (int index = 0; index < count; ++index)
            if (request->value == items[index]) { *current = index; changed = true; break; }
    changed = ImGui::Combo(label, current, items, count, height) || changed;
    record(label, AccessibleRole::choice, *current >= 0 && *current < count ? items[*current] : "");
    return changed;
}
bool InputText(const char* label, char* buffer, std::size_t capacity, ImGuiInputTextFlags flags) {
    request_product_font_coverage(buffer);
    const bool changed = edit(take(label), buffer, capacity, flags);
    const bool entered = ImGui::InputText(label, buffer, capacity, flags);
    record(label, AccessibleRole::edit, buffer, false, false, (flags & ImGuiInputTextFlags_ReadOnly) != 0);
    return entered || changed;
}
bool InputTextWithHint(const char* label, const char* hint, char* buffer, std::size_t capacity, ImGuiInputTextFlags flags) {
    if (tree) previous_text = hint;
    request_product_font_coverage(buffer);
    const bool changed = edit(take(label), buffer, capacity, flags);
    const bool entered = ImGui::InputTextWithHint(label, hint, buffer, capacity, flags);
    record(label, AccessibleRole::edit, buffer, false, false, (flags & ImGuiInputTextFlags_ReadOnly) != 0);
    return entered || changed;
}
bool InputTextMultiline(const char* label, char* buffer, std::size_t capacity, const ImVec2& size, ImGuiInputTextFlags flags) {
    request_product_font_coverage(buffer);
    const bool changed = edit(take(label), buffer, capacity, flags);
    const bool entered = ImGui::InputTextMultiline(label, buffer, capacity, size, flags);
    record(label, AccessibleRole::edit, buffer, false, false, (flags & ImGuiInputTextFlags_ReadOnly) != 0);
    return entered || changed;
}
bool InputScalar(const char* label, ImGuiDataType type, void* value, const void* step,
                 const void* fast_step, const char* format, ImGuiInputTextFlags flags) {
    request_product_font_coverage(label);
    const auto request = take(label);
    bool changed = false;
    if (request && request->action == AccessibleAction::set_value && type == ImGuiDataType_U64 &&
        (flags & ImGuiInputTextFlags_ReadOnly) == 0) {
        std::uint64_t number{};
        const auto [end, error] = std::from_chars(request->value.data(), request->value.data() + request->value.size(), number);
        if (error == std::errc{} && end == request->value.data() + request->value.size()) {
            *static_cast<std::uint64_t*>(value) = number; changed = true;
        }
    }
    changed = ImGui::InputScalar(label, type, value, step, fast_step, format, flags) || changed;
    record(label, AccessibleRole::edit, type == ImGuiDataType_U64 ? std::to_string(*static_cast<std::uint64_t*>(value)) : "",
           false, false, type != ImGuiDataType_U64 || (flags & ImGuiInputTextFlags_ReadOnly) != 0);
    return changed;
}
void Text(const char* format, ...) { va_list args; va_start(args, format); text(format, args, 0); va_end(args); }
void LiveTextWrapped(const char* format, ...) { va_list args; va_start(args, format); text(format, args, 1); va_end(args); if (tree && !nodes.empty()) nodes.back().live = true; }
void TextWrapped(const char* format, ...) { va_list args; va_start(args, format); text(format, args, 1); va_end(args); }
void TextDisabled(const char* format, ...) { va_list args; va_start(args, format); text(format, args, 2); va_end(args); }
void TextColored(const ImVec4& color, const char* format, ...) { va_list args; va_start(args, format); text(format, args, 3, color); va_end(args); }
void TextUnformatted(const char* value, const char* end) {
    request_product_font_coverage(end ? std::string_view{value, static_cast<std::size_t>(end - value)} : std::string_view{value});
    ImGui::TextUnformatted(value, end);
    if (tree) {
        const std::string label = end ? std::string(value, end) : std::string(value);
        record(label.c_str(), AccessibleRole::text);
        previous_text = label;
    }
}
} // namespace blackbox::ui::accessible
