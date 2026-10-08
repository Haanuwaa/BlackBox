#pragma once
#include "core/accessibility_tree.hpp"
#include <memory>

namespace blackbox::platform::windows {
class WindowsUiAutomation {
public:
    WindowsUiAutomation();
    ~WindowsUiAutomation();
    WindowsUiAutomation(const WindowsUiAutomation&) = delete;
    WindowsUiAutomation& operator=(const WindowsUiAutomation&) = delete;
    bool attach(void* window, std::shared_ptr<core::AccessibilityTree> tree);
    void notify_changes();
    void detach() noexcept;
private:
    struct Native;
    std::unique_ptr<Native> native_;
};
} // namespace blackbox::platform::windows
