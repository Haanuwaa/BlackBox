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
| System disk/network throughput | Native | Native `/proc` and `/sys` | Native BSD interfaces and IOKit block-driver statistics |
| Disk latency/queue/service evidence | Native | Native read/write/combined service and interval-average queue | Native read/write/combined service; exact queue unsupported |
| Network connectivity/transport quality | Native | Local-link transitions plus `/proc/net/snmp` TCP MIB | Local-link transitions plus native TCP send/retransmission/failure subset; exact established resets open |
| Foreground-application evidence | Native, capability-gated | Privacy-bounded X11 EWMH PID correlated to process creation identity; Wayland explicitly unsupported | Privacy-bounded `NSWorkspace` PID correlated to process creation identity |
| GPU and responsiveness evidence | Native, capability-gated | GPU explicitly unsupported; PSI is promising pressure evidence but is not DPC/ISR and needs a distinct portable contract | GPU explicitly unsupported; public Metal counters are app-owned, not passive whole-system evidence |
| Power source, battery, frequency, thermal, uptime | Native, capability-gated | Native power/battery/uptime, weighted CPU policy frequency, and ACPI platform-profile saver state; thermal open | Native power/battery/uptime and Low Power Mode; CPU frequency/thermal open |
| Native suspend/resume lifecycle evidence | Native power notifications | Native logind `PrepareForSleep`; explicit partial status without system D-Bus/logind | Native IOKit system-power notifications |
| Privacy-reduced symptom/system events | Native, independently gated | Identifier-free kernel device add/remove context | Identifier-free IOKit storage-media add/remove context |
| Tray/background controls and single-instance enforcement | Native | Native SDL/POSIX | Native SDL/POSIX |
| Desktop notifications | Native | Bounded session D-Bus queue | Bounded, permission-aware UserNotifications |
| Launch at login | Current-user Run value | Exact owned XDG entry | Current `SMAppService` main-app registration |
| Global incident shortcut | Native registration | XDG GlobalShortcuts portal | AppKit global/local key monitor with Input Monitoring permission; passive and not conflict-aware |
| Increased contrast and reduced motion | Native | Nonblocking XDG Settings portal | Native AppKit preferences |
| Crash evidence | Bounded native minidump | Fixed POSIX signal record | Fixed POSIX signal record |
| Engineering package | Portable ZIP | TGZ, DEB, and RPM | Native `.app` in unsigned TGZ |
| Hosted native compiler/provider/package checks | Windows runners passed on `59c1204` | Ubuntu, Debian, Fedora, X11, Wayland passed on `59c1204` | Apple Silicon and Intel passed on `59c1204` |
| Physical desktop and long-running qualification | Incomplete release gate | Not started | Not started |
| Production support claim | Intended V1.0 target, not yet released | None | None |

## Ordered parity work

1. Validate the implemented native Linux logind and macOS IOKit suspend/resume events on hosted native
   graphs, then physically exercise them during later desktop qualification. They use the existing
   independently scheduled, fixed-capacity event boundary, request cadence resynchronization, and do
   not manufacture incidents or rely on monotonic clocks that may pause during sleep.
2. Physically validate macOS Input Monitoring onboarding, denial/retry, local/global delivery, function-
   key behavior, and app restart. The AppKit monitor is intentionally labeled passive because it cannot
   detect conflicts or reserve the combination like Windows/XDG registration.
3. Physically validate Linux CPU-frequency/profile coverage across governors and hardware, macOS Low
   Power Mode transitions, and privacy-bounded foreground identity on macOS and X11. Wayland remains
   unsupported because its public portal API has no standardized active-window interface.
4. Design a separate portable pressure/responsiveness contract before considering Linux PSI. CPU,
   memory, and I/O stall pressure must not be relabeled as Windows DPC/ISR activity. Keep passive
   Linux/macOS whole-system GPU evidence unsupported until a documented cross-vendor public source
   satisfies exact semantics and bounded background collection cost.
5. Retain and physically qualify the implemented Linux/macOS telemetry, accessibility, crash,
   background, notification, autostart, package, sleep/resume, and shortcut boundaries.
6. Run physical GNOME/KDE and macOS client matrices, accessibility/DPI review, sleep/resume and long-run
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
- [Apple power-source snapshot](https://developer.apple.com/documentation/iokit/1523858-iopsgetprovidingpowersourcetype)
- [Apple sleep-inclusive continuous time](https://developer.apple.com/documentation/driverkit/mach_continuous_time)
- [Apple `getifaddrs` interface statistics](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/getifaddrs.3.html)
- [Apple `IOBlockStorageDriver` statistics](https://developer.apple.com/documentation/kernel/ioblockstoragedriver)
- [Linux block-device I/O statistics](https://docs.kernel.org/admin-guide/iostats.html)
- [Linux kernel uevent environment](https://docs.kernel.org/driver-api/driver-model/uevent.html)
- [systemd-logind `PrepareForSleep`](https://www.freedesktop.org/software/systemd/man/latest/org.freedesktop.login1.html)
- [Linux TCP SNMP counters](https://docs.kernel.org/networking/snmp_counter.html)
- [XDG Global Shortcuts portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.GlobalShortcuts.html)
- [XDG Settings portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Settings.html)
- [Apple Quartz event taps](https://developer.apple.com/documentation/coregraphics/cgevent/tapcreate%28tap%3Aplace%3Aoptions%3Aeventsofinterest%3Acallback%3Auserinfo%3A%29)
- [Apple AppKit global event monitors](https://developer.apple.com/documentation/appkit/nsevent/addglobalmonitorforevents%28matching%3Ahandler%3A%29)
- [Apple event-listening permission](https://developer.apple.com/forums/thread/811443)
- [Apple `NSWorkspace.frontmostApplication`](https://developer.apple.com/documentation/appkit/nsworkspace/frontmostapplication)
- [Apple Low Power Mode notifications](https://developer.apple.com/documentation/xcode/responding-to-power-notifications)
- [Apple Metal GPU counters](https://developer.apple.com/documentation/metal/gpu-counters-and-counter-sample-buffers)
- [Linux CPUFreq sysfs policy](https://docs.kernel.org/admin-guide/pm/cpufreq.html)
- [Linux platform profile](https://docs.kernel.org/userspace-api/sysfs-platform_profile.html)
- [Linux Pressure Stall Information](https://docs.kernel.org/accounting/psi.html)
- [Extended Window Manager Hints](https://specifications.freedesktop.org/wm/latest-single/)
- [XDG Desktop Portal API reference](https://flatpak.github.io/xdg-desktop-portal/docs/api-reference.html)
- [Apple system sleep/wake notifications](https://developer.apple.com/library/archive/qa/qa1340/_index.html)
