# Testing and qualification

[Documentation index](README.md) · [Current candidate](CURRENT_CANDIDATE.md)

- [UI qualification](#ui-qualification)
- [Clean-client and physical qualification](#clean-client-and-physical-qualification)
- [Wall-clock soak campaigns](#wall-clock-soak-campaigns)

## UI qualification

Screen-reader support is unqualified. Windows has a UI Automation semantic bridge for basic
controls, focus, values and status changes, with bounded actions executed on the UI thread.
Linux/macOS do not yet have equivalent AT-SPI/NSAccessibility bridges for ImGui controls.
Keyboard navigation, high contrast, exports and automated provider tests do not establish
complete assistive-technology support.
Narrator, VoiceOver and Orca qualification must record actual names, focus, values and announcements,
including unsupported workflows. Native file picker cancellation keeps the previous path;
overlong UTF-8 selections are rejected without truncation. See [current gates](CURRENT_CANDIDATE.md).

BlackBox uses deterministic software raster evidence to catch native UI rendering regressions before
the physical Windows client matrix. This is a development qualification aid, not a substitute for
testing the packaged application on real displays and assistive configurations.

The product UI uses one code-native semantic visual system: restrained navy surfaces, explicit
text/accent/success/warning roles, consistent padding and rounding, a system-native UI font when one
is available, and a reversible maximum-contrast palette. Live leads with recording state and one
primary capture action. Collection profiles, capture preferences, archive maintenance, and
permanent removal use one Settings scroll area. The raster
runner and production app share the same platform-font loader and fallback. Compare the same
font installation and DPI context when reviewing raster output. Fixtures scale the font and style
at the target DPI and settle page layout before capture; they do not enlarge a 100% bitmap.

### Generate the direct-v1 evidence bundle

Build the Release tests, then choose a new destination that does not already exist:

```powershell
cmake --build out/build/windows-vs2026-release --config Release --target blackbox_tests
.\scripts\run-ui-qualification.ps1 `
  -OutputDirectory .\out\ui-qualification-raster-v1-<revision> `
  -SourceRevision <40-hex-revision>
```

The runner refuses an existing final or `.partial` destination. It renders representative and
large incident fixtures through the SDL3 software renderer at these two page modes:

- 1100 x 700 logical/physical pixels at 100%, using the normal dark palette
- 1100 x 700 logical pixels on a 1650 x 1050 physical surface at 150%, using the high-contrast
  palette

Every Live, Incidents, Detail, Patterns, Settings, and Diagnostics page is rendered in both modes
for both fixtures. Four additional scrolled Detail cases pin a sticky cursor at -15 seconds and
show it beside the independent incident marker across a lower synchronized timeline and the process
table. Two representative Detail cases explicitly expose the feedback reset, one-step rollback,
and preceding/marker-spanning/post-marker contributor roles with their attribution controls. A
compact 800 x 600 first-run case protects responsive keyboard guidance at 100%; a second 800 x 600
logical case renders on a 1600 x 1200 surface at 200% with high contrast. A passing bundle contains
exactly 32 BMP files, `summary.ini`, and `manifest.sha256.ini`.
The runner and independent verifier validate every filename, exact V5 BMP structure/payload,
dimension, SHA-256 digest, summary hash, source revision, and test/runner/verifier identity before one
same-volume rename publishes the directory. Re-run `verify-ui-qualification.ps1` on the immutable
result. A failed or interrupted attempt remains visibly
`.partial`. `format=1` is the only pre-release evidence format; there is no migration or
compatibility reader.

The Release test executable is compiled with the configure-time source revision. When evidence
output is enabled it compares that identity with `-SourceRevision` before rendering the first case;
a caller cannot relabel a raster generator built from another tree. `local-uncommitted` remains
available only for explicit rehearsal output.

`summary.ini` deliberately records:

```ini
manual_visual_review_required=1
physical_matrix_satisfied=0
```

Automated generation therefore cannot close the V0.17 physical qualification gate.

### Visual review

Review all 32 images at native pixel size. Record the bundle manifest hash and any finding outside
the bundle; do not edit generated evidence. Verify:

- the complete application viewport is rasterized, with no unused high-DPI canvas caused by an
  incorrect renderer scale;
- the six navigation targets remain visible and the active page is distinguishable;
- every Live status value, including the complete product version, remains visible rather than
  collapsing to a one-character column;
- Live leads with an unambiguous recording/capture state and its single primary capture action;
  technical status, forensic telemetry, rolling history, and active-process disclosure controls
  remain visible and keyboard reachable without making their dense contents the default view;
- text, controls, tables, plot regions, the incident marker, and timeline sections are not clipped
  or overlapped at the top, left, or right edges;
- the cyan synchronized cursor remains distinct from the orange incident marker at both scales,
  and wide PIDs or large metric values do not collide with adjacent process-table columns;
- feedback calibration status, confirmed-similar historical context, reset confirmation/action,
  rollback action, all three temporal-role labels, contributor-attribution selectors, raw/adjusted
  scores, and calibration state remain legible at 100% dark and 150% high contrast;
- long large-fixture content remains scrollable rather than silently truncated;
- high-contrast text, disabled text, selection, buttons, borders, accents, and warnings remain
  distinguishable without color being the only state cue;
- switching Windows high contrast on and off while the dashboard is open updates every semantic
  control color within one second and restores the normal palette without restarting; the Live page
  also reports the current system-animation preference;
- user-visible punctuation renders as a real glyph rather than `?`; the bundled default ImGui font
  is Basic Latin, so the UI source is intentionally ASCII-only and a regression test enforces that
  boundary;
- incident symptom, contributor, uncertainty, capture provenance, feedback controls, and evidence
  caveats remain readable in Detail; the collapsed evidence control follows the headline and at
  most three contributor previews without implying that hidden raw evidence was discarded;
- Settings and Diagnostics expose recovery/status information without hidden or ambiguous actions.
- compact and 200% onboarding keep every step scrollable, preserve visible keyboard focus, expose
  recorder/capture/storage/privacy readiness, and retain one unambiguous full-width completion action.

The physical walkthrough must additionally exercise first-run completion and the Incidents loading,
empty, no-match, and archive-unavailable presentations. Each state must name a safe next action,
preserve keyboard focus visibility, and avoid claiming that unsaved or filtered evidence was deleted.

Raster hashes are expected to change when rendering, fonts, layout, or fixture content changes.
Review the new images and retain a new complete bundle instead of blessing individual changed hashes.
After all 32 pass, use `record-ui-visual-review.ps1 -ConfirmAllCasesPassed` and independently run
`verify-ui-visual-review.ps1`. The separate review directory binds the exact raster manifest and
cannot exist without explicit confirmation. `RELEASE_EVIDENCE.md` contains the full commands.

### Physical Windows matrix

The release revision still requires clean, ordinary-user Windows 10 22H2 and supported Windows 11
hosts. At minimum record OS build, BlackBox/package hash, CPU/GPU, display topology and work areas,
per-display scale, resolution, power source/mode, keyboard/accessibility settings, and result for:

- 100%, 125%, 150%, and 200% display scaling, including moving the window between monitors with
  different scales and work areas;
- keyboard-only navigation, focus visibility, tab order, activation, scrolling, dialogs, and global
  capture hotkey behavior;
- Windows high contrast and increased text/scale configurations;
- repeated live standard/high-contrast transitions, including changing the preference while the app
  is hidden and then reopening it from the tray;
- single- and multi-monitor placement, taskbar/work-area changes, disconnect/reconnect, and resume;
- low-end hardware, battery operation, battery saver, balanced, and performance power modes;
- packaged hidden startup, tray recovery, capture, incident inspection, settings, diagnostics, and
  orderly exit.

Record defects and rerun the affected cases on the same source revision after correction. Software
raster success proves deterministic page output only; it does not prove Windows DPI notifications,
font quality, input accessibility, real GPU behavior, monitor transitions, battery impact, or usable
layout on the supported client matrix. `TESTING.md` defines the immutable package-bound
interactive profiles and aggregate verifier used to retain that evidence.

## Clean-client and physical qualification

The V0.17 client gate qualifies the portable ZIP that would be released, not a binary left in a
developer build tree. BlackBox uses direct-format-v1 evidence with no migration or compatibility
reader. A local automated smoke is useful wiring evidence, but it cannot represent a clean-client,
accessibility, display, battery, or signing pass.

### Package smoke

Build the current Release configuration, run CPack from its build directory, generate the checksum,
and choose a new evidence destination:

```powershell
cmake --build out/build/windows-vs2026-release --config Release
Push-Location out/build/windows-vs2026-release
cpack --config CPackConfig.cmake -C Release
Pop-Location
./scripts/write-release-checksum.ps1 `
-PackagePath ./out/build/windows-vs2026-release/BlackBox-0.27.0-windows-x64.zip
./scripts/run-client-qualification.ps1 `
-PackagePath ./out/build/windows-vs2026-release/BlackBox-0.27.0-windows-x64.zip `
  -OutputDirectory ./out/client-smoke-<revision> -Mode smoke
```

The runner first copies the ZIP and sidecar into fresh staging and verifies that exact copy. The
package verifier requires one exact package-named ZIP root, safe non-duplicated paths, bounded entry
counts and sizes, the three shipped executables, release/user documentation, and one exact
filename-bound SHA-256 sidecar. `-RequireAuthenticode` additionally requires every shipped
executable to have a valid trusted Authenticode signature and timestamp. Signing only the desktop
executable is not sufficient.

Smoke mode refuses to start while any other `blackbox.exe` process is running, extracts the verified
ZIP into a fresh staging directory, redirects both settings files and the schema-v1 archive, launches
the packaged desktop executable for a bounded hidden diagnostic, checks collection/writer/archive
health and the 80 MiB working-set gate, and binds the ZIP, launched application, qualification
runner, evidence verifier, and every executable hash/signature state. It records
`single_host_profile_satisfied=0`,
`clean_client_matrix_satisfied=0`, and `physical_matrix_satisfied=0` by construction.

The runner invokes the verifier before publication. Re-run it independently on the immutable final
directory before relying on the bundle:

```powershell
./scripts/verify-client-evidence.ps1 `
  -CampaignDirectory ./out/client-smoke-<revision>
```

For an official interactive bundle, add `-RequireInteractive -RequireAuthenticode`. Never pass
`-AllowStaging` during independent review; that switch exists only for the runner's pre-publication
check of its own `.partial` directory. The package-smoke example above deliberately names the
current `0.27.0` engineering build. Final release evidence must instead use the exact signed
`BlackBox-1.0.0-windows-x64.zip`; no prerelease filename can satisfy the V1 composition gate.

### Interactive profiles

Run these only as an ordinary user on clean x64 client installations. No other BlackBox instance
may be running. Interactive evidence requires the exact lowercase 40-character source revision
used to build the package:

```powershell
./scripts/run-client-qualification.ps1 `
  -PackagePath C:/qualification/BlackBox-1.0.0-windows-x64.zip `
  -OutputDirectory C:/qualification/win11-standard-<revision> `
  -Mode interactive -Profile standard -SourceRevision <40-hex-revision>
```

Before extraction or launch, the runner passes that revision to the strict package verifier. All
three packaged executables must expose the same embedded source identity, so an operator-supplied
revision cannot relabel a package built from another commit. This check happens before any physical
case is recorded.

The runner launches the extracted application visibly and prints its `.partial` campaign path.
Perform one required observation at a time from a second PowerShell window and record only what was
actually observed:

```powershell
./scripts/record-client-case.ps1 `
  -CampaignDirectory C:/qualification/win11-standard-<revision>.partial `
  -Case tray_hide_restore -Result pass
```

The helper accepts only a case required by that running profile, refuses duplicates, verifies that
the packaged process is still alive **and has the exact packaged application hash**, and stores only
UTC time, case identifier, and pass/fail. A failed case is evidence of failure and must not be
changed to pass; fix the defect and run a new campaign. After every case is recorded, exit BlackBox
using the tray menu. The runner requires an orderly zero exit and then runs the same isolated package
diagnostic as smoke mode before it can publish.

Profiles have non-overlapping purposes:

- `standard`: ordinary-user launch, keyboard-only first-run onboarding, focus visibility under
  increased text/display scaling, tray restore, hotkey/capture/viewer/settings/diagnostics,
  keyboard-only use, live and hidden high-contrast transitions, and 100/125/150/200% scaling;
- `multimonitor`: mixed-scale movement, taskbar/work-area changes, monitor disconnect/reconnect, and
  suspend/resume; at least two active displays are mechanically required;
- `low-end`: responsiveness and resource bounds on a host with at most 8 GiB RAM or at most four
  logical processors;
- `battery`: battery operation, battery saver, balanced/performance modes, and battery
  suspend/resume; a detected battery is mechanically required.

The interactive `standard` profile deliberately launches with onboarding incomplete. Record
`first_run_onboarding_keyboard` only after completing the modal without a pointer: traverse every
focus target with Tab/Shift+Tab, scroll its bounded content, and activate the final action with the
keyboard. Record `focus_visibility_text_scaling` only after confirming visible focus, legible text,
and reachable controls at increased text size and each required display scale. These operator cases
add evidence obligations; they do not make raster output or one development host physical proof.

Host evidence deliberately excludes computer name, username, serial numbers, device IDs, local
paths, and operator notes. It records OS family/build/edition, architecture, CPU/GPU, memory and
logical processors, process count, power state/scheme, accessibility preferences, and bounded
display bounds/work areas/DPI. Windows family is derived from the kernel build because Windows can
report a legacy product-name string on newer clients.

### Matrix verification

Retain at least five independent interactive bundles for the exact same ZIP and source revision:

- a `standard` pass on clean Windows 10 22H2;
- a `standard` pass on supported Windows 11;
- one `multimonitor` pass;
- one qualifying `low-end` pass;
- one qualifying `battery` pass.

Then aggregate them without editing any bundle:

```powershell
./scripts/verify-client-matrix.ps1 `
  -EvidenceDirectory C:/qualification/win10-standard, `
                     C:/qualification/win11-standard, `
                     C:/qualification/multimonitor, `
                     C:/qualification/low-end, `
                     C:/qualification/battery `
  -OutputDirectory C:/qualification/client-matrix-<revision> `
  -RequireAuthenticode
```

The matrix invokes the independent single-bundle verifier for every input. That verifier rejects
partial output by default, links, extra files or even extra empty directories, oversized inputs,
locale-dependent or malformed process rows, and any mismatch between the copied package, launched
application, runner/verifier provenance, summary, campaign, or manifest. It streams a bounded ZIP
header check rather than loading the archive into memory, re-verifies each embedded ZIP and checksum,
validates the direct-v1 settings/report and SQLite signature, and checks every exact operator case.
The matrix then rejects reuse of one bundle and requires common package/revision identity. It
publishes only after every coverage rule passes. Omit `-RequireAuthenticode` for a pre-signing
rehearsal; only the signed invocation can produce `official_signed_matrix_satisfied=1`.

Retain every source bundle. Independently recheck a published matrix by regenerating it from those
same bundles and requiring exact byte-for-byte agreement:

```powershell
./scripts/verify-client-matrix-evidence.ps1 `
  -MatrixDirectory C:/qualification/client-matrix-<revision> `
  -EvidenceDirectory C:/qualification/win10-standard, `
                     C:/qualification/win11-standard, `
                     C:/qualification/multimonitor, `
                     C:/qualification/low-end, `
                     C:/qualification/battery `
  -ExpectedSourceRevision <40-hex-revision> -RequireAuthenticode
```

A passed one-host bundle contains the ZIP and sidecar, `app-report.ini`, `campaign.ini`, `host.ini`,
the isolated `data/` files, case/process journals, `summary.ini`, and `manifest.sha256.ini`.
Destinations are immutable: existing final or `.partial` paths are refused, failures remain
`.partial`, and success uses one same-volume directory rename. Do not rename, repair, or reuse failed
evidence.

## Wall-clock soak campaigns

The V0.17 wall-clock gate is an elapsed-time release qualification, not a fast unit test. The
campaign runner launches the assembled Release app hidden, uses a fresh settings directory and
schema-v1 archive, records bounded process samples and app diagnostics, and publishes the evidence
directory only after validation passes. It never opens or modifies the user's normal archive.
The isolated profile registers Ctrl+Shift+Alt+F10 so the long-running app does not reserve the
product default Ctrl+Shift+F12 used by integration tests or an ordinary installation.

### Campaigns

Run from the repository root in a normal interactive Windows session:

```powershell
# Wiring smoke; this does not satisfy either release soak.
./scripts/run-wall-clock-soak.ps1 -Mode smoke `
  -OutputDirectory ./out/soaks/smoke-20260821

# At least eight elapsed hours.
./scripts/run-wall-clock-soak.ps1 -Mode overnight `
  -SourceRevision <40-or-64-digit-release-revision> `
  -OutputDirectory ./out/soaks/overnight-20260821

# Exactly 72 elapsed hours, including the operator actions below.
./scripts/run-wall-clock-soak.ps1 -Mode 72-hour `
  -SourceRevision <40-or-64-digit-release-revision> `
  -OutputDirectory ./out/soaks/72-hour-20260821
```

The runner and standalone verifier support both inbox Windows PowerShell 5.1 and current PowerShell.
Bundle paths are normalized only after proving that their full paths remain beneath the campaign
root; no newer `.NET` `Path.GetRelativePath` API is required. Contract tests verify an accepted
bundle through Windows PowerShell 5.1 when it is installed.

Before launching timed work, the runner invokes the assembled executable's
`--validate-settings-only` mode against both generated direct-v1 settings files. This uses the same
production parsers as startup and fails before elapsed work if a required field is absent, unknown,
or invalid. The mode cannot be combined with background/runtime arguments and does not construct the
application, collector, shell, archive, analysis, or UI.

Binary and fault-probe source revisions are checked before timed work. The runner copies the
application and all adjacent runtime DLLs into an isolated `runtime/` directory and preserves their
SHA-256 inventory. Settings preflight has a 30-second watchdog. During the run, `app-progress.ini`
updates every five seconds; terminal collector/unexpected writer failures stop the run early.
Collection progress must advance within 120 seconds, and app shutdown must complete by requested
duration plus 60 seconds. `failure.txt` preserves the observed failure reason in partial evidence.
Every five minutes the runner lists missing 72-hour operator attestations.

Only smoke mode accepts a shortened `-DurationSeconds`. Overnight is fixed at 28,800 seconds and
72-hour is fixed at 259,200 seconds so release evidence cannot silently substitute a shorter run.
Their cadences are fixed too: overnight captures every 900 seconds with a 60-second process
checkpoint, while 72-hour captures every 1,800 seconds with the same 60-second checkpoint. Long-mode
cadence overrides are rejected before application launch. Under the pinned one-second post-window
and completion margin, that requires at least 31 overnight or 143 72-hour scheduled incidents.
The default Release executable and development-only archive-fault probe are selected from
`out/build/windows-vs2026-release/src/Release`. Explicit paths may be supplied when qualifying a
different build.

To validate the real archive-fault path without claiming release soak evidence, run a smoke long
enough to include a failed capture and a later recovery:

```powershell
./scripts/run-wall-clock-soak.ps1 -Mode smoke -DurationSeconds 60 `
  -CaptureIntervalSeconds 10 -CheckpointSeconds 2 -ExerciseArchiveFault `
  -OutputDirectory ./out/soaks/archive-fault-smoke-20260821
```

Fault injection waits for the app's first successful write and uses its heartbeat clock. Fault
rehearsals require capture intervals of at least ten seconds and at least four scheduled captures;
shorter intervals do not leave enough margin for startup, the post-window and bounded retries.

The 72-hour runner automatically takes a write lock on its isolated archive across a scheduled
capture. This must produce bounded writer retry exhaustion, retain the failed immutable incident,
release the lock, export the failed incident, and explicitly retry it after a later normal write
succeeds. The fault probe is never installed or shipped
and cannot target the user's archive because the runner supplies its private campaign path.

Qualification requires exactly one exhausted write, capture sequence 2, a failure timestamp inside
the injected fault window, zero unretained incidents, one explicit recovery and an empty recovery
slot at completion. The manifest includes `recovered-incident.sqlite3` for this campaign.

### Separate product-default rehearsal

```powershell
./scripts/run-wall-clock-soak.ps1 -Mode smoke -Profile defaults `
  -OutputDirectory ./out/hardening/default-profile
```

This 240-second rehearsal uses automatic detection, notifications, executable paths, 300-second
history and 120/30-second windows. Optional context stays at product defaults. Its archive path,
completed onboarding and nonconflicting qualification hotkey are isolated. After 130 seconds of
warmup, an owned 20-second CPU workload uses up to 64 threads. The rehearsal requires an actual
automatic capture, a merged manual trigger and visible/minimized/hidden transitions. Missing
coverage fails rather than silently passing. This is separate from the elapsed-time release gates.

### Required 72-hour operator actions

Perform each action while the campaign is running, then attest the completed action against the
`.partial` campaign directory:

```powershell
./scripts/record-soak-event.ps1 -CampaignDirectory ./out/soaks/72-hour-20260821.partial `
  -Event sleep_resume
./scripts/record-soak-event.ps1 -CampaignDirectory ./out/soaks/72-hour-20260821.partial `
  -Event lock_unlock
./scripts/record-soak-event.ps1 -CampaignDirectory ./out/soaks/72-hour-20260821.partial `
  -Event device_churn
```

- `sleep_resume`: put the machine to sleep long enough to cross multiple sample deadlines, resume,
  and wait for normal collection to restart. The app report must independently contain a resume
  event; the journal alone is insufficient.
- `lock_unlock`: lock Windows, sign back in, and confirm the tray process remained alive. The shell
  must independently report both WTS session transitions and that native session notifications were
  available; the journal alone is insufficient.
- `device_churn`: safely disconnect/reconnect a test USB or audio device that the operator owns.
  Do not disrupt a production device or another user's session. The event collector must
  independently record at least one device or audio event during the campaign.

The archive-fault start and recovery events are written by the runner, not the operator. Journal
rows are bounded UTC/type attestations with no notes, usernames, device names, or paths. Aggregate
native counters corroborate that matching session/device activity occurred, but intentionally do
not retain identifiers or claim which physical device produced a transition.

### Evidence and pass conditions

A passed directory contains:

- `app-report.ini`: path-free direct-v1 collector, capture, event, writer, archive, shell, and crash
  counters emitted after orderly drain, including the compiled source revision and effective
  automatic-detection/trigger/capture/event-request counters;
- `app-progress.ini`: bounded live counters;
- `runtime/` and `runtime-inventory.ini`: preserved executable/DLL inputs and hashes;
- `process-samples.tsv`: UTC/elapsed resource checkpoints;
- `operator-events.tsv`: bounded operator and automatic fault events;
- `summary.ini`: duration, fixed cadence, coverage minimums, logical-processor count, independently
  reproducible resource maxima, sampling gaps, total-machine CPU, and steady-state growth;
- `manifest.sha256.ini`: SHA-256 bindings for the report, settings, summary, and journals;
- `data/`: the isolated direct-v1 settings and archive for local investigation.

The runner requires completed duration, a healthy schema-v1 archive, sample coverage, internally
consistent capture/writer/archive counts, no collector/event worker failures, no sample drops or
deadline misses, no unexplained long gaps or unretained incidents, and no unexpected writer failures.
The isolated profile requires automatic detection, detector triggers, automatic captures and
event-driven requests to remain disabled; the defaults profile requires automatic capture. Long modes
also enforce an 80 MiB maximum
working set, 1% average total-machine CPU, and bounded first-to-last ten-checkpoint growth of 16 MiB
working set, 16 MiB private memory, and 32 handles. Every recorded event must be accounted for by
one of the ten bounded source counters. The 72-hour mode additionally requires all operator events,
native session-notification availability, lock and unlock counts, device/audio activity, and
independent resume and writer fault/recovery counters.

Coverage is explicit rather than inferred from a passed duration. Overnight requires at least 455
of its 479 nominal process checkpoints, 27,360 one-second collections, and 31 scheduled incidents.
The 72-hour campaign requires at least 4,103 of 4,319 nominal process checkpoints, 246,240
collections, and 143 scheduled incidents. The 95% process/collection allowance accommodates the
required real sleep/resume exercise while preventing a sparse journal from qualifying. Every
sampling gap is still recomputed and published.

Native resume notifications reset scheduling and increment the resume counter independently of
thread ordering. Suspend alone resets rate baselines without counting as resume or excusing missed
deadlines. A long-gap fallback resets rates but remains unclassified until matching timestamped
native resume evidence arrives. Unrelated events cannot excuse gaps. Unexplained gaps fail even
when ordinary dropped-tick counters are zero. Keep physical sleep brief and between capture markers:
the collection allowance permits at most 5% missing time and scheduled-capture coverage still applies.

Timing percentiles and window maxima cover the last 256 observations, or fewer at startup.
`timing_window_samples` states that count. Collection/jitter lifetime maxima retain spikes since
the collector configuration epoch began; they are not whole-run percentiles.

Windows process identity metadata is not equivalent to handle ownership. The collector may retain
bounded metadata for every readable identity but keeps at most 16 live process handles; uncached
queries must release their temporary handle before the sample returns. Before freezing a long-run
revision, repeat a short smoke under controlled process churn and inspect both maximum and
first/last steady-window handle counts. A smoke is diagnostic only and never substitutes for the
72-hour resource-growth gate.

The runner stages to `<output>.partial`. Cancellation, power loss, app failure, a failed gate, or a
runner error leaves that directory in place and never turns it into passing evidence. A successful
campaign atomically renames the staging directory to the requested output. Never rename a failed
or interrupted directory by hand.

The final direct-v1 manifest binds the archive, settings, journals, completed checkpoint, report,
and summary. Campaign/summary provenance also binds the application, runner, verifier, optional
fault probe, and caller-supplied source revision; the application report must declare that same
compiled revision, and the runner rehashes every executable input before publication and fails if
one changed during elapsed time. `local-uncommitted` is permitted for
pre-release harness work but cannot identify a release revision. The runner invokes
`verify-wall-clock-soak.ps1` against staged evidence before atomic
publication. Re-run the verifier on retained evidence before using it in a release decision:

```powershell
./scripts/verify-wall-clock-soak.ps1 `
  -CampaignDirectory ./out/soaks/overnight-20260821
```

It rejects partial directories, extra/link files, invalid bounds, inconsistent counters, malformed
journals, archive/header mismatches, and any changed hash. It also requires the retained runner and
verifier hashes to match the current release-source scripts, checks the app-report cadence and all
three coverage floors, and independently recomputes elapsed/CPU monotonicity, gap count, CPU average,
resource maxima, and first/last resource growth from `process-samples.tsv`. Only the runner may write
the automatic
`archive_fault_started` and `archive_recovered` rows; the operator tool accepts only the three
physical actions.

One overnight pass and one 72-hour pass on the release revision are required before checking the
V0.17 soak milestone. A short smoke proves only that the harness and assembled app work together.

### Automated Windows accessibility smoke

`tests/scripts/windows_ui_automation_smoke.ps1` runs under Windows PowerShell with a new
isolated settings/archive directory. It checks actual UI Automation navigation and toggles,
normal-exit draft confirmation and persisted save-and-exit. The Windows Release CI job runs
this test and retains failures. It is not Narrator or physical accessibility qualification.

The fault-recovery diagnostic scheduler retains its next scheduled trigger while the
intentionally failed write is retained, then issues it after explicit recovery. Minimum
completed-capture counts, zero rejected/cancelled captures and exactly one injected exhausted
write remain required; the verifier does not waive unrelated failures. Product backpressure
itself is exercised by the accepted-slot/storage-outage regression tests.
