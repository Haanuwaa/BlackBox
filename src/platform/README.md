# Platform module

Non-telemetry operating-system services live here behind platform-independent interfaces.

`IGlobalHotkeyManager` contains no native types. Its Windows implementation owns a dedicated message
thread, registers the configured chord with `RegisterHotKey`, converts `WM_HOTKEY` into an
application callback, and unregisters/joins before collector shutdown. Registration conflicts are
data exposed to the UI; the capture button and tray capture command remain available.

V0.11 adds `IBackgroundShell`, a portable command/status/diagnostics contract. The Windows
implementation alone owns the hidden message window, notification-area API, current-user startup
registry value, Explorer `TaskbarCreated` recovery, end-session handling, and named single-instance
mutex. Its callback emits bounded commands to the application composition root; it cannot access
telemetry, the recorder, SQLite, analysis, SDL, or UI state. Notifications retain only one pending
bounded title/body pair and callback failures never escape the native message thread.
