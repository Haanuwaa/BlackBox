# V0.15.1 multi-hardware collection campaign

## Goal

Produce the first V0.15.1 corpus that can honestly test diagnostic usefulness across machines. The
campaign is complete only when a newly collected direct-format-v1 corpus passes `readiness`, freezes
without reusing any V0.15 held-out incident, and passes the one-shot held-out gate. Controlled
workloads and the earlier single-host corpus cannot substitute for natural evidence.

## Before collection

1. Build one Release revision and record its executable hashes.
2. Record `blackbox_dogfood_tool fingerprint`; every capture and evaluation machine must use the
   same pipeline version and configuration fingerprint.
3. Create a new corpus with `blackbox_dogfood_tool init <directory> <corpus-id>`.
4. Obtain the participant's agreement to local telemetry collection and privacy-reduced campaign
   use before the session. Set `consent_attested=1` only after that confirmation; missing or false
   consent is structurally rejected by protocol V1.
5. Assign pseudonymous hardware profiles, session operators, session IDs, and frozen split roles
   before reviewing any prediction. Do not use account, host, device, endpoint, or path identifiers.
6. Assign two annotator pseudonyms per calibration and held-out incident. Neither may equal the
   linked session operator.

No current-format file is produced by converting the V0.15 corpus. The earlier `incidents.tsv` is
used only as the explicit exclusion list at freeze.

## Minimum acquisition matrix

At least three coarse hardware profiles must each contribute all of the following:

| Evidence | Calibration | Held out |
|---|---:|---:|
| Natural session | at least 1 | at least 1 |
| Quiet exposure | at least 1 hour | at least 1 hour |
| Scorable incident truth | at least 1 | at least 1 |

Across the complete corpus:

- quiet exposure totals at least ten hours;
- scorable calibration and held-out truth each cover CPU starvation, disk stall, network
  interruption, application crash, application hang, game stutter, audio interruption, quiet, and
  ambiguous symptoms;
- calibration contains at least ten scorable rows whose expected diagnosis is supported;
- held out contains at least ten scorable truth rows;
- every declared incident has exactly the declared number of source ballots;
- controlled and development captures remain distinguishable from natural evidence.

More evidence is preferable when it broadens hardware, power mode, OS build, workload, and genuine
symptom diversity. Repetition on one machine does not repair a missing profile or symptom class.

## Capture and archive handling

Keep one schema-v1 SQLite archive per hardware profile. The capture operator records provider
failures, drops, capture rejection, workload timing, and whether the symptom was actually noticed.
A capture with collection failure or dropped telemetry is retained as an exclusion, not silently
promoted to scorable truth.

Create a local tab-separated archive map:

```text
hardware_profile_id\tarchive_path
profile-a\tcaptures/profile-a.sqlite3
profile-b\tcaptures/profile-b.sqlite3
profile-c\tcaptures/profile-c.sqlite3
```

The map is not part of the corpus and must not be published. The evaluator requires every
represented profile, refuses one archive reused for multiple profiles, rejects duplicate incident
keys across archives, and verifies every incident against its session profile. Map only existing
regular archive files; physical aliases to the same file are rejected.

For a completed consented quiet exposure with no automatic captures, create the packet atomically
with the dedicated prediction-free helper:

```powershell
.\scripts\new-consented-quiet-session-packet.ps1 `
  -DogfoodTool .\out\build\windows-vs2026-release\src\Release\blackbox_dogfood_tool.exe `
  -BaseCorpusDirectory .\corpus-000 -OutputPacketDirectory .\quiet-packet-001 `
  -ProfileId profile-a -OsBuildBucket windows-11-24h2 -CpuFamily x64-family-a `
  -LogicalProcessors 16 -MemoryGibBucket 32-63 -GpuFamily gpu-family-a -PowerMode ac `
  -SessionId quiet-a-cal-001 -OperatorId operator-a -Split calibration `
  -DurationSeconds 3600 `
  -ConsentAttestation PARTICIPANT-CONSENT-CONFIRMED `
  -ExposureAttestation QUIET-EXPOSURE-COMPLETED `
  -NoCaptureAttestation NO-AUTOMATIC-CAPTURES
```

Supply those exact tokens only after the participant consented, the declared exposure actually
finished, and the app recorded no automatic capture during it. The helper cannot infer or measure
those facts. If any capture occurred, preserve it and use the full archive-backed, independently
annotated session-packet workflow; never erase a false capture to improve the quiet denominator.

## Independent annotation

Each annotator records a separate `annotations.tsv` ballot without seeing the analyzer prediction
or the other ballot. Local process ordinals may be obtained with
`blackbox_dogfood_tool inspect <archive> <incident-key>`; process names printed by that command stay
local and never enter the corpus.

After an annotator completes the blank ballot in an ordinal-only truth-review directory, validate
it before handoff:

```powershell
blackbox_dogfood_tool validate-ballot <review>/ballot-template.tsv `
  <expected-incident-key> <session-operator-id>
```

