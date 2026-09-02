# BlackBox

BlackBox is a lightweight, native computer flight recorder for answering a simple question: **what caused the short-lived performance problem that just happened?** It retains a bounded history of system behavior in memory, then saves a focused pre/post-incident window only when an incident is captured.

The long-term product will help diagnose freezes, game and audio stutter, disk spikes, hangs, network latency, and unexpected CPU/GPU changes. The recorder comes first; statistical and machine-learning analysis will consume recorded data later and will never be required for collection.

## Current status

Version **0.26.0** is the pre-1.0 platform-evidence engineering line. The product has one bounded
native recorder, direct-V1 archive, optional local statistical analysis, and an evidence-first
desktop workflow across its Windows product target and Linux/macOS engineering previews. The default
UI now leads with recorder readiness, capture, current activity, saved incidents, and plain-language
explanations; technical details remain available on demand. Recent-activity graphs use human time,
capture and archive workflows are keyboard accessible, and tray behavior is explained in context.

Visible rendering remains capped at 30 Hz while idle, but recent mouse, touch, or keyboard activity
temporarily raises presentation to 60 Hz for smooth scrolling and hover inspection. This changes only
presentation: collection, capture, persistence, and analysis remain independent of window lifetime
and frame cadence. V0.24's isolated comparison reduced visible average CPU by 79.1% without changing
recorder results or materially increasing memory; the adaptive path retains that idle policy.

Windows remains the intended first product target, but no platform has a public support claim before
the remaining physical, signing, long-run, and diagnostic-quality gates pass. Linux implements native
CPU/memory/process/disk/network/power/uptime evidence, capability-driven GPU backends, exact PSI
CPU/memory/I/O stall fractions, X11 foreground identity, Wayland portal integration, and TGZ/DEB/RPM
engineering packages. Generic Wayland process identity remains unavailable because the public portal
surface has no standardized permission-bounded active-window API. Advertising wlroots compositors
can instead supply an opaque session application key that cannot be correlated to process/GPU data.

macOS implements native CPU/memory/process/disk/network/power/uptime evidence, public Metal inventory,
BlackBox renderer health, separate coarse thermal and memory-pressure states, BlackBox-owned login-
item authorization, desktop integration, global-shortcut
permission UX, and unsigned TGZ/DMG/PKG engineering packages. It does not relabel renderer health as
whole-system GPU utilization or invent exact disk-queue/PSI evidence. Runtime ML remains unshipped:
the offline comparison harness exists, but adoption stays blocked until a representative,
independently labelled held-out corpus proves material value over the optional statistical baseline.

## Principles

- Extremely lightweight: low CPU and memory, effectively zero recording-time disk writes, no wasteful hidden UI.
- Native: C++23, no Electron, browser UI, Node.js, or shipped Python runtime.
- Cross-platform core: operating-system APIs stay behind platform interfaces.
- Windows first: prove the recorder before implementing additional backends.
- Recorder before intelligence: telemetry collection cannot depend on analysis or ML.
- Measure before optimizing: simple concurrency first; profile before adding complexity.

The central invariant is:

```text
TelemetryProvider -> Normalizer -> Recorder
```

This path must work with the UI, persistence, analysis, and future ML disabled.

## Dependencies

The vcpkg manifest pins the dependency registry and declares:

- SDL3
- Dear ImGui with SDL3 platform and renderer bindings
- ImPlot
- SQLite3
- Catch2

The UI uses SDL3's renderer backend. This avoids choosing a custom Vulkan or Direct3D renderer before requirements justify one.

## Build on Windows

Prerequisites:

- Windows 10 or later
- Visual Studio 2022 or 2026 with **Desktop development with C++**
- CMake 3.25 or later
- Git
- vcpkg checked out and bootstrapped

Set `VCPKG_ROOT` to the absolute path of the vcpkg checkout, then run from a Developer PowerShell:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
.\out\build\windows-msvc-debug\src\Debug\blackbox.exe
```

Use the matching `windows-msvc-release` presets for a release build. The first configure downloads and builds manifest dependencies and can take several minutes. Dependencies are installed below `vcpkg_installed/` and build artifacts below `out/`; neither is committed.

Create the portable release ZIP after the Release build and tests:

```powershell
cpack --preset windows-msvc-release
```

Visual Studio 2026 users should substitute `windows-vs2026-debug` or `windows-vs2026-release` in the commands above. Separate generator presets avoid relying on machine-specific auto-detection and keep CI selection explicit.

For a telemetry/core-only target graph, configure with `BLACKBOX_BUILD_APP=OFF` and `BLACKBOX_ENABLE_STORAGE_DEPENDENCIES=OFF`. This omits UI and storage targets; vcpkg may still materialize manifest packages in the build directory. Set `BLACKBOX_ENABLE_AUTOMATIC_DETECTION=OFF` to compile out the detector implementation while leaving the recorder unchanged.

## Repository map

```text
src/app          executable lifecycle and composition root
src/core         platform-independent shared foundations
src/ui           native diagnostic UI
src/telemetry    provider contracts, normalization, OS backends, mocks
src/platform     non-telemetry OS services such as global hotkeys
src/storage      incident archive persistence
src/analysis     optional statistical and personalized incident analysis
tests            native unit and integration tests
tools            development, profiling, maintenance, and benchmark utilities
docs             telemetry and performance specifications
```

Start with the [documentation map](docs/README.md). It separates user guidance, contributor
contracts, platform/reference material, and release qualification so the repository root stays
approachable. [ARCHITECTURE.md](ARCHITECTURE.md) remains the authoritative dependency contract and
[ROADMAP.md](ROADMAP.md) tracks only current status, completed milestone outcomes, and open gates.
