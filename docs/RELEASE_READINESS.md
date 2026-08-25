# V1.0 qualification plan

The current application version is 0.18.0. V1.0 is reserved for the completed product and is not
yet achieved. This document defines future release gates; it does not assert that an unsigned local
package, an authored CI workflow, accelerated-time fixtures, or server-runner builds satisfy them.
Roadmap headings `V0.16` and `V0.17` are engineering stages. The runtime/package version now follows
the completed cross-platform evidence engineering stage as `0.18.0`; it changes to `1.0.0` only for the
exact final candidate revision that must repeat and compose the complete evidence.

The first public release version is exactly `1.0.0`. No prerelease or release-candidate build may
carry that version. The final qualification chain accepts only
`BlackBox-1.0.0-windows-x64.zip`, requires both same-revision wall-clock application reports to
declare `application_version=1.0.0`, and records `product_version=1.0.0` in the final evidence
ledger. The CMake project version remains the runtime/package source of truth; the final version
change must update its exact-version test and every maintained command example together. Direct-V1
archive, settings, export, evaluation, and evidence format numbers are independent data-format
identities and remain `1`; they are not semantic product versions or migration stages.

The same CMake version generates Windows `VERSIONINFO` for `blackbox.exe`, the two shipped offline
tools, and the two local qualification helpers. Final binaries must expose exact `1.0.0` string
FileVersion/ProductVersion fields with fixed numeric `1.0.0.0` parts and correct executable identity;
each also carries `source_revision=<40-hex-release-revision>` in its signed Windows version resource.
The Windows build test rejects blank, stale, or independently edited PE metadata. The final V1
verifier independently extracts the signed package and applies the same `1.0.0` and source-revision
identity contract to all three shipped executables.

The official signing script also defaults to expected version `1.0.0`, requires the exact release
revision, proves `SourceRoot` is clean at that Git HEAD, and executes the same strict three-binary
identity preflight before it accesses `signtool` or a certificate. Thus a dirty source tree, wrong
revision, stale prerelease build, renamed tool, missing file, link, or blank/inconsistent resource
fails before any signature is produced.

## Supported systems

The intended V1.0 production target is x64 Windows 10 22H2 and supported x64 Windows 11 releases in an
ordinary interactive desktop session. Administrator rights are not required. Protected processes
can remain inaccessible; BlackBox exposes that state and does not elevate. ARM64, Windows Server,
Windows on ARM emulation, and machines with more than 64 logical processors are not qualified V1.0
targets. On the latter, system CPU currently covers only the calling processor group.

Visual Studio 2022 and 2026 builds use the Windows 10 SDK. The general platform target remains
`_WIN32_WINNT=0x0601`, while the Windows telemetry target declares `0x0A00` for its Windows 10
notification APIs; all selected runtime APIs are available on the supported client floor. CI compiles/tests on the Windows 2022 and
Windows 2025 runner kernels. The release checklist still requires an extracted-package smoke test
on clean Windows 10 22H2 and current Windows 11 x64 hosts before publishing an official build; CI
server kernels supplement rather than replace those client tests.

Linux and macOS are not supported products in V1.0. Linux readiness is an engineering claim: the
headless core/telemetry graph has no Win32 dependency; the Linux provider implements bounded
system/process telemetry; and Ubuntu, Debian, and Fedora hosted containers build, measure, package,
and smoke the native desktop. Its platform boundary now has a tray, per-user lock, and XDG
autostart, but GPU, event, power, physical-desktop, session, and installer qualification remain open.
Its platform adapter now reads standardized contrast and reduced-motion settings asynchronously
through XDG Settings; physical desktop behavior is unqualified. Linux and macOS also publish a fixed,
bounded local POSIX signal record behind the portable crash-diagnostics boundary. macOS has an
engineering-only native system/process provider and `.app` shell with a single-instance lock, tray,
ServiceManagement login item, permission-gated local notifications, and AppKit accessibility
preferences. Native network throughput, power source, battery, and uptime are implemented;
disk/quality/GPU/events, global shortcut, physical-client, signing, notarization, and distribution
qualification remain open.
No platform support is claimed until a real backend lives in its OS directory, passes the same
contracts, represents unsupported data explicitly, and meets equivalent quality gates.

## Release gates

An official V1.0 candidate must satisfy all of the following on the same source revision:

- Debug and Release full graphs, the full-app analysis-disabled graph, and the fully headless graph
  have zero failing tests; the direct pre-release schema-v1 baseline round-trips every field and
  rejects non-v1, unversioned non-empty, or canonically incompatible version-1 archives without
  modifying them. Restore applies the same exact-layout check through a read-only source.
- The accelerated seven-day recorder/detector soak remains bounded, produces no smooth-fixture
  automatic captures, and retention/reopen/recovery tests pass.
- The same assembled Release revision passes the real overnight and 72-hour campaigns in
  `WALL_CLOCK_SOAKS.md`; the latter includes independently observed sleep/resume, operator-attested
  lock/device churn, and isolated archive write-fault/recovery evidence.
