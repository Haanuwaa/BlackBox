# BlackBox

BlackBox is a lightweight, native computer flight recorder for answering a simple question: **what caused the short-lived performance problem that just happened?** It retains a bounded history of system behavior in memory, then saves a focused pre/post-incident window only when an incident is captured.

The long-term product will help diagnose freezes, game and audio stutter, disk spikes, hangs, network latency, and unexpected CPU/GPU changes. The recorder comes first; statistical and machine-learning analysis will consume recorded data later and will never be required for collection.

## Current status

Version **0.15.0** is a pre-1.0 product milestone of the local-native Windows flight recorder.
It combines bounded configurable recording, manual/automatic capture, an evidence-first six-page
workflow, synchronized incident timelines, guided archive recovery, privacy-safe local support
bundles, bounded Windows crash evidence, the versioned local analysis
pipeline, archive browsing and labeling, explicit retention/privacy maintenance, persisted validated
recorder profiles, bounded retry for transient incident-persistence failures, and a native Windows
tray/background shell with close-to-tray, pause/resume, launch-at-login, lifecycle notifications,
Explorer recovery, single-instance activation, capability-gated GPU/responsiveness/power gauges,
independent privacy-bounded Windows event evidence, and a frozen one-host dogfood evaluation with
calibration/held-out tooling. It is not yet the production product
described by the vision: representative multi-host/natural diagnostic quality, held-out symptom
classification/ML comparison, clean-client qualification, distribution lifecycle, and official
signing remain roadmap gates. A development-only Linux system/process provider and label-free offline
model-comparison harness now exercise the platform and evaluation boundaries, but neither is a
support claim or a shipped ML runtime. Native ML remains unshipped because no representative
held-out dataset demonstrates material benefit; all intelligence stays optional and unable to
affect recording.

Windows is the only currently supported product platform. Linux now has an engineering desktop build,
CPU/memory/disk/network/process telemetry, a bounded provider-overhead gate, and an explicitly
unsupported TGZ engineering preview. A separate Ubuntu 24.04, Debian 13, and Fedora 43 hosted matrix
builds, measures, extracts, and launches that package. The Linux platform boundary also owns a native
SDL tray, per-user instance lock, and exact XDG autostart entry, while unavailable tray/notification
protocols remain explicit. Linux still lacks physical desktop/accessibility/session/power/installer
qualification and the evidence required for support.
macOS now has an engineering-only native CPU/memory/process provider and `.app` bundle. Its platform
adapter adds a bounded single-instance lock, SDL menu-bar tray, current ServiceManagement login item,
permission-aware local notifications, and AppKit contrast/reduced-motion preferences. Global shortcuts,
broader system telemetry, crash handling, signing/notarization, and physical-client qualification remain
open, so this is not a macOS support claim.

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
The bounded component record format and replaceable-sink rules are documented in
[docs/LOGGING.md](docs/LOGGING.md).
The same-revision hosted/UI/client/soak/signing composition contract is documented in
[docs/V017_RELEASE_EVIDENCE.md](docs/V017_RELEASE_EVIDENCE.md); it cannot replace the separate
diagnostic-quality gate.

The dated [project audit](docs/PROJECT_AUDIT_2026-08-22.md) summarizes current product maturity,
resource-blocked release gates, UI/maintenance priorities, and safe parallel paths for Linux and
offline ML research.
