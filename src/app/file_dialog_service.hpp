#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

struct SDL_Window;

namespace blackbox::app {

enum class FileDialogKind : std::uint8_t { open_file, save_file, folder };
struct FileDialogResult {
    std::uint32_t field{};
    std::string path{};
    std::string error{};
};

// Native callbacks only publish an owned UTF-8 result; UI updates occur on the
// application thread. A late callback never dereferences a destroyed app.
class FileDialogService final {
public:
    FileDialogService();
    void show(SDL_Window* parent, FileDialogKind kind, std::uint32_t field,
              std::string default_location);
    [[nodiscard]] std::optional<FileDialogResult> take_result();
    [[nodiscard]] bool pending() const;

private:
    struct State {
        std::mutex mutex;
        bool pending{};
        std::optional<FileDialogResult> result{};
    };
    std::shared_ptr<State> state_;
};
} // namespace blackbox::app
