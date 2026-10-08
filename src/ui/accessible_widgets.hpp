#pragma once
#include "core/accessibility_tree.hpp"
#include <imgui.h>

namespace blackbox::ui::accessible {
void begin_frame(core::AccessibilityTree* tree);
void end_frame();
bool Button(const char* label, const ImVec2& size = ImVec2(0, 0));
bool Checkbox(const char* label, bool* value);
bool Selectable(const char* label, bool selected = false, ImGuiSelectableFlags flags = 0,
                const ImVec2& size = ImVec2(0, 0));
bool CollapsingHeader(const char* label, ImGuiTreeNodeFlags flags = 0);
bool Combo(const char* label, int* current, const char* const items[], int count, int height = -1);
bool InputText(const char* label, char* buffer, std::size_t capacity, ImGuiInputTextFlags flags = 0);
bool InputTextWithHint(const char* label, const char* hint, char* buffer, std::size_t capacity,
                       ImGuiInputTextFlags flags = 0);
bool InputTextMultiline(const char* label, char* buffer, std::size_t capacity,
                        const ImVec2& size = ImVec2(0, 0), ImGuiInputTextFlags flags = 0);
bool InputScalar(const char* label, ImGuiDataType type, void* value, const void* step = nullptr,
                 const void* fast_step = nullptr, const char* format = nullptr,
                 ImGuiInputTextFlags flags = 0);
void Text(const char* format, ...);
void LiveTextWrapped(const char* format, ...);
void TextWrapped(const char* format, ...);
void TextDisabled(const char* format, ...);
void TextColored(const ImVec4& color, const char* format, ...);
void TextUnformatted(const char* text, const char* end = nullptr);
} // namespace blackbox::ui::accessible
