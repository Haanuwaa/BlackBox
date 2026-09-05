#include "app/file_dialog_service.hpp"

#include <SDL3/SDL.h>
#include <utility>

namespace blackbox::app {

FileDialogService::FileDialogService() : state_{std::make_shared<State>()} {}

void FileDialogService::show(SDL_Window* parent, const FileDialogKind kind,
                             const std::uint32_t field, std::string default_location) {
    struct Request {
        std::weak_ptr<State> state;
        std::uint32_t field;
        std::string location;
    };
    auto request = std::make_unique<Request>(Request{state_, field, std::move(default_location)});
    {
        const std::scoped_lock lock{state_->mutex};
        if (state_->pending) return;
        state_->pending = true;
    }
    const auto callback = [](void* context, const char* const* files, int) {
        const std::unique_ptr<Request> completed{static_cast<Request*>(context)};
        const auto state = completed->state.lock();
        if (!state) return;
        const std::scoped_lock lock{state->mutex};
        state->pending = false;
        try {
            FileDialogResult result{completed->field};
            if (files == nullptr)
                result.error = SDL_GetError();
            else if (files[0] != nullptr)
                result.path = files[0];
            state->result = std::move(result);
        } catch (...) {
            // Allocation failure cannot cross SDL's native callback boundary.
            state->result.reset();
        }
    };
    static constexpr SDL_DialogFileFilter all_files[]{{"All files", "*"}};
    auto* context = request.release(); // SDL invokes the callback exactly once.
    switch (kind) {
    case FileDialogKind::open_file:
        SDL_ShowOpenFileDialog(callback, context, parent, all_files, 1, context->location.c_str(),
                               false);
        break;
    case FileDialogKind::save_file:
        SDL_ShowSaveFileDialog(callback, context, parent, all_files, 1, context->location.c_str());
        break;
    case FileDialogKind::folder:
        SDL_ShowOpenFolderDialog(callback, context, parent, context->location.c_str(), false);
        break;
    }
}

std::optional<FileDialogResult> FileDialogService::take_result() {
    const std::scoped_lock lock{state_->mutex};
    if (!state_->result) return std::nullopt;
    auto result = std::move(*state_->result);
    state_->result.reset();
    return result;
}

bool FileDialogService::pending() const {
    const std::scoped_lock lock{state_->mutex};
    return state_->pending;
}
} // namespace blackbox::app
