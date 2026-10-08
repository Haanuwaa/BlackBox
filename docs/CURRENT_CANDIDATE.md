# Current candidate

Status reviewed on **7 October 2026**. BlackBox remains engineering version **0.27.0**,
with Windows as the first intended product target. No platform is release-qualified.
This page owns validation status; the other guides describe behavior and procedures.

## Committed baseline

The September hardening changes were committed in `52fe9f4`, followed by CI maintenance
in `525600f1a16734fc27e4667a8ae1905e9eeb3db8`. All four hosted workflows passed on that
latter revision, verified again on 7 October:
[Windows](https://github.com/Haanuwaa/BlackBox/actions/runs/33996307959),
[Linux](https://github.com/Haanuwaa/BlackBox/actions/runs/33996307944),
[macOS](https://github.com/Haanuwaa/BlackBox/actions/runs/33996308057), and
[quality/security](https://github.com/Haanuwaa/BlackBox/actions/runs/33996308002).
The older Mutter failure is not the current baseline result. These passes do not
validate subsequent edits.

That baseline includes native resume accounting, strict capture-loss verification,
runner watchdog/progress checks, separate product-default workloads, coordinated
purge/restore, Unicode paths, path-independent Windows process names, verified backups,
file pickers, readable incident exports, package notices and native platform presets.

## October hardening scope

- Ordinary preference changes preserve recording. Disruptive changes wait for accepted
  captures, and capture-window changes preserve existing history.
- Storage outages retain up to three already accepted snapshots and block new captures
  until recovery; recovery remains visible in the dashboard and tray.
- Normal exit protects drafts and accepted work, drains the writer off the UI thread,
  then checks for failed writes before allowing exit. Explicit discard remains available.
- Maintenance drains accepted jobs on shutdown and reports completion/failure/cancellation.
  Incremental backups use a consistent read-only connection, expose progress and support
  cooperative cancellation/deadlines without holding the active archive lock.
- Windows UI Automation exposes bounded semantic snapshots and queued control actions.
  This implementation is not a claim of complete screen-reader qualification.
- Build/CI changes add bounded MSVC compilation parallelism, native dependency caches,
  boundary-test diagnostics and CMake-derived Mac package versions.
- Documentation is consolidated into 15 guides, with current architecture and performance
  summaries and an offline local-link/anchor checker.

## Validation for this working candidate

Windows validation on 7 October used Visual Studio 2026 Release builds:

- **407/407 tests passed**, plus **193/193** in the independent collection-only graph
  with UI, storage, analysis and automatic detection disabled. After the final snapshot
  optimization, that graph was rebuilt and its 20 capture-related tests passed again.
  Local evidence is under `out/implementation-*`.
- Native UI Automation navigation, checkbox changes, unsaved-draft confirmation and
  save-and-exit passed. A five-second in-progress capture was drained and the isolated
  archive contained exactly one incident after exit.
- The 60-second injected-fault smoke saved five captures, recovered exactly one exhausted
  write, and reported zero rejected/cancelled captures, unretained failures or dropped samples.
  An earlier attempt remains a failed partial run: it identified the diagnostic scheduler's
  need to retain its trigger while recovery blocks admission.
- Workflow lint, immutable dependency policy, quality policy, local documentation links and
  release-claims contracts passed. An unsigned engineering ZIP passed checksum, runtime
  inventory, version/source identity and documentation-layout verification.
- The 500-process/150-frame snapshot benchmark averaged **39.495 ms before** and **19.741 ms
  after** replacing repeated per-row identity sorting with unique identity membership.
  Both used ten local iterations; worst observed times were 46.577 and 21.849 ms. Forty
  related capture/storage/accessibility tests passed after the optimization.
- Storage's 75,000-row workload averaged 446.3 ms (five trials), and the viewer's 500-process
  frame averaged 0.046 ms (100 iterations). These are synthetic, bounded engineering probes.
- Separate 30-second product-default probes on this 16-logical-processor Windows host
  averaged 0.289% total-machine CPU visible and 0.106% in the background. Average private
  memory was 69.2 MiB visible and 63.2 MiB background; these short probes do not establish
  long-run memory stability or performance on other machines.
- Production-font raster fixtures passed and representative settings, annotations and
  high-contrast detail images were visually inspected. The installed Windows CJK fallback
  test passed; optional system fonts and real screen-reader workflows remain separate gates.

These edited-tree binaries identify as `local-uncommitted`. The local checks are not frozen
release evidence; hosted CI must run on the committed revision. They do not replace the
physical, long-run or representative diagnostic-quality gates.

Historical September default-profile measurements are summarized in
[Performance](PERFORMANCE.md); they precede the October changes. No new 72-hour run has
been started. Original review reports and fixtures were removed after the September pass.

## Branding and dependency follow-up

The next candidate adds a shared recorder logo, Windows executable/tray resources,
macOS bundle icon, Linux desktop icon and embedded SDL window/tray image. Windows
hover text now explicitly opts into version-4 standard tooltips; the existing native
right-click menu has an automated open/dismiss check and uses the notification's anchor.

The immutable vcpkg baseline is now `2750401336fb7c95f6619657a46a7e798661341c`:
Catch2 3.16.0, ImGui upstream 1.92.9b (port version 1.92.9), ImPlot 1.0,
SDL 3.4.18 and SQLite 3.53.4. Artifact upload/download actions use 7.0.2/8.0.2,
and CodeQL uses 4.38.2, all pinned to full commits. The dependency inventory was regenerated.

Hosted Windows, Linux and macOS workflows passed for `a2a68f6`; quality failed on
unchecked UI Automation `SafeArrayPutElement` results. That failure path now handles
the HRESULT and releases its allocation. Those hosted passes precede this follow-up.

Local Windows validation passed **410/410 tests**, production MSVC native static analysis
with warnings treated as errors, workflow/dependency policies, documentation links, asset
reproducibility and SBOM generation. The extracted ZIP passed version/inventory/checksum
verification and native UI Automation navigation, draft saving and active-capture exit.
Evidence is under `out/branding-*`; binaries identify as `local-uncommitted`. New hosted
results and physical tray/screen-reader qualification remain separate from these checks.
Obsolete root CPack staging (0.15.0), duplicate package staging/extraction, root `imgui.ini`
and temporary consolidation/benchmark files were removed. Qualification evidence and
active builds were retained.

## Deferred external gates

| Gate | Remaining evidence |
| --- | --- |
| Windows endurance | Frozen-revision overnight/72-hour campaigns; physical sleep/resume, lock/unlock and device churn. |
| Physical clients | Clean Windows 10/11 installation/removal, ordinary-account recovery, real DPI/multi-monitor transitions, shortcuts and permissions. |
| Accessibility | Narrator, VoiceOver and Orca workflows; semantic coverage and real announcements. Windows UIA work does not provide macOS/Linux bridges. |
| macOS | No physical Mac is available. Oldest-OS launch, permissions, lifecycle, signing/notarization and client qualification remain open. |
| Linux | Physical GNOME/KDE/Sway sessions, portal denial/revocation/reconnect, tray/notifications, packaging and supported-hardware coverage. |
| Performance | Repeated full-sized captures, archive contention, lower-end/battery hardware and long-run memory/handle characterization. |
| Diagnostic quality and ML | Consented representative multi-machine corpus and independently annotated held-out evaluation. Runtime ML remains deferred. |

Manual qualification and data gathering are intentionally deferred. The
[release-readiness ledger](RELEASE_READINESS.md) defines acceptance, not a promise that
those gates have passed. Never promote synthetic fixtures, incomplete campaigns or
`local-uncommitted` rehearsals into release evidence.
