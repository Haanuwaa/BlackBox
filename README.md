# BlackBox

BlackBox is a lightweight, native computer flight recorder for answering a simple question: **what caused the short-lived performance problem that just happened?** It retains a bounded history of system behavior in memory, then saves a focused pre/post-incident window only when an incident is captured.

The long-term product will help diagnose freezes, game and audio stutter, disk spikes, hangs, network latency, and unexpected CPU/GPU changes. The recorder comes first; statistical and machine-learning analysis will consume recorded data later and will never be required for collection.

## Current status

Version **0.23.0** is the pre-1.0 audit-closure and quality-hardening engineering line. The product
has one bounded native recorder, direct-V1 archive, optional statistical-analysis
pipeline, and evidence-first desktop workflow across its Windows product target and Linux/macOS
engineering previews. Accepted incident-viewer mutations are now isolated from coalescible reads and
drained on shutdown, with persistence failure counted separately while later FIFO work continues;
portable settings and event contracts use source-neutral terminology; and the dashboard avoids
rebuilding large projections when the collector has not advanced. The V0.23 quality graph adds four
native fuzz targets, randomized lifecycle models, full-desktop app/UI coverage floors, shared pinned
dependency caches, and an exact-process visible/minimized/hidden/background measurement harness.

Windows remains the intended first product target, but no platform has a public support claim before
the remaining physical, signing, long-run, and diagnostic-quality gates pass. Linux implements native
CPU/memory/process/disk/network/power/uptime evidence, capability-driven GPU backends, exact PSI
CPU/memory/I/O stall fractions, X11 foreground identity, Wayland portal integration, and TGZ/DEB/RPM
engineering packages. Wayland foreground identity remains explicitly unavailable because the public
portal surface has no standardized permission-bounded active-window identity API.

macOS implements native CPU/memory/process/disk/network/power/uptime evidence, public Metal inventory,
BlackBox renderer health, coarse public thermal-pressure state, desktop integration, global-shortcut
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

See [ARCHITECTURE.md](ARCHITECTURE.md) for dependency rules, [ROADMAP.md](ROADMAP.md) for tracked
milestones, [docs/USER_GUIDE.md](docs/USER_GUIDE.md) to operate the recorder,
[docs/CONFIGURATION.md](docs/CONFIGURATION.md) for validated settings, and
[docs/ANALYSIS.md](docs/ANALYSIS.md) for scoring semantics. The frozen diagnostic evaluation,
required multi-hardware follow-up, and limits are documented in
[docs/DOGFOOD_PROTOCOL.md](docs/DOGFOOD_PROTOCOL.md),
[docs/V0151_COLLECTION_CAMPAIGN.md](docs/V0151_COLLECTION_CAMPAIGN.md), and
[docs/V015_DOGFOOD_RESULTS.md](docs/V015_DOGFOOD_RESULTS.md). See
[docs/WINDOWS_FORENSICS.md](docs/WINDOWS_FORENSICS.md),
[docs/PERSONALIZATION.md](docs/PERSONALIZATION.md),
[docs/CONTRIBUTOR_RANKING.md](docs/CONTRIBUTOR_RANKING.md), and
[docs/RECURRING_INCIDENTS.md](docs/RECURRING_INCIDENTS.md) for analysis evidence boundaries. Build,
fixture, capture, persistence, inspection, signal, and overhead contracts are in
[docs/BUILD_AND_RELEASE.md](docs/BUILD_AND_RELEASE.md), [docs/FIXTURES.md](docs/FIXTURES.md),
[docs/INCIDENTS.md](docs/INCIDENTS.md), [docs/STORAGE.md](docs/STORAGE.md),
[docs/VIEWER.md](docs/VIEWER.md), [docs/TELEMETRY.md](docs/TELEMETRY.md), and
[docs/PERFORMANCE.md](docs/PERFORMANCE.md).

The sanitizer, static-analysis, dependency/SBOM, fuzz/property, and coverage contracts are in
[docs/QUALITY_GATES.md](docs/QUALITY_GATES.md).

V0.14 source semantics, bounds, privacy controls, and researched non-adoptions are documented in
[docs/WINDOWS_EVENT_EVIDENCE.md](docs/WINDOWS_EVENT_EVIDENCE.md).
Crash/support behavior, the privacy boundary, and evidence-preserving recovery procedures are
documented in [docs/SUPPORTABILITY.md](docs/SUPPORTABILITY.md),
[docs/PRIVACY_THREAT_MODEL.md](docs/PRIVACY_THREAT_MODEL.md), and
[docs/RECOVERY_RUNBOOKS.md](docs/RECOVERY_RUNBOOKS.md). Real elapsed-time release campaigns and
their evidence contract are in [docs/WALL_CLOCK_SOAKS.md](docs/WALL_CLOCK_SOAKS.md). Deterministic
native UI raster evidence and the separate physical Windows accessibility/DPI/multi-monitor matrix
are defined in [docs/UI_QUALIFICATION.md](docs/UI_QUALIFICATION.md). Portable-package smoke,
single-host profiles, independent bundle verification, and aggregate clean-client coverage are in
[docs/CLIENT_QUALIFICATION.md](docs/CLIENT_QUALIFICATION.md).
The development Linux system/process boundary is described in
[docs/TELEMETRY.md](docs/TELEMETRY.md), and the label-free feature export plus verified baseline
comparison workflow is documented in [docs/OFFLINE_ML.md](docs/OFFLINE_ML.md).
The current outcome-by-outcome Windows/Linux/macOS gap map and ordered native work are maintained in
[docs/PLATFORM_PARITY.md](docs/PLATFORM_PARITY.md).
The bounded component record format and replaceable-sink rules are documented in
[docs/LOGGING.md](docs/LOGGING.md).
The same-revision hosted/UI/client/soak/signing composition contract is documented in
[docs/V017_RELEASE_EVIDENCE.md](docs/V017_RELEASE_EVIDENCE.md); it cannot replace the separate
diagnostic-quality gate.

The dated [project audit](docs/PROJECT_AUDIT_2026-08-22.md) summarizes current product maturity,
resource-blocked release gates, UI/maintenance priorities, and safe parallel paths for Linux and
offline ML research.
