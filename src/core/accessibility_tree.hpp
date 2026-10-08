#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace blackbox::core {
enum class AccessibleRole { text, button, checkbox, edit, choice, item, disclosure };
enum class AccessibleAction { invoke, focus, set_value, toggle, expand, collapse, select };
struct AccessibleNode {
    std::uint32_t id{};
    AccessibleRole role{AccessibleRole::text};
    std::string name, value;
    double x{}, y{}, width{}, height{};
    bool enabled{true}, focused{}, offscreen{}, selected{}, expanded{}, read_only{};
    bool live{};
};
struct AccessibleRequest {
    std::uint32_t id{};
    AccessibleAction action{};
    std::string value;
    std::string name;
};
struct AccessibleSnapshot {
    std::vector<AccessibleNode> nodes;
    std::uint64_t generation{};
    bool available{true};
};

// Providers exchange immutable values with the UI thread; no widget pointers or
// blocking calls into the renderer are retained by an automation client.
class AccessibilityTree {
public:
    void enable() { const std::scoped_lock lock{mutex_}; enabled_ = true; }
    [[nodiscard]] bool enabled() const { const std::scoped_lock lock{mutex_}; return enabled_; }
    [[nodiscard]] std::shared_ptr<const AccessibleSnapshot> snapshot() const {
        const std::scoped_lock lock{mutex_}; return snapshot_;
    }
    void publish(std::vector<AccessibleNode> nodes, bool available = true) {
        if (nodes.size() > 2048U) nodes.resize(2048U);
        auto next = std::make_shared<AccessibleSnapshot>();
        next->nodes = std::move(nodes);
        next->available = available;
        const std::scoped_lock lock{mutex_};
        next->generation = snapshot_->generation + 1U;
        snapshot_ = std::move(next);
        if (!available) requests_.clear();
    }
    [[nodiscard]] bool request(AccessibleRequest request) {
        const std::scoped_lock lock{mutex_};
        if (!snapshot_->available || requests_.size() >= 64U || request.value.size() > 4096U) return false;
        const auto node = std::find_if(snapshot_->nodes.begin(), snapshot_->nodes.end(),
            [&](const auto& item) { return item.id == request.id; });
        if (node == snapshot_->nodes.end() || !node->enabled ||
            (request.action == AccessibleAction::set_value && node->read_only)) return false;
        const bool allowed =
            (request.action == AccessibleAction::focus && node->role != AccessibleRole::text) ||
            (request.action == AccessibleAction::invoke && (node->role == AccessibleRole::button || node->role == AccessibleRole::item)) ||
            (request.action == AccessibleAction::set_value && (node->role == AccessibleRole::edit || node->role == AccessibleRole::choice)) ||
            (request.action == AccessibleAction::toggle && node->role == AccessibleRole::checkbox) ||
            ((request.action == AccessibleAction::expand || request.action == AccessibleAction::collapse) && node->role == AccessibleRole::disclosure) ||
            (request.action == AccessibleAction::select && node->role == AccessibleRole::item);
        if (!allowed || request.value.find('\0') != std::string::npos) return false;
        request.name = node->name;
        requests_.push_back(std::move(request));
        return true;
    }
    [[nodiscard]] std::deque<AccessibleRequest> take_requests() {
        const std::scoped_lock lock{mutex_};
        auto result = std::move(requests_);
        requests_.clear();
        return result;
    }
private:
    mutable std::mutex mutex_;
    bool enabled_{};
    std::shared_ptr<const AccessibleSnapshot> snapshot_{std::make_shared<AccessibleSnapshot>()};
    std::deque<AccessibleRequest> requests_;
};
} // namespace blackbox::core
