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
| GPU and responsiveness evidence | Native counters plus non-software DXGI inventory with adapter type explicitly unknown | Capability-driven AMD sysfs and optional runtime-loaded NVIDIA NVML whole-system usage/memory; privacy-bounded DRM `fdinfo` foreground activity; inventory type explicitly unknown | Public non-identifying Metal device inventory plus active SDL renderer evidence; passive whole-system and foreground utilization explicitly unsupported |
| Power source, battery, frequency, thermal, uptime | Native, capability-gated | Native power/battery/uptime, weighted CPU policy frequency, and ACPI platform-profile saver state; thermal unavailable | Native power/battery/uptime, Low Power Mode, and public coarse thermal-pressure state; CPU frequency unavailable |
| Resource-pressure evidence | Windows responsiveness evidence stays separate; cumulative-stall channels unsupported | Exact per-interval CPU/memory/I/O PSI `some`/`full` fractions from cumulative totals | Exact cumulative-stall channels unsupported; coarse thermal state stays separate |
| Native suspend/resume lifecycle evidence | Native power notifications | Native logind `PrepareForSleep`; explicit partial status without system D-Bus/logind | Native IOKit system-power notifications |
| Privacy-reduced symptom/system events | Native, independently gated | Identifier-free device/audio/storage/display/network uevents, logind power, systemd job result class, and coredump crash marker | Identifier-free application lifecycle, audio/default, storage, display, network, and power context; general service events unsupported |
| Tray/background controls and single-instance enforcement | Native | Native SDL/POSIX plus coalesced XDG Background portal status; exact XDG autostart remains authoritative | Native SDL/POSIX |
| Desktop notifications | Native | Bounded XDG Notification portal queue with freedesktop-service fallback and service rediscovery | Bounded, permission-aware UserNotifications |
| Launch at login | Current-user Run value | Exact owned XDG entry | Current `SMAppService` main-app registration |
| Global incident shortcut | Native registration | Versioned XDG GlobalShortcuts session with closure/service-loss recovery and user-removal detection | AppKit global/local key monitor with Input Monitoring permission; passive and not conflict-aware |
| Increased contrast and reduced motion | Native | Nonblocking XDG Settings portal | Native AppKit preferences |
| DPI and display membership | Per-monitor SDL/PerMonitorV2 | SDL Wayland display-scale/pixel-size/membership events; canonical font/style rebuild | SDL native high-density display-scale/pixel-size/membership events |
| Crash evidence | Bounded native minidump | Fixed POSIX signal record | Fixed POSIX signal record |
| Engineering package | Portable ZIP | TGZ, DEB, and RPM with hosted install/launch/uninstall lifecycle | Native `.app` in unsigned TGZ, DMG, and PKG; optional Developer ID/notary hooks |
| Hosted native compiler/provider/package checks | V0.22 Windows matrix passed on `2d7ac36` | V0.22 Ubuntu, Debian, Fedora, package lifecycle, Weston/Mutter/KWin/Sway, PSI, TSan, fuzz, and coverage passed on `2d7ac36` | V0.22 Apple Silicon and Intel telemetry/package matrix passed on `2d7ac36` |
| Physical desktop and long-running qualification | Incomplete release gate | Not started | Not started |
| Production support claim | Intended V1.0 target, not yet released | None | None |

The V0.22 row is bound to exact revision `2d7ac36b49a76011309b10b74ba9ca70bba39333`:
[Windows run 33465385573](https://github.com/Haanuwaa/BlackBox/actions/runs/33465385573),
[Linux run 33465385795](https://github.com/Haanuwaa/BlackBox/actions/runs/33465385795),
[macOS run 33465385552](https://github.com/Haanuwaa/BlackBox/actions/runs/33465385552), and
[quality/security run 33465385553](https://github.com/Haanuwaa/BlackBox/actions/runs/33465385553).

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
4. Physically validate Linux GPU coverage on AMD, NVIDIA, Intel, hybrid, permission-restricted, and
   hotplug-capable hosts. AMD and NVIDIA provide whole-system device gauges; DRM `fdinfo` supplies
   only readable foreground-client activity and is never relabeled as complete host utilization.
   Keep macOS passive whole-system GPU utilization unsupported. Linux PSI is now implemented against
   `PRESSURE_CONTRACT.md`; physically validate its reset, permission, suspend/resume, and overhead
   behavior. macOS cumulative-stall pressure remains unsupported; its coarse thermal state is not a
   PSI substitute. PSI is not Windows DPC/ISR activity.
5. Retain and physically qualify portal permission/denial/restart behavior, the implemented
   Linux/macOS telemetry, accessibility, crash, background, notification, autostart, package,
   sleep/resume, and shortcut boundaries. Hosted compositor smoke cannot exercise a real user's
   permission dialogs or notification/tray shell extensions.
6. Run physical GNOME/KDE/Sway and macOS client matrices, fractional-scale and mixed-scale
   accessibility/DPI review, sleep/resume and long-run
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
- [XDG Notification portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Notification.html)
- [XDG Background portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Background.html)
- [SDL3 high-DPI guidance](https://wiki.libsdl.org/SDL3/README-highdpi)
- [Apple Quartz event taps](https://developer.apple.com/documentation/coregraphics/cgevent/tapcreate%28tap%3Aplace%3Aoptions%3Aeventsofinterest%3Acallback%3Auserinfo%3A%29)
- [Apple AppKit global event monitors](https://developer.apple.com/documentation/appkit/nsevent/addglobalmonitorforevents%28matching%3Ahandler%3A%29)
- [Apple event-listening permission](https://developer.apple.com/forums/thread/811443)
- [Apple `NSWorkspace.frontmostApplication`](https://developer.apple.com/documentation/appkit/nsworkspace/frontmostapplication)
- [Apple Low Power Mode notifications](https://developer.apple.com/documentation/xcode/responding-to-power-notifications)
- [Apple Metal GPU counters](https://developer.apple.com/documentation/metal/gpu-counters-and-counter-sample-buffers)
- [Apple Metal device discovery](https://developer.apple.com/documentation/metal/1433401-mtlcopyalldevices)
- [Linux DRM client usage statistics](https://docs.kernel.org/gpu/drm-usage-stats.html)
- [Linux AMDGPU monitoring](https://docs.kernel.org/gpu/amdgpu/thermal.html)
- [NVIDIA NVML utilization](https://docs.nvidia.com/deploy/nvml-api/structnvmlUtilization__t.html)
- [Linux CPUFreq sysfs policy](https://docs.kernel.org/admin-guide/pm/cpufreq.html)
- [systemd Manager D-Bus API](https://www.freedesktop.org/software/systemd/man/org.freedesktop.systemd1.html)
- [systemd-coredump journal identifier](https://www.freedesktop.org/software/systemd/man/systemd-coredump.html)
- [Apple display reconfiguration callback](https://developer.apple.com/documentation/coregraphics/cgdisplayregisterreconfigurationcallback%28_%3A_%3A%29)
- [Apple SystemConfiguration dynamic store](https://developer.apple.com/documentation/systemconfiguration/scdynamicstore)
- [Linux platform profile](https://docs.kernel.org/userspace-api/sysfs-platform_profile.html)
- [Linux Pressure Stall Information](https://docs.kernel.org/accounting/psi.html)
- [Extended Window Manager Hints](https://specifications.freedesktop.org/wm/latest-single/)
- [XDG Desktop Portal API reference](https://flatpak.github.io/xdg-desktop-portal/docs/api-reference.html)
- [Apple system sleep/wake notifications](https://developer.apple.com/library/archive/qa/qa1340/_index.html)