The command accepts only a non-link regular file up to 4 KiB, requires the exact protocol-v1 header
and one completed row, binds the row to the
expected incident, rejects the collection operator as annotator, and uses the same parser as corpus
loading. Its output contains only validity, prediction-free status, incident key, and annotator
pseudonym—not the diagnosis-bearing fields. It does not compare annotators or choose consensus.

Once both independently completed ballots have been handed to the coordinator, mechanically derive
the required disagreement flag without printing either payload:

```powershell
blackbox_dogfood_tool compare-ballots <ballot-a.tsv> <ballot-b.tsv> `
  <expected-incident-key> <session-operator-id>
```

Both ballots pass the same binding and bounds checks, must name distinct annotators, and produce
only validity, incident/annotator pseudonyms, `annotator_count=2`, and `disagreement=0|1`. Agreement
does not make a ballot correct, and disagreement does not choose a winner; the coordinator still
records the explicitly adjudicated consensus truth.

After both ballots exist, create the consensus `incidents.tsv` row. Its `annotator_count` must equal
the distinct ballot count. Set `disagreement=1` whenever any ballot differs from the consensus truth
fields. Disputed, uncertain, and unresolvable evidence remains in coverage but is not primary score
truth. Do not erase disagreement to increase a metric.

## Readiness, freeze, and evaluation

Run throughout collection:

```powershell
blackbox_dogfood_tool validate <corpus>
blackbox_dogfood_tool readiness <corpus>
blackbox_dogfood_tool campaign-status <corpus> <new-status-directory>
```

`validate` returns success for a structurally valid collecting corpus. `readiness` prints per-profile
and per-symptom status plus every unmet requirement and exits with code 3 until all freeze gates are
met. `campaign-status` publishes the same qualification state as an exact six-file schema-v1
directory with machine-readable TSVs and a self-contained `status.html`. It refuses occupied final
or sibling `.partial` destinations and publishes atomically. The page is explicitly prediction-free
and evidence-neutral: it helps coordinators find missing profiles, splits, quiet hours, truth, and
symptom classes, but it does not create evidence or satisfy any gate.

When a packet or corpus is invalid, validation names the exact safe pseudonymous session, incident,
operator, or annotator involved and prints declared versus observed counts/disagreement where
applicable. These diagnostics contain no ballot payload, process identity, archive path, or analyzer
result; use them to repair the draft packet before immutable merge.

Freeze exactly once against the previous held-out table:

```powershell
blackbox_dogfood_tool freeze <corpus> <v015-incidents.tsv>
```

Then fit calibration without reading held-out truth for development decisions:

```powershell
blackbox_dogfood_tool evaluate <archive-map.tsv> <corpus> calibration none <new-calibration-output>
```

Finally run held out once:

```powershell
blackbox_dogfood_tool evaluate <archive-map.tsv> <corpus> held_out `
  <new-calibration-output>/calibration.tsv <new-heldout-output>
```

The held-out command writes and locks a complete failure as well as a pass. V0.15.1 requires at
least 80% supported-diagnosis precision, 60% supported recall, 90% Unknown-truth abstention, and 70%
contributor top-3 with nonzero denominators. A miss is a product finding, not permission to rerun or
relabel the split.

The evaluator acquires its lock before any held-out diagnosis. It writes outputs to
`<output>.partial` and publishes the final directory atomically only when every required file is
complete. Check the durable state with:

```powershell
blackbox_dogfood_tool heldout-status <corpus>
```

`running` after a crash or operational error is an auditable consumed attempt, not permission to
delete the lock silently. Preserve the lock and partial directory, diagnose the failure, and record
any decision about whether the entire campaign must be restarted. Run evaluation only against
offline archive copies; do not leave the desktop writer mutating a mapped archive during the run.

## Evidence retained

Retain together: source revision, Release binaries and hashes, pipeline/configuration fingerprint,
five-file frozen corpus, exclusion table hash, archive-map template without private paths, per-archive
hashes, calibration artifact and fingerprint, evaluation output and fingerprint, one-shot attempt
directory, hardware distribution, exclusions,
annotation disagreements, and separate natural/controlled results. These artifacts remain
engineering evidence; an unsigned developer build is not a public V1 release claim.
