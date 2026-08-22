# Wall-clock soak campaigns

The V0.17 wall-clock gate is an elapsed-time release qualification, not a fast unit test. The
campaign runner launches the assembled Release app hidden, uses a fresh settings directory and
schema-v1 archive, records bounded process samples and app diagnostics, and publishes the evidence
directory only after validation passes. It never opens or modifies the user's normal archive.
The isolated profile registers Ctrl+Shift+Alt+F10 so the long-running app does not reserve the
product default Ctrl+Shift+F12 used by integration tests or an ordinary installation.

## Campaigns

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
./scripts/run-wall-clock-soak.ps1 -Mode smoke -DurationSeconds 40 `
  -CaptureIntervalSeconds 5 -CheckpointSeconds 2 -ExerciseArchiveFault `
  -OutputDirectory ./out/soaks/archive-fault-smoke-20260821
```

The 72-hour runner automatically takes a write lock on its isolated archive across a scheduled
capture. This must produce bounded writer retry exhaustion, retain the failed immutable incident,
release the lock, and show a later writer recovery. The fault probe is never installed or shipped
and cannot target the user's archive because the runner supplies its private campaign path.

## Required 72-hour operator actions

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

## Evidence and pass conditions

A passed directory contains:

- `app-report.ini`: path-free direct-v1 collector, capture, event, writer, archive, shell, and crash
  counters emitted after orderly drain;
- `process-samples.tsv`: UTC/elapsed resource checkpoints;
- `operator-events.tsv`: bounded operator and automatic fault events;
- `summary.ini`: duration, fixed cadence, coverage minimums, logical-processor count, independently
  reproducible resource maxima, sampling gaps, total-machine CPU, and steady-state growth;
- `manifest.sha256.ini`: SHA-256 bindings for the report, settings, summary, and journals;
- `data/`: the isolated direct-v1 settings and archive for local investigation.

The runner requires completed duration, a healthy schema-v1 archive, sample coverage, internally
consistent capture/writer/archive counts, no collector/event worker failures, no sample drops or
deadline misses, and no unexpected writer failures. Long modes also enforce an 80 MiB maximum
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

The runner stages to `<output>.partial`. Cancellation, power loss, app failure, a failed gate, or a
runner error leaves that directory in place and never turns it into passing evidence. A successful
campaign atomically renames the staging directory to the requested output. Never rename a failed
or interrupted directory by hand.

The final direct-v1 manifest binds the archive, settings, journals, completed checkpoint, report,
and summary. Campaign/summary provenance also binds the application, runner, verifier, optional
fault probe, and caller-supplied source revision; the runner rehashes every executable input before
publication and fails if one changed during elapsed time. `local-uncommitted` is permitted for
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
