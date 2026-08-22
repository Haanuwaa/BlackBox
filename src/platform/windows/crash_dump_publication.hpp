#pragma once

namespace blackbox::platform::windows::detail {

// The crash path has already closed and flushed the staging handle. Publication is
// allocation-free and retries only bounded transient Windows file contention.
[[nodiscard]] bool publish_completed_dump(const wchar_t* pending_path,
                                          const wchar_t* completed_path) noexcept;

} // namespace blackbox::platform::windows::detail
