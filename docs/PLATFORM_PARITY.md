# Platform parity

BlackBox keeps one portable recorder, incident, storage, analysis, and UI graph. “Parity” therefore
means equivalent product outcomes behind platform adapters; it does not mean forcing every operating
system to expose the same native counters. An unavailable metric remains explicit and must never be
fabricated from an unrelated source.

Windows is the only intended V1.0 product target. Linux and macOS rows below describe engineering
coverage until their physical, packaging, reliability, privacy, and support gates are independently
qualified.

| Outcome / boundary | Windows | Linux engineering preview | macOS engineering preview |
| --- | --- | --- | --- |
| Portable recorder, capture, schema-v1 archive, analysis, UI | Implemented | Same portable graph | Same portable graph |
| System CPU and memory | Native | Native `/proc` | Native Mach/sysctl |
| Process identity, CPU, RSS, and disk I/O | Native | Native `/proc` | Native libproc |
| System disk/network throughput | Native | Native `/proc` and `/sys` | Open |
| Disk latency/queue/service evidence | Native | Open | Open |
| Network connectivity/transport quality | Native | Open | Open |
| GPU, foreground, and responsiveness evidence | Native, capability-gated | Open | Open |
| Power source, battery, frequency, thermal, uptime | Native, capability-gated | Open | Open |
| Privacy-reduced symptom/system events | Native, independently gated | Open | Open |
| Tray/background controls and single-instance enforcement | Native | Native SDL/POSIX | Native SDL/POSIX |
| Desktop notifications | Native | Bounded session D-Bus queue | Bounded, permission-aware UserNotifications |
| Launch at login | Current-user Run value | Exact owned XDG entry | Current `SMAppService` main-app registration |
| Global incident shortcut | Native registration | XDG GlobalShortcuts portal | Open; no legacy Carbon fallback |
| Increased contrast and reduced motion | Native | Open; XDG Settings portal selected | Native AppKit preferences |
| Crash evidence | Bounded native minidump | Open | Open |
| Engineering package | Portable ZIP | TGZ, DEB, and RPM | Native `.app` in unsigned TGZ |
| Hosted native compiler/provider/package checks | Windows runners | Ubuntu, Debian, Fedora, X11, Wayland | Apple Silicon and Intel passed on `52d6c8b` |
| Physical desktop and long-running qualification | Incomplete release gate | Not started | Not started |
| Production support claim | Intended V1.0 target, not yet released | None | None |

## Ordered parity work

1. Keep the Windows release revision and qualification evidence isolated from cross-platform edits.
2. Complete and retain the native macOS shell/bundle workflow on Apple Silicon and Intel.
3. Add macOS system network throughput plus power-source, battery, and uptime evidence through native
   low-cost APIs, with provider-contract and overhead checks before claiming each capability.
4. Add Linux increased-contrast/reduced-motion state through XDG Settings without blocking the UI or
   collection thread, then add power/session evidence through similarly bounded native adapters.
5. Add Linux/macOS crash evidence behind `ICrashDiagnostics`; crash publication must remain independent
   of SQLite and safe during partial startup.
6. Decide the macOS global-shortcut product flow around current accessibility permission requirements.
   A passive Quartz event tap is technically available but is not equivalent to conflict-aware shortcut
   registration and must not be enabled merely to fill a table cell.
7. Run physical GNOME/KDE and macOS client matrices, accessibility/DPI review, sleep/resume and long-run
   campaigns, then design signed/notarized distribution. Hosted compilation alone cannot establish
   product support.

Runtime ML is intentionally outside platform parity. The recorder and normalized incident format stay
identical with ML absent; a native optional model should be considered only after representative held-out
evidence beats the existing statistical pipeline under the predeclared accuracy and overhead gates.

## Native API references

- [SDL3 tray API](https://wiki.libsdl.org/SDL3/SDL_CreateTray)
- [Apple ServiceManagement `SMAppService`](https://developer.apple.com/documentation/servicemanagement/smappservice)
- [Apple local-notification authorization](https://developer.apple.com/documentation/usernotifications/asking-permission-to-use-notifications)
- [Apple AppKit accessibility display preferences](https://developer.apple.com/documentation/appkit/nsworkspace/accessibilitydisplayshouldincreasecontrast)
- [XDG Global Shortcuts portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.GlobalShortcuts.html)
- [XDG Settings portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Settings.html)
- [Apple Quartz event taps](https://developer.apple.com/documentation/coregraphics/cgevent/tapcreate%28tap%3Aplace%3Aoptions%3Aeventsofinterest%3Acallback%3Auserinfo%3A%29)