- The Release UI test produces and hashes the exact 30-case direct-v1 raster bundle defined in
  `UI_QUALIFICATION.md`; every image receives manual visual review, and the separate clean-client
  keyboard/accessibility, 100/125/150/200%-DPI, mixed-scale multi-monitor, low-end, battery, and
  power-mode matrix passes on the packaged candidate.
- Real child-process crash probes produce a bounded valid Windows minidump or Linux/macOS POSIX signal
  record; clean shutdown removes unused staging, and privacy-safe/consented-evidence support bundles
  pass exact-content and atomic-publication tests. Recovery runbooks and the privacy threat model match
  the shipped direct-v1 behavior.
- Dependency policy/SBOM validation, dependency review, CodeQL, MSVC native analysis, Windows ASan,
  Linux UBSan, native fuzzing, and coverage floors pass on the same release revision. The deliberate
  crash probe remains required in ordinary graphs and is excluded only from ASan execution.
- Default one-second recording has zero normal-load drops/deadline misses, effectively zero
  telemetry disk writes, collection P99 below 250 ms, and hidden idle CPU below 1% on each clean
  client validation host.
- Working set is at most 80 MiB after ring warm-up. This supersedes the early aspirational 50 MiB
  target while preserving the 5 MiB regression-investigation threshold.
- Incident snapshot P99 is below 50 ms at 500 processes; a 500-process SQLite transaction is below
  500 ms; a 500-process intelligent analysis is below 100 ms; no controlled quiet diagnosis or
  smooth automatic-trigger false positive is allowed.
- The package checksum verifies, its documented contents are present, and official binaries have
  valid trusted Authenticode signatures and RFC 3161 timestamps.

Hardware-sensitive results must identify OS build, CPU, process count, duration, power mode, build,
and settings. Shared CI timing is informational; correctness and bounds remain hard gates.

## Signing, packaging, update, and uninstall

Local developer packages are intentionally unsigned and must not be represented as official.
Official builds use an organization-controlled code-signing certificate: run
`scripts/sign-release.ps1 -ExpectedSourceRevision <40-hex-release-revision>` on all three shipped
BlackBox executables before CPack, create the ZIP, run
`scripts/write-release-checksum.ps1`, then run `scripts/verify-release.ps1 -RequireAuthenticode`.
Certificate private keys must live in a protected signing service or hardware-backed store and
must never enter the repository or package.

The exact ZIP must then complete the direct-v1 host profiles and aggregate verification in
`CLIENT_QUALIFICATION.md`. A developer-machine smoke or one interactive host cannot satisfy the
clean-client/physical matrix, and an unsigned matrix rehearsal cannot satisfy the official signed
candidate gate.

Finally run the same-revision, fail-closed V0.17 composition in `V017_RELEASE_EVIDENCE.md`. It
re-verifies the timestamped package, overnight and 72-hour campaigns, provenance-bound UI raster and
explicit visual review, deterministically regenerated physical client matrix and source bundles, and
both hosted workflow attestations. The resulting hash ledger proves V0.17 evidence composition only;
V1 still requires the independently collected V0.15.1 corpus to meet its one-shot diagnostic gate.

After both chains pass, run the final fail-closed composition in `V1_RELEASE_EVIDENCE.md`. It
re-verifies every V0.17 source, requires the exact signed packaged dogfood evaluator, recomputes the
canonical held-out report, and matches its calibration/report fingerprints to the complete passing
one-shot attempt. Only its independently rechecked `v1_release_evidence_satisfied=1` ledger can close
the evidence-composition requirement; the ledger cannot replace any retained source artifact.

The current V0.18 package is portable and has no background updater or installer. It can optionally
create one current-user Run value for launch-at-login; disabling **Start with Windows** removes it.
Updates are side-by-side and recoverable: exit BlackBox from its tray menu, verify the new ZIP and
signatures, extract to a new directory, and retain the prior directory until verification succeeds.
Before the first public release only the current schema-v1 archive/settings/dataset formats are
accepted; pre-release builds do not promise compatibility with obsolete local artifacts. Use the
Settings page to create a verified archive backup before testing another build. A failed new binary
does not overwrite the previous directory or rewrite an incompatible archive.

Uninstall means disabling **Start with Windows**, exiting BlackBox, and deleting its extracted
program directory. User data is
deliberately retained at `%LOCALAPPDATA%\BlackBox`. To erase captured data first, use the explicit
confirmed purge action in Settings; then delete `incidents.sqlite3`, its `-wal`/`-shm`
sidecars if present, `settings.ini`, and `product-settings.ini`. No service, driver, scheduled task, shell extension, or
machine-wide registry key is installed.

## Privacy and retention

Normal recording remains RAM-only. SQLite writes happen only for accepted incidents, annotation or
feature/profile updates, and explicit maintenance. `PRAGMA secure_delete=ON` overwrites deleted
SQLite cells. Offline `keep-newest` and `prune-before` maintenance is transactional, cascades all
incident-owned rows, removes orphaned profiles, truncates the WAL, and compacts only because the
user explicitly requested maintenance. `purge-all` additionally requires the literal
`DELETE-ALL` confirmation token and compacts the empty archive. BlackBox never performs silent
automatic deletion.
