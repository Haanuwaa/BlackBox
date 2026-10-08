# Build, CI and packaging

[Documentation index](README.md) · [Current candidate](CURRENT_CANDIDATE.md)

- [Build, test, and release](#build-test-and-release)
- [Quality and security gates](#quality-and-security-gates)
- [Representative incident fixtures](#representative-incident-fixtures)
- [Build throughput and documentation checks](#build-throughput-and-documentation-checks)
- [Brand assets and local cleanup](#brand-assets-and-local-cleanup)

## Brand assets and local cleanup

The source geometry for the recorder mark lives in `scripts/generate-brand-assets.py`.
Run it with Python 3 to regenerate the committed SVG logo, PNG preview, Windows ICO
sizes and status badges, macOS ICNS, Linux desktop icon and embedded SDL image.
`--check` verifies reproducibility. Normal builds require no image tools or new libraries.
Windows embeds native resources; the app and non-Windows trays use a compiled-in PNG,
so they do not depend on the working directory. macOS packages include the ICNS resource.

`_CPack_Packages/`, root `imgui.ini`, extracted package copies and Python `__pycache__`
directories are reproducible local output and can be removed when unused. Build directories
can also be regenerated, but removing them forces recompilation. Do not blanket-delete
`out/`: it also holds archives, qualification/soak evidence and potentially worktrees.
Inspect contents and references first; retain source evidence and actual user recordings.

## Build, test, and release

Linux presets `linux-gcc-release` and `linux-clang-release`, and macOS presets
`macos-arm64-release` and `macos-x64-release`, use Ninja and the pinned vcpkg graph.
Set `VCPKG_ROOT` and use the preset name with configure/build/test. Linux presets select
GCC 14 (`g++-14`) or Clang 18 (`clang++-18`), matching the compiler families pinned in the
repository's quality jobs; install the corresponding C++23 standard-library development package.
macOS presets require the Xcode 26.2 command-line tools selected with `DEVELOPER_DIR`.
A C++23 compiler and library are required;
hosted matrix results establish compiler compatibility, not the existence of a preset.
macOS CI selects Xcode 26.2 and explicitly targets 13.0 for the app and dependency overlay triplets.
Run `bash scripts/verify-macos-deployment.sh <BlackBox.app> 13.0`, then test the oldest runtime
on both architectures. A compile/link availability failure blocks that minimum-version claim.
The verifier enumerates every architecture with `lipo` and requires one macOS deployment
command per slice, including embedded libraries. A passing host slice cannot hide a newer
foreign slice; unreadable traversal, missing metadata and non-macOS targets fail verification.

Native macOS CI also extracts the TGZ and launches that extracted application using
`python3 scripts/smoke-native-package.py <executable> <new-evidence-directory>
--platform macOS --revision <revision>`. This five-second rehearsal uses fresh settings,
requires completed collection and matching identity, and kills a hung application after
30 seconds. It does not test Finder/Gatekeeper, the oldest runtime or physical permissions.
The runner's Python standard-library contract tests and the Bash deployment/Wayland fixtures
are registered with CTest when their interpreters are available (Git Bash on Windows).

Linux/macOS jobs retain configuration logs, CTest logs/JUnit results and available runtime
diagnostics even after failure. Wayland evidence includes the failing stage and application
output, with bounded compositor cleanup. Reusing a Wayland evidence directory is an error.
The hosted job limits are 45 minutes for native builds and 15 minutes for compositor runs;
individual CTest cases have a 120-second default limit. These bounds do not turn failed
qualification into success.

CI bootstraps vcpkg through `cmake -P scripts/bootstrap-vcpkg.cmake`. Set `VCPKG_ROOT`
to a new absolute directory; the helper refuses an existing checkout, reads the immutable
registry revision from `vcpkg.json`, and propagates bounded Git/bootstrap failures.
Dependency policy requires one action commit per upstream repository, so CodeQL `init`
and `analyze` cannot drift apart. Binary-cache keys include the manifest and overlay triplets.

The September CI refresh pins cache 6.1.0, download-artifact 8.0.1, and CodeQL 4.37.9
to their verified release commits. Checkout 7.0.1, upload-artifact 7.0.1,
dependency-review-action 5.0.0, and gcovr 8.6 were already current. Dependabot groups
GitHub Actions updates into one pull request. Its branches run PR validation without a
duplicate push campaign; other branches retain push validation. All four workflows cancel
superseded runs on the same ref and have explicit job limits. Historical qualification
runs and retained artifacts remain available under their existing retention policies.
Windows matrix jobs also retain CTest/JUnit and configuration diagnostics after failure.
The ZIP verifier checks the preserved `docs/docs/` guide layout and the packaged project
license, third-party overview, and original runtime dependency notices.

Packages preserve the root README/architecture/roadmap files and the nested `docs/` tree.
Project license and third-party overview accompany the docs; original resolved dependency notices
are in `licenses/<package>/copyright`. macOS bundles also include them in
`Contents/Resources/licenses`. Project rights are reserved pending an owner-selected open-source
license; dependency terms remain separate. See [current candidate](CURRENT_CANDIDATE.md).

### Intended release target

V0.27 is a pre-1.0 product build intended for x64 Windows desktop with Windows 10 22H2 or
Windows 11 and an ordinary, interactive user session. It has not completed the clean-client,
wall-clock soak, usability, accessibility, or official-signing gates required for V1.0. The native
APIs selected are non-elevated. Protected processes may remain inaccessible and are represented
explicitly rather than treated as recorder failure.

Build prerequisites are Visual Studio 2022 (Desktop development with C++), CMake 3.25+, Git, and a
bootstrapped vcpkg checkout referenced by `VCPKG_ROOT`.

The repository `.gitattributes` is authoritative for checkout normalization: source, workflow,
PowerShell, documentation, and direct-text formats use LF on every host, while packages,
executables, libraries, databases, dumps, fonts, and raster assets are explicitly binary. Do not
override this policy with a broad repository-local `core.autocrlf`; the hygiene CTest and
`git check-attr` should agree before the first commit and on hosted runners.

```powershell
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release
cpack --preset windows-msvc-release
```

The package is `out/build/windows-msvc-release/BlackBox-0.27.0-windows-x64.zip`. It includes the
executable, non-system runtime DLLs discovered from the target graph, and user/architecture docs.
Extract it to a writable directory and launch `blackbox.exe`; no installer or service is required.

Debug and Visual Studio 2026 presets are also provided. To prove the collection invariant without
UI or SQLite targets:

```powershell
cmake -S . -B out/build/windows-headless -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DBLACKBOX_BUILD_APP=OFF -DBLACKBOX_ENABLE_STORAGE_DEPENDENCIES=OFF `
  -DBLACKBOX_ENABLE_ANALYSIS=OFF -DBLACKBOX_ENABLE_AUTOMATIC_DETECTION=OFF `
  -DBLACKBOX_BUILD_TESTS=ON
cmake --build out/build/windows-headless --config Release
ctest --test-dir out/build/windows-headless -C Release --output-on-failure
```

`BLACKBOX_ENABLE_ANALYSIS=OFF` may also be used with a full app/storage build; the viewer remains
usable and reports analysis disabled. Analysis is a static optional module and adds no runtime DLL.
`BLACKBOX_ENABLE_AUTOMATIC_DETECTION=OFF` removes the automatic detector implementation and leaves
manual capture and the collector/provider/recorder graph intact.

`.github/workflows/windows.yml` runs Debug, Release, headless collection, fixture generation, and
ZIP packaging on Windows 2022, repeats Release tests on Windows 2025, and builds/tests the portable
headless graph on Linux. CI fetches the exact vcpkg commit named by the manifest baseline before
bootstrapping the tool, so the dependency registry and frontend are reproducible together.

`.github/workflows/linux-compatibility.yml` is a separate engineering matrix for Ubuntu 24.04,
Debian 13, and Fedora 43. Each container builds/tests the complete Linux desktop graph, exercises
the shell/autostart/notification/portal-shortcut boundaries, measures 64 real provider samples,
and creates a TGZ plus a native DEB or RPM. The package verifier checks desktop integration,
application icon, executable layout, private-library RPATH, native package metadata, and extraction.
In its disposable container only, the lifecycle gate then performs a real native install, launches
the installed `/usr/bin/blackbox` under Xvfb, uninstalls it, and proves the owned executable, desktop
entry, and icon were removed. The script refuses to run without both `CI=true` and the explicit
`BLACKBOX_ALLOW_SYSTEM_PACKAGE_TEST=1` guard, refuses a pre-existing installation, and installs only a
package whose native name is exactly `blackbox`. Dedicated packaged-app jobs identify SDL's Wayland
driver under headless Weston, Mutter, KWin, and Sway compositor engines, require clean diagnostic
shutdown, and enforce the safe visible-window fallback when no tray integration exists. These jobs
exercise package/compositor startup and portal-loss-safe boundaries; they do not reproduce a full
physical GNOME/KDE session, permission dialog, shell extension, assistive technology, or real mixed-
scale displays. The direct-v1 reports compare package bytes, provider P95, and maximum observed process
cardinality. Passing this workflow is cross-distribution engineering evidence; it is not physical
Linux product qualification.

`.github/workflows/macos.yml` builds and tests the complete native graph on hosted Apple Silicon and
Intel macOS runners, executes the real Mach/libproc provider contract and 64-sample benchmark, and
creates unsigned TGZ, DMG, and component PKG engineering previews. The DMG is verified with
`hdiutil`; the PKG payload is inspected and targets `/Applications`. The packaging script can sign
the staged app and installer and submit/staple both artifacts when the application identity,
installer identity, and notary keychain profile are explicitly supplied. Hosted previews omit those
secrets, so this validates telemetry, composition, and unsigned native package structure; it does not
qualify background services, signing/notarization, installation on a physical client, or product support.

`.github/workflows/quality.yml` adds isolated dependency/SBOM, dependency-review, CodeQL, MSVC
native-analysis, Windows ASan, Linux UBSan/TSan, native-fuzz, and coverage jobs. The CodeQL graph
restores a pinned vcpkg binary cache before dependency resolution while keeping third-party builds
outside tracing. Every dependency-building quality job now uses the same OS/architecture restore
prefix with a job-specific save key, so later runs can reuse compatible vcpkg binaries without
coupling jobs or weakening their independent configurations. Run the local policy,
Visual Studio 2026 analyzer, and ASan commands documented in `BUILD_AND_RELEASE.md` before a release
candidate. Authored jobs do not satisfy the separate hosted-execution gate until the release
revision actually passes and its artifacts are retained.

Developer ZIPs are unsigned. Official publishing additionally signs the BlackBox executables,
generates a SHA-256 sidecar, and verifies package contents/signatures with the scripts under
`scripts/`. Complete the package and physical host evidence in `TESTING.md`. See
`RELEASE_READINESS.md` for client compatibility, thresholds, protected-key rules,
side-by-side updates, pre-release format policy, and uninstall/data-removal behavior. The final
same-revision V0.17 composition is defined in `RELEASE_EVIDENCE.md`.

`scripts/sign-release.ps1` defaults to `-ExpectedVersion 1.0.0` and requires the exact lowercase
40-character `-ExpectedSourceRevision`. Before locating or invoking `signtool`, it verifies that
`SourceRoot` is a clean Git worktree at that exact HEAD and runs the certificate-free
`verify-release-build-binaries.ps1` preflight. The preflight requires the exact three regular
non-link shipped executables and checks semantic/fixed numeric versions, embedded source revision,
product, description, internal-name, and original-filename identity. A deliberate prerelease
signing rehearsal must pass its actual version and committed revision explicitly; it cannot be
confused with the default final-release operation.

```powershell
./scripts/sign-release.ps1 -BuildDirectory <release-build> `
  -CertificateThumbprint <40-hex-thumbprint> `
  -ExpectedSourceRevision <40-hex-release-revision>
```

All Windows executable `VERSIONINFO` resources are generated from the top-level CMake
`PROJECT_VERSION` and `BLACKBOX_SOURCE_REVISION`. Its default `auto` mode embeds the exact Git HEAD
only when the worktree is clean; an absent commit or any staged, unstaged, or untracked source embeds
`local-uncommitted`. The build test requires exact FileVersion/ProductVersion strings, matching
fixed numeric parts, source identity, product identity, descriptions, internal names, and original
filenames for every enabled product/qualification executable. Do not edit PE version strings
independently. The final version bump to `1.0.0` must therefore happen once in
`project(... VERSION ...)`, after which the runtime version, dataset metadata, executable resources,
CPack name, and evidence gates must agree.

### Release checklist

1. Run Debug, Release, and headless Release tests with no failures.
2. Generate and open the representative fixture described in `BUILD_AND_RELEASE.md`.
3. Run `blackbox_windows_provider_benchmark 0 0 <seconds>` and
   `blackbox_windows_event_provider_benchmark <milliseconds-per-source>` as an ordinary user and
   retain the per-source output.
4. Run storage/viewer benchmarks and compare with `PERFORMANCE.md` regression thresholds.
5. Exercise hotkey and tray capture, close-to-tray, pause/resume, duplicate launch, Explorer
   restart, suspend/resume, archive lock/failure recovery, clean tray Exit, and restart inspection.
6. Generate and review the exact 32-image direct-v1 raster bundle in `TESTING.md`, then
   complete its clean-client physical accessibility, DPI, multi-monitor, low-end, battery, and
   power-mode matrix on this revision.
7. Complete and retain the overnight and 72-hour campaigns in `TESTING.md` on this revision.
8. Sign official binaries, create the ZIP with CPack, generate/verify its SHA-256 sidecar, and
   complete the exact clean Windows 10 22H2/current Windows 11 and physical-profile bundle matrix in
   `TESTING.md`.
9. Retain both hosted workflow attestations and run the fail-closed aggregate in
   `RELEASE_EVIDENCE.md`; it must bind the exact packaged `blackbox.exe` to both soak runs.
10. Do not name the build V1 until the independent V0.15.1 corpus also passes its predeclared
    one-shot diagnostic-quality gate.

## Quality and security gates

The quality layer is build and test infrastructure. It does not enter the desktop executable's
runtime dependency graph, change collection, add a recorder thread, or alter any archive/settings
format. BlackBox remains pre-release and accepts only its current direct-v1 formats; these gates do
not introduce migrations, compatibility adapters, or legacy fixtures.

### Hosted jobs

`.github/workflows/quality.yml` defines independent fail-closed jobs:

- dependency-policy validation and a validated CycloneDX 1.5 SBOM artifact;
- pull-request dependency review, failing for newly introduced vulnerabilities of moderate severity
  or higher;
- CodeQL C++ analysis with the extended security query suite and an explicit manual production
  graph. The desktop executable and every product/development tool keep all production translation
  units observable, while tests, fixtures, and benchmarks stay out of the CodeQL database because
  their production dependencies are already traced and their own code is covered by warnings-as-
  errors, MSVC native analysis, sanitizers, coverage, property tests, fuzzing, and execution. Broad
  CodeQL quality queries are intentionally excluded from the Security tab because they report test-
  registration helpers and generated dependency headers. Pinned third-party packages are resolved
  before CodeQL initialization, so manual tracing observes only the subsequent six-target BlackBox
  production build. A 60-minute job timeout fails closed, and same-workflow/same-ref concurrency
  cancels superseded runs instead of allowing obsolete scans to consume runner capacity. This trace
  hygiene reduced the exact hosted CodeQL job from 35:19 to 15:46 (55.3%) without reducing the
  `security-extended` query suite or production graph;
- MSVC native `/analyze` with warnings treated as errors across product and test translation units;
- Windows AddressSanitizer across the app and test graph;
- Linux UndefinedBehaviorSanitizer for the portable headless graph;
- a narrowly scoped Linux ThreadSanitizer graph for collector, writer, viewer, and metadata-cache
  concurrency;
- Clang libFuzzer seed smoke plus four bounded 30-second campaigns covering direct-V1 settings,
  native `/proc`/sysfs/power/PSI/GPU parsers, read-only direct-V1 SQLite schema validation, and typed
  XDG portal response decoding; and
- Linux line/branch coverage with 60%/45% aggregate minimums, 45%/30% component minimums for core,
  telemetry, storage, and analysis, conservative 15%/10% app/UI module floors, a real visible
  diagnostic launch under Xvfb, and retained HTML/XML reports.

Every GitHub Action reference is an immutable 40-character commit. Dependabot checks those action
pins weekly. The vcpkg registry baseline is an immutable commit, the direct dependency and ImGui
feature sets are allowlisted, and the manifest version must match the CMake project version.

Repository controls block force pushes and branch deletion on `main`, require linear history and
resolved review conversations, and leave administrator enforcement disabled so the single-maintainer
exact-revision workflow remains usable. Vulnerability alerts, automatic Dependabot security fixes,
secret scanning, and secret push protection are enabled. A CodeQL alert may be dismissed only after
review with a per-alert reason and bounded comment; a dismissed local-CLI dataflow is not silently
suppressed from future query results.

An authored workflow is not evidence that GitHub executed it. After all required jobs succeed on a
push or manual run, each workflow's `hosted-evidence` job publishes a direct-v1 attestation binding
the GitHub SHA, repository, workflow, run ID/attempt, ref, and exact writer script. Pull-request runs
cannot publish release attestations. V0.17 requires both downloaded artifacts to pass
`verify-hosted-ci-attestation.ps1` for the release revision; see `RELEASE_EVIDENCE.md`.

The final V1 composition is a separate fail-closed gate documented in `RELEASE_EVIDENCE.md`.
It independently re-runs V0.17, binds the evaluator to the signed package, recomputes the frozen
held-out report, and requires its calibration/report fingerprints to match the consumed passing
one-shot attempt. `v1_release_evidence_script_contracts` parses both scripts and exercises the
local-uncommitted and partial-output rejection paths in every Windows graph. The contract test does
not claim the external evidence exists.

### Local Windows commands

Run the mechanically checked policy contracts first:

```powershell
./scripts/verify-dependency-policy.ps1
./scripts/verify-quality-gates.ps1
```

Generate a validated SBOM after a manifest configure has produced a vcpkg status file:

```powershell
./scripts/generate-sbom.ps1 `
  -StatusPath out/build/windows-vs2026-release/vcpkg_installed/vcpkg/status `
  -OutputPath out/quality/blackbox.cdx.json
```

Visual Studio 2026 has local presets for the Windows gates:

```powershell
cmake --preset windows-vs2026-static-analysis
cmake --build --preset windows-vs2026-static-analysis

cmake --preset windows-vs2026-address-sanitizer
cmake --build --preset windows-vs2026-address-sanitizer
ctest --preset windows-vs2026-address-sanitizer
```

The ASan preset uses `cmake/triplets/x64-windows-blackbox-asan.cmake`, so Catch2, SDL, ImGui,
ImPlot, and SQLite are compiled with the same instrumentation and MSVC STL annotations as BlackBox.
The compiler ASan runtime is copied beside the app and test binary. The deliberate child-process
unhandled-exception probe is excluded from ASan because its required access violation is the test
input; it remains required in every ordinary Debug/Release graph.

Instrumentation modes are isolated: MSVC analysis and coverage cannot be combined with sanitizer or
fuzzer modes, and ASan, UBSan, and TSan cannot be combined with each other. CMake rejects invalid
combinations rather than silently producing ambiguous evidence.

### Direct-v1 pre-release contract

Every build graph registers `pre_release_direct_v1_contract`. The cross-platform CMake check pins
the SQLite archive schema and the eleven persisted settings, dataset, support, soak, dogfood,
evaluation, truth-review, and campaign-status formats to version 1. It also requires exactly one schema publication
at `PRAGMA user_version=1` and fails if production sources introduce migration, legacy-reader,
compatibility-reader, or versioned alternate-schema tokens. Ordinary parser tests prove that
otherwise valid product and recorder settings changed to format 2 are rejected without conversion.
The source contract complements behavioral parser/archive tests; it does not claim that text
inspection replaces corruption, property, fuzz, or restore coverage.

Every graph also registers `release_claims_documentation_contract`. It binds the public telemetry
matrix's implemented V0.14 GPU and responsiveness claims to the Windows provider and rejects the
superseded pre-V0.14 GPU-research wording. It also requires the consented corpus, one-shot held-out,
physical-client, unsigned-package, and clean-client disclosures to remain explicitly open. This
contract pins application crash in the nine-class acquisition matrix, requires exact `1.0.0`
package names in final client/V0.17/V1 evidence instructions, and rejects a prerelease package name
from either final composition guide. This does not manufacture external evidence; it prevents local
documentation edits from claiming missing evidence exists or directing the final campaign at the
wrong artifact.

Windows resource and release-binary contracts also bind build provenance. Each enabled executable
must carry the configure-time source revision in its standard signed version resource. The signing
preflight requires a clean exact Git HEAD before binary inspection or certificate access, and
package/final-evidence verification rejects a different embedded revision. Contract fixtures prove
clean acceptance plus wrong-HEAD, tracked-dirty, untracked, binary mismatch, package mismatch, and
renamed-executable rejection. `local-uncommitted` remains valid only for unsigned development work.

Qualification producers fail early on the same boundary. Client qualification verifies the copied
package against its declared revision before extraction or execution. UI qualification compiles the
effective revision into the raster test executable and rejects a different requested revision before
writing any BMP. The UI contract launches the real generator with a wrong revision and proves that
the staging directory remains empty.

Every graph also registers `prediction_free_dogfood_cli_contract`. It proves the CLI main dispatch
contains no eager analyzer construction, permits exactly three explicit pipeline-identity calls,
and limits analyzer construction sites to identity/initialization, prediction-bearing development
inspection, and evaluation. This is a source-boundary guard for blinded annotation and campaign
coordination; the end-to-end acquisition contract separately executes those paths over a real
schema-v1 fixture archive. That CLI contract also completes a truth-review ballot, validates its
incident/operator binding without prediction-bearing output, and proves wrong-incident and
collection-operator ballots are rejected. It then compares agreeing and differing independent
ballots, proves only the disagreement bit changes, and rejects a repeated annotator identity.
Native corpus tests also corrupt session counts, ballot counts, disagreement flags, annotator
identity, and duplicate ballots and require each failure to identify the exact privacy-safe row and
numeric mismatch.

Diagnostic artifact tests serialize the exact V1 prediction/report pair, independently parse the
prediction rows, recompute every metric from frozen truth, and require canonical byte equality. They
exercise metric tampering, sparse-row removal, calibration-fingerprint declaration changes, excess
rows, oversized rows, duplicate contributor ordinals, and corpus-provenance mismatch. Publication
also runs this verifier both before and after its atomic rename; the CLI contract proves a collecting
corpus cannot enter standalone `verify-evaluation`.

Confidence-calibration artifact tests exercise the single offline V1 codec independently of the
CLI. They require exact-precision canonical round trips and verified new-file publication, then
reject alternate numeric spellings, CRLF, reordered fields, inconsistent sample totals, decreasing
knots, missing final LF, oversized lines/files, directories, occupied destinations/staging paths,
and inconsistent assertion state. The direct-v1 source contract additionally requires the non-link
check, canonical rejection path, and sole serializer, preventing a second permissive app reader.

### Property and fuzz scope

The ordinary suite deterministically truncates and mutates the strict product/recorder settings
inputs. Every accepted mutation must still pass the direct-v1 validator. It also presents 128
bounded deterministic random non-database files to the archive and proves rejection does not modify
their bytes.

One native fuzzer calls the same in-memory strict-v1 settings parsers with a 20 KiB input ceiling.
A second independently bounded entry point exercises Linux `/proc`, sysfs frequency/profile, power,
PSI, AMD GPU, and DRM `fdinfo` parsers plus the strict finite-decimal evaluation parser. A third
deserializes at most 1 MiB into an in-memory, read-only SQLite database and invokes the production
direct-V1 schema validator. A fourth builds typed, bounded D-Bus responses and exercises the
production portal response decoders without connecting to a user session. Checked-in seeds are
copied into a build-local corpus. CI first runs deterministic smoke iterations, then runs four
30-second ASan-backed campaigns with explicit timeout, input, and memory limits. A short bounded
campaign is a regression gate, not a claim of exhaustive fuzzing.

### Coverage meaning

Coverage measures the portable `src/` graph on Linux and excludes Windows-only platform and
telemetry sources that are not compiled there. The established 60% line and 45% branch aggregate
floors cover the non-app/UI portable graph. Conservative 45%/30% floors independently prevent core,
telemetry, storage, or analysis from disappearing behind another well-covered component. App and UI
are deliberately disjoint from that aggregate and each has its own 15% line/10% branch floor after
the complete suite and a visible diagnostic launch. This prevents the lower new interactive-module
floors from weakening or double-counting the established aggregate. These are minimum gates, not a
statement that unmeasured Windows or interactive UI paths are covered. Windows behavior is covered
by its ordinary, integration, static-analysis, and ASan jobs.

### Failure policy

Sanitizer findings, analyzer warnings, policy mismatches, malformed/unapproved dependency metadata,
SBOM validation errors, vulnerability-floor violations, CodeQL failures, fuzz crashes/timeouts, and
coverage-floor regressions fail their jobs. Exceptions require a source-reviewed narrow change to
the relevant gate and documentation; broad warning or sanitizer suppression is not the default.

## Representative incident fixtures

`blackbox_fixture_generator` creates a deterministic schema-current SQLite archive for tests,
viewer development, screenshots, and current-schema checks. It writes three annotated incidents:

| Fixture | Shape | Purpose |
|---|---:|---|
| Unavailable metrics | 2 system rows, 1 process row | Explicit unsupported, inaccessible, and temporary states |
| Typical workstation | 150 frames x 50 processes | Default 120-second pre/30-second post window at ordinary scale |
| High process scale | 150 frames x 500 processes | Pagination, plotting, storage, and memory stress |

From a configured build, run:

```powershell
.\out\build\windows-msvc-release\tests\Release\blackbox_fixture_generator.exe `
  .\out\fixtures\representative-incidents.sqlite3
```

The destination is replaced intentionally. Generated databases are build artifacts rather than
source artifacts, preventing committed SQLite files from silently drifting behind the schema.
The generator verifies the incident count and database size before succeeding. CTest invokes it
on every storage-enabled validation run.

## Build throughput and documentation checks

Visual Studio generators default to two compiler processes per project via
`BLACKBOX_MSVC_COMPILE_JOBS` (valid range 1–32). Use `cmake --build ... --parallel 2`
to bound project concurrency too; their product is the approximate upper bound on
compiling processes. Ninja already schedules individual files. MSVC native analysis
uses its separate graph without this option. This follows the
[MSVC multiprocess build model](https://learn.microsoft.com/en-us/cpp/build/reference/mp-build-with-multiple-processes).

Native Windows/macOS CI jobs cache vcpkg binary packages by OS, architecture, runner,
job and manifest/triplet/bootstrap inputs. vcpkg still checks package ABIs; caches are
an optimization, not release evidence. Boundary jobs retain CTest logs and JUnit results
on failure. Mac bundle/package versions come from CMake's configured project version.

Run `python scripts/check-docs.py` after documentation edits. It validates repository
Markdown targets and heading anchors without accessing external sites. The release-claims
CTest guard separately checks that implemented capabilities and unfulfilled human gates
remain stated honestly. Current revision-specific outcomes belong in
[Current candidate](CURRENT_CANDIDATE.md), not duplicated test-count summaries.
