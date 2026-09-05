# Build, test, and release

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

Packages preserve the root README/architecture/roadmap files and the nested `docs/` tree.
Project license and third-party overview accompany the docs; original resolved dependency notices
are in `licenses/<package>/copyright`. macOS bundles also include them in
`Contents/Resources/licenses`. Project rights are reserved pending an owner-selected open-source
license; dependency terms remain separate. See [current candidate](CURRENT_CANDIDATE.md).

## Intended release target

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
Visual Studio 2026 analyzer, and ASan commands documented in `QUALITY_GATES.md` before a release
candidate. Authored jobs do not satisfy the separate hosted-execution gate until the release
revision actually passes and its artifacts are retained.

Developer ZIPs are unsigned. Official publishing additionally signs the BlackBox executables,
generates a SHA-256 sidecar, and verifies package contents/signatures with the scripts under
`scripts/`. Complete the package and physical host evidence in `CLIENT_QUALIFICATION.md`. See
`RELEASE_READINESS.md` for client compatibility, thresholds, protected-key rules,
side-by-side updates, pre-release format policy, and uninstall/data-removal behavior. The final
same-revision V0.17 composition is defined in `V017_RELEASE_EVIDENCE.md`.

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

## Release checklist

1. Run Debug, Release, and headless Release tests with no failures.
2. Generate and open the representative fixture described in `FIXTURES.md`.
3. Run `blackbox_windows_provider_benchmark 0 0 <seconds>` and
   `blackbox_windows_event_provider_benchmark <milliseconds-per-source>` as an ordinary user and
   retain the per-source output.
4. Run storage/viewer benchmarks and compare with `PERFORMANCE.md` regression thresholds.
5. Exercise hotkey and tray capture, close-to-tray, pause/resume, duplicate launch, Explorer
   restart, suspend/resume, archive lock/failure recovery, clean tray Exit, and restart inspection.
6. Generate and review the exact 32-image direct-v1 raster bundle in `UI_QUALIFICATION.md`, then
   complete its clean-client physical accessibility, DPI, multi-monitor, low-end, battery, and
   power-mode matrix on this revision.
7. Complete and retain the overnight and 72-hour campaigns in `WALL_CLOCK_SOAKS.md` on this revision.
8. Sign official binaries, create the ZIP with CPack, generate/verify its SHA-256 sidecar, and
   complete the exact clean Windows 10 22H2/current Windows 11 and physical-profile bundle matrix in
   `CLIENT_QUALIFICATION.md`.
9. Retain both hosted workflow attestations and run the fail-closed aggregate in
   `V017_RELEASE_EVIDENCE.md`; it must bind the exact packaged `blackbox.exe` to both soak runs.
10. Do not name the build V1 until the independent V0.15.1 corpus also passes its predeclared
    one-shot diagnostic-quality gate.
