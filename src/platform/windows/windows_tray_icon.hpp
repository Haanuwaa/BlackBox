#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include "platform/background_shell.hpp"

namespace blackbox::platform::windows {
inline constexpr unsigned int brand_icon_resource = 101U;
[[nodiscard]] inline NOTIFYICONDATAW tray_icon_data(HWND window, unsigned int id,
    BackgroundShellStatus status) noexcept {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = id;
    // Version 4 suppresses the standard hover tooltip unless SHOWTIP is set.
    data.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    unsigned int resource = 102U;
    const wchar_t* tip = L"BlackBox - recording";
    switch (status) {
    case BackgroundShellStatus::recording: break;
    case BackgroundShellStatus::capturing: resource = 103U; tip = L"BlackBox - capturing incident"; break;
    case BackgroundShellStatus::paused: resource = 104U; tip = L"BlackBox - recording paused"; break;
    case BackgroundShellStatus::retrying_storage: resource = 105U; tip = L"BlackBox - retrying incident storage"; break;
    case BackgroundShellStatus::error: resource = 106U; tip = L"BlackBox - attention required"; break;
    }
    data.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(resource), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (data.hIcon == nullptr) data.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    for (unsigned int index = 0;
         index + 1U < sizeof(data.szTip)/sizeof(data.szTip[0]) && tip[index] != L'\0'; ++index)
        data.szTip[index] = tip[index];
    return data;
}
} // namespace blackbox::platform::windows
