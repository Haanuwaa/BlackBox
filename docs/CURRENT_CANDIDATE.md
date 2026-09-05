# Current hardening candidate

The September 2026 hardening work remains version 0.27.0. Local builds identify
the edited tree as `local-uncommitted`; they are rehearsals, not frozen release
evidence. A release candidate needs a clean revision, a fresh configure/build,
and required evidence repeated against that exact revision.

## Implemented scope

- Typed native resume accounting; unexplained long gaps fail qualification.
- One identified, timestamped injected write failure, no unretained captures,
  and export plus explicit retry of the original failed incident.
- Binary preflight, isolated runtime/DLL inventory, live collection progress,
  bounded settings/drain watchdogs, and retained failure reasons.
- Separate default-profile rehearsal with full capture windows, automatic/manual
  overlap, visible/minimized/hidden transitions, and a bounded CPU workload.
- Coordinated purge/restore, emptied recovery and pending-write state after
  purge, and invalidated viewer caches. Restore refuses unresolved recovery.
- Windows basic process metadata independent of path privacy, UTF-8 UI paths,
  and integrity/schema verification of backups and pre-restore safety copies.
- Production-font raster coverage, file pickers, unsaved settings/incident edits,
  recovery/capacity notices, and readable incident summary export.
- Native developer presets, explicit macOS deployment settings and Mach-O checks,
  dependency notices, preserved documentation layout, and extracted font,
  dialog, summary and backup operations.

## Local validation

Validation on Windows 10.0.26200, Visual Studio 2026/MSVC 19.51, 12 logical
processors, on 5 September 2026:

- Clean Release rebuild followed by **393/393 passing tests** on the completed
  changes (`out/hardening/ctest-complete.log`, 58.37 seconds).
- Independent headless configure/build with UI, storage, analysis and automatic
  detection disabled: **185/185 tests passed**
  (`out/hardening/headless-tests.log`, 29.81 seconds).
- **32 passing production-font raster fixtures**, with settled layouts and
  100/150/200% font/style scaling (`out/hardening/ui-settled/`). Live, Settings
  and onboarding received visual inspection. Native monitor transitions and
  assistive technology remain separate physical checks.
- **60-second fault rehearsal passed** in a Unicode directory
  (`out/hardening/fault-verified-é-测试/`): five archived captures, exactly one
  exhausted write for sequence 2, one explicit retry, no unretained loss. The
  exported recovery database and active archive both pass SQLite integrity
  checks; sequence 2 has matching marker/window/sample counts in both.
- **240-second default-profile rehearsal passed**
  (`out/hardening/default-profile/`): 241 collections, two completed captures,
  one real automatic trigger with manual overlap, and visible/minimized/hidden
  transitions. No failed/dropped collections, rejected captures or failed writes.
- The watchdog integration rejects a wrong revision before staging/launch and
  kills an intentionally suspended, test-owned application after the external
  deadline, retaining failed partial evidence. Its repeatable opt-in test is
  `tests/scripts/wall_clock_watchdog_integration.ps1`.
- CPack ZIP extraction matched all five tested executable/DLL hashes, included
  runtime dependency notices, excluded development fault/workload helpers and
  had no broken local documentation links. A 12-second smoke from the extracted
  runtime passed (`out/hardening/package-smoke/`). The unsigned development ZIP
  is under `out/hardening/package/`; it is not a clean-client qualification.

The default-profile run reached 82.2 MiB working set and 132.4 MiB private bytes.
Its two full-sized snapshots took at most 12.05 ms and its two archive writes at
most 115.34 ms. Two observations are not a representative tail distribution.
Separate serial 30-second UI measurements, with five seconds of warmup and
product defaults enabled, recorded:

| Mode | Average total-machine CPU | Average working set | Average private bytes |
| --- | --- | --- | --- |
| Visible | 0.418% | 74.5 MiB | 129.4 MiB |
| Minimized | 0.140% | 72.7 MiB | 126.7 MiB |
| Hidden | 0.021% | 72.0 MiB | 126.5 MiB |
| Background | 0.016% | 59.8 MiB | 116.0 MiB |

These results exceed the aspirational 30–50 MB initial working-set target.
Keep the target visible and profile allocations on representative hardware;
do not raise qualification limits to turn these measurements into a pass.
The isolated short-window soak's limits do not establish the cost of defaults.

Development evidence lives under `out/hardening/` and does not satisfy the
public release-evidence ledger. Earlier failed fault rehearsals remain partial:
they exposed startup-relative injection timing and ANSI environment-path
conversion, both corrected. Build outputs and rehearsals changed during this
hardening pass; the next release campaign must freeze and bind one final revision.

The original September review report, probes, synthetic fixtures and review logs
were removed after implementation and local validation. The old interrupted
72-hour campaign and new failed rehearsal evidence remain identifiable as incomplete.
The old 0.15.0 portable ZIP and checksum now live under `out/artifacts/0.15.0/`.

## Cross-platform follow-up

The current native Linux graph was built in an isolated Ubuntu 24.04 x86_64 Docker
container on this Windows host with GCC 13 and the pinned vcpkg graph. The strict
build initially failed on GCC's `maybe-uninitialized` diagnostic for exchanging an
optional file-dialog result. An explicit check/move/reset now compiles on both GCC
and MSVC; warning enforcement remains enabled.

- **365/365 Linux tests passed** in 11.79 seconds, including the three new platform
  script suites. Logs and JUnit results are under `out/cross-platform/ubuntu/`.
- TGZ extraction and a five-second X11 runtime smoke passed with six collections
  and zero failed samples. DEB metadata/desktop validation, installation, launch
  and removal also passed. An explicit LF rule for `.desktop` files prevents the
  CRLF packaging failure found when using this Windows working copy.
- The current native build passed a five-second headless Mutter/Wayland smoke as
  ordinary user `ubuntu` (UID 1000): six collections, zero failures, and the window
  remained visible when no tray was available. This is container runtime evidence,
  not a physical GNOME session or permission-dialog qualification.
- The native provider completed 64 samples without failure (P95 878 microseconds),
  but saw only four container processes. This is a provider smoke, not representative
  desktop performance qualification.
- Windows rebuilt successfully after the GCC adjustment. The new script suites pass
  there too: 22 macOS deployment fixtures, four Wayland failure/success scenarios plus
  stale-evidence rejection, and five Python package-runner tests (including malformed
  reports and an actual timed-out child). These do not execute native macOS APIs.

The latest hosted matrix is described in [platform parity](PLATFORM_PARITY.md): Mac
jobs passed on an older product tree, while the Linux Mutter job failed. Its exact
Ubuntu artifact was hash-verified and passed a local headless Mutter reproduction;
the hosted failure remains unresolved. Native jobs now retain failure diagnostics,
and Mac packaging adds a bounded launch from the extracted TGZ plus explicit checks
of every Mach-O architecture's deployment target. Those workflow changes still need
a hosted run against the current candidate.

The new Linux packages are unsigned engineering previews in `out/cross-platform/ubuntu/`.
They and the test binaries still identify as `local-uncommitted`; documentation and
packaging line endings were finalized after the native compile. Earlier Windows
rehearsals and the existing Windows ZIP precede this follow-up. None is frozen release
evidence. No physical Mac is available, and the 72-hour campaign has not been started.

## Required external qualification

| Gate | Evidence still required |
| --- | --- |
| Frozen Windows campaign | Same-revision overnight and 72-hour passes; physical sleep/resume, lock/unlock and safe device churn. |
| Physical Windows clients | Clean Windows 10/11 package launch, removal, login behavior, permissions, real DPI changes, file pickers and ordinary-account recovery. |
| Assistive technology | Narrator, VoiceOver and Orca focus/name/value/announcement testing. ImGui currently has no complete semantic screen-reader bridge; keyboard/high contrast do not establish support. |
| macOS | Hosted arm64/x64 build and Mach-O checks; oldest-OS launch, permission denial/revocation, sleep and package tests. Signing/notarization require owner identities. |
| Linux | Hosted distro builds; physical GNOME/KDE, Wayland/X11, denied/revoked portals, reconnect, absent tray, notifications, installation and removal. |
| Performance and usefulness | Repeated full-profile runs on lower-end/battery hardware, storage contention, full archives, and independently labelled representative incident evaluation. |

Keep interrupted 72-hour attempts identifiable as incomplete. Do not promote
synthetic fixtures or `local-uncommitted` rehearsals to release evidence.
Runtime ML, cloud services, telemetry expansion, a UI-framework rewrite,
lock-free concurrency and schema changes remain deferred.
