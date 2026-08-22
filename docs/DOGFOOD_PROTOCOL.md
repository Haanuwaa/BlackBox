# V0.15.1 local dogfood protocol

## Purpose and evidence level

Protocol version 1 records what a person or controlled workload says happened independently from
what BlackBox predicts. It is an offline engineering protocol, not telemetry uploaded by the
desktop application. The recorder, detector, UI, archive writer, and analysis pipeline never read
these files during normal operation.

Three evidence sources must remain distinguishable:

- `controlled`: a bounded workload has mechanically known timing/resource truth. It validates the
  native provider-to-diagnosis path, but a controlled frame-stall window is not a real game and a
  loopback reset workload is not an ISP interruption.
- `natural`: a person marks a problem encountered during ordinary use and annotates what they could
  verify. This is the strongest local usefulness evidence but frequently has uncertain cause truth.
- `quiet`: a timed ordinary-use exposure with no noticed incident. It supplies the denominator for
  automatic false captures per hour and tests whether post-capture analysis abstains.

Uncertain evidence is retained and published as coverage, never silently converted into correct or
incorrect truth. Controlled and natural results must be reported separately before they are
combined.

## Directory contract

`blackbox_dogfood_tool init <new-directory> <corpus-id>` creates exactly five UTF-8 files. The
destination must not already exist.

`manifest.ini` fixes:

- format `blackbox-dogfood-corpus`, protocol version 1;
- corpus identifier and collecting/frozen state;
- intelligent-analysis pipeline version and complete configuration fingerprint;
- the exact predeclared metric list;
- a nonzero annotation fingerprint after freeze.

`hardware.tsv` contains coarse, privacy-reduced profiles:

```text
profile_id  os_family  os_build_bucket  cpu_family  logical_processors  memory_gib_bucket  gpu_family  power_mode
```

Do not write device names, serial numbers, account names, host names, exact paths, or network
identifiers. Bucket values use 64-byte identifiers containing only letters, digits, `.`, `_`, or
`-`. At most 128 profiles are accepted.

`sessions.tsv` contains:

```text
session_id  hardware_profile_id  operator_id  split  kind  symptom  duration_seconds  expected_incidents  automatic_captures  consent_attested
```

The quiet-session duration is the only false-capture/hour denominator. A quiet session with no
capture is valuable and does not need a fabricated incident row. Sessions are capped at 2,000 and
one week each.

`operator_id` is a campaign-local pseudonym, not an account or device identity. Session kind is
`controlled`, `natural`, or `quiet`. `quiet` must pair with the `quiet` symptom; the other two may
not. Declared incident counts must exactly match truth rows, and automatic captures cannot exceed
the declared incident count. `consent_attested` must be exactly `1`, set only after the participant
has agreed to local BlackBox collection and use of the privacy-reduced session in this engineering
campaign. Missing, `0`, or noncanonical values invalidate the direct-v1 corpus; there is no older
header reader or conversion path. This is an operator attestation, not cryptographic proof of consent.

A qualifying V0.15.1 corpus needs at least three fully qualified coarse hardware profiles. Each of
those same profiles must contribute a calibration natural session, a held-out natural session, at
least one quiet hour in each split, and scorable truth in each split. The corpus also needs at least
six natural sessions and ten aggregate quiet hours. This prevents development-only or quiet-only
machines from making a single-machine diagnostic evaluation appear multi-hardware.

`incidents.tsv` contains:

```text
incident_key  session_id  split  symptom  certainty  user_visible  expected_diagnosis  expected_contributor_ordinal  expected_context  recurrence_family  detector_should_capture  usefulness  annotator_count  disagreement
```

The 32-character `incident_key` is the archive's random export key. Contributor truth uses the
incident-local process ordinal shown by `blackbox_dogfood_tool inspect`; it never stores a PID,
creation token, process name, or executable path in the corpus. A blank ordinal means contributor
truth is unavailable. Corpus size is capped at 10,000 incidents.

`annotations.tsv` contains the independent source ballots behind each consensus truth row:

```text
incident_key  annotator_id  symptom  certainty  user_visible  expected_diagnosis  expected_contributor_ordinal  expected_context  recurrence_family  usefulness
```

Every `annotator_id` is a campaign-local pseudonym. The `(incident_key, annotator_id)` pair must be
unique, and an annotator cannot equal the linked session's `operator_id`. The number of ballots must
exactly equal `annotator_count`. If every ballot agrees with the consensus truth fields,
`disagreement` is `0`; otherwise it must be `1`, and that row remains visible but is excluded from
primary scoring. Calibration and held-out scorable truth requires at least two distinct ballots.
The direct protocol-v1 reader accepts only this current five-file contract; there is no pre-release
legacy reader or migration path.

## Fixed vocabularies

Splits are `development`, `calibration`, and `held_out`.

Symptoms are `cpu_starvation`, `disk_stall`, `network_interruption`, `application_crash`,
`application_hang`, `game_stutter`, `audio_interruption`, `quiet`, and `ambiguous`.

Truth certainty is:

- `confirmed`: directly measured or mechanically imposed;
- `probable`: strong corroboration with a meaningful alternative still possible;
- `uncertain`: insufficient evidence for scoring;
- `unresolvable`: the retained evidence cannot distinguish the candidates.

Only confirmed/probable rows without annotator disagreement enter primary accuracy or calibration.
Uncertain, unresolvable, and disputed rows remain visible in coverage counts.

Expected diagnoses are the claims the current analyzer can actually emit: `unknown`,
`cpu_pressure`, `memory_pressure`, `storage_pressure`, `network_pressure`, and
`multi_resource_pressure`, `application_crash`, `application_hang`, `dns_resolution_timeout`,
`display_driver_recovery`, and `storage_io_retry`. The final five symptom explanations require their
matching normalized Windows event and retain only the bounded event fact; resource activity alone
cannot produce them and none asserts an underlying root cause. A game stutter or audio
interruption correctly has expected diagnosis `unknown` because V1 has no bounded trustworthy
OS-wide frame-pacing or audio-glitch source. Symptom and diagnosis are deliberately different
fields.

Usefulness is `unscored`, `not_useful`, `unsure`, or `useful`. Usefulness is a human judgment about
whether the explanation helped; it is not cause truth.

## Collection

The Windows-only development harness records the real provider, normalizer, automatic detector,
bounded event stream, recorder, immutable incident construction, and schema-v1 archive:

```powershell
blackbox_dogfood_capture.exe <archive.sqlite3> 60 3 5 <workload.exe> [arguments]
```

It records at least 60 seconds of baseline, optionally launches the workload three seconds before
the marker, and retains the configured post-window. It prints the incident key, trigger counts,
sample counts, and all failure/drop counters. A capture with a provider failure or drop is not
eligible for quality scoring.

Keep one direct schema-v1 archive per coarse hardware profile. Evaluation uses a local
`archive-map.tsv` rather than merging databases:

```text
hardware_profile_id  archive_path
profile-a             captures/profile-a.sqlite3
profile-b             captures/profile-b.sqlite3
profile-c             captures/profile-c.sqlite3
```

The real file uses tabs. Paths may be relative to the map. Every represented profile must appear
exactly once, every path must be an existing regular file, one physical archive cannot stand in for
multiple profiles, duplicate incident keys are rejected, and each truth-linked incident must reside
in the archive matching its session profile. The path map is local coordination state and is never
fingerprinted or published.

`blackbox_dogfood_workload.exe` supplies controlled `cpu`, `disk`, `network`, `hang`,
`game-stutter`, and `audio-glitch` development scenarios. The game case is explicitly a visible
Win32 frame-stall surrogate, not a real-game result. The hang workload deliberately stops
dispatching its visible window. The audio case starves a low-volume test tone between two playback
segments; it is an audible playback-gap surrogate, not evidence about a device or driver. The
network workload uses local abortive TCP closes and therefore tests reset evidence, not WAN
latency. The project does not automate audio-device disruption, real adapter changes, or a user's
game; those require consented natural sessions and manual truth annotation.

Truth collection and prediction inspection are deliberately separate:

- `blackbox_dogfood_tool list-truth <archive>` lists privacy-safe incident export keys, UTC times,
  and row counts without trigger, label, feedback, analysis, or contributor output. Use it to locate
  the incident recorded for a predeclared session.
- `blackbox_dogfood_tool inspect-truth <archive> <incident-key>` opens the archive read-only and
  prints only the stored window/sample counts and incident-local process ordinals. It does not run
  or reveal BlackBox diagnosis, resource, context, or contributor ranking. Use this command for
  calibration and held-out annotation.
- `blackbox_dogfood_tool export-truth <archive> <incident-key> <new-review-directory>
  ordinal-only` publishes a self-contained, prediction-free review directory for an annotator. Its
  `review.html` plots the stored system and per-process time series around the event marker and
  lists normalized system events. The accompanying TSV files contain the same raw normalized
  evidence, stable incident-local process ordinals, and a blank independent ballot template. The
  output contains no diagnosis, confidence, contributor ranking, label, feedback, or analyzer
  result, and exporting never invokes analysis.
- `blackbox_dogfood_tool inspect <archive> <incident-key>` additionally prints the current analysis
  and is development-only. Never show this output to calibration or held-out annotators before their
  ballots and consensus truth are fixed.

An ordinal-only truth review contains exactly `manifest.ini`, `system-samples.tsv`, `processes.tsv`,
`process-samples.tsv`, `system-events.tsv`, `ballot-template.tsv`, and `review.html`. The exporter
rejects more than 20,000 system samples, 250,000 process samples, 10,000 process identities, or
65,536 system events before creating output. It stages at `<new-review-directory>.partial`, refuses
an existing final or staging destination, verifies the exact nonempty file set, and publishes with
one sibling rename. A validation error creates neither directory; an I/O failure leaves a visibly
partial artifact for investigation.

Process names and PIDs shown by console inspection remain local and are never copied into the
corpus. The only way to include them in a review directory is the explicit
`include-local-identities` privacy mode. That mode includes the bounded recorded process name and
PID for local disambiguation, still excludes paths and creation tokens, and produces a sensitive
local artifact that must not be added to the corpus or shared by default. The privacy mode is a
mandatory command argument so an operator cannot opt in accidentally. All truth inspection and
export paths use the storage read-only mode: they require an existing regular schema-v1 archive,
cannot initialize a database, and cannot write annotations, profiles, or incidents.

CLI dispatch also keeps prediction state out of this path structurally. `list-truth`,
`inspect-truth`, `export-truth`, validation/readiness/freeze/merge/status, and campaign-status
commands do not construct the intelligent analyzer. Only explicit pipeline identity/initialization,
development inspection, and evaluation paths can construct it. The cross-graph
`prediction_free_dogfood_cli_contract` enforces that command boundary.

Open `review.html` locally, inspect the evidence without prediction-bearing material present, and
record each annotator's decision outside the artifact. Copy only the independently fixed ballot
fields into a session packet. The supplied blank template is not a consensus generator and the
review directory is acquisition working state, not one of the corpus's five files. Before relying
on a newly built binary for collection, manually exercise both selectors and both charts in the
local browser used by the campaign; automated tests verify the generated document contract but do
not replace that visual check.

Validate each completed ballot before the coordinator copies it into a packet:

```powershell
blackbox_dogfood_tool validate-ballot <review>/ballot-template.tsv `
  <expected-incident-key> <session-operator-id>
```

This accepts only a non-link regular file up to 4 KiB and reads exactly one protocol-v1 annotation
row through the same canonical parser used by corpus loading. It rejects a wrong incident key,
malformed or extra rows, unsafe values, out-of-range
ordinals, and an annotator matching the session operator. Successful output deliberately omits
symptom, diagnosis, contributor, context, recurrence, and usefulness fields. The command validates
one independently fixed ballot; it neither compares ballots nor generates the consensus row.

After both independent handoffs, run:

```powershell
blackbox_dogfood_tool compare-ballots <ballot-a.tsv> <ballot-b.tsv> `
  <expected-incident-key> <session-operator-id>
```

The command reloads both files through the same bounded validator, rejects repeated annotator
pseudonyms, and emits only their bindings, `annotator_count=2`, and the exact payload disagreement
bit. It does not expose either payload, decide correctness, or write consensus. Copy the reported
count/disagreement values only after the coordinator has separately fixed the consensus row.

### Transaction-safe session acquisition

Do not append directly to the active campaign's four TSV tables. A corpus requires exact cross-file
session, incident, and ballot counts, so an interrupted hand edit otherwise leaves the entire
campaign temporarily invalid. Instead, prepare one complete session packet and publish a new corpus:

For a completed consented quiet exposure of at least one hour with zero automatic captures,
`scripts/new-consented-quiet-session-packet.ps1` creates and validates that packet under a sibling
`.partial` directory before publishing it atomically. It requires exact post-fact consent, completed-
exposure, and no-capture attestation tokens and invokes only prediction-free initialization and
validation. It does not observe the exposure or prove those attestations. Any incident-bearing
session must use the full archive-backed, independently annotated workflow below.

For a completed consented natural session containing exactly one incident and two independently
completed ballots, `scripts/new-consented-incident-session-packet.ps1` removes the remaining
cross-table hand-edit step. Supply the hardware/session fields, incident consensus truth, the two
ballot files, the real schema-v1 archive, and the exact consent/session/consensus attestation tokens.
The helper uses only `validate-ballot`, `compare-ballots`, `init-session`, `validate`, and
`merge-session`; it never constructs or invokes the analyzer. It derives `annotator_count=2` and the
disagreement bit, copies each validated ballot exactly once, then performs a disposable read-only
merge to prove the incident key and automatic-capture count against the archive. It hashes the main
archive before and after proof, leaves the base corpus byte-identical, removes its proof corpus, and
publishes the five-file packet through a sibling rename only after validation. Wrong archive
provenance, operator-authored/repeated ballots, mismatched consensus, false attestation tokens, and
occupied/reserved destinations are rejected. These checks validate records and attestations; they
do not prove that consent occurred, that the session was observed, or that the coordinator chose the
correct consensus.

Use PowerShell parameter discovery for the complete bounded field list:

```powershell
Get-Help scripts/new-consented-incident-session-packet.ps1 -Detailed
```

The required attestation values are `PARTICIPANT-CONSENT-CONFIRMED`,
`INCIDENT-SESSION-COMPLETED`, and `COORDINATOR-CONSENSUS-FIXED`. Calibration and held-out truth must
still be prepared by the designated coordinator without prediction inspection.

```powershell
# The base corpus supplies the exact corpus/pipeline/configuration identity.
blackbox_dogfood_tool init-session corpus-000 session-packet-001

# For multi-incident sessions, fill the packet's one hardware row, one session row, all incident
# truth rows, and every independent annotation ballot. Then verify it in isolation. Prefer the
# incident-session helper above for its supported one-incident/two-ballot case.
blackbox_dogfood_tool validate session-packet-001

# An incident-bearing packet must be proven against its profile's real archive.
blackbox_dogfood_tool merge-session corpus-000 session-packet-001 `
  captures/profile-a.sqlite3 corpus-001

# A genuinely zero-incident quiet session has no archive keys to prove.
blackbox_dogfood_tool merge-session corpus-001 quiet-packet-002 none corpus-002
```

A packet is the same direct protocol-v1 five-file layout, but import requires exactly one hardware
profile and one complete session. Repeating an existing profile is allowed only when all coarse
fields are identical. Every packet incident key must exist in the supplied regular schema-v1
archive, and the number of stored incidents with automatic trigger provenance must exactly equal
the session's `automatic_captures`. The archive is opened read-only and its main database bytes are
not modified.

Merge never edits the base or packet. It builds and reload-verifies a complete candidate under
`<new-corpus>.partial`, refuses existing final/staging destinations, and publishes through one
same-volume directory rename. A failed write remains visibly partial; validation/provenance failures
create no output. Use the successfully published corpus as the next base and retain earlier bases and
packets as acquisition provenance. Packets may not contain notes, manifests, exports, or any sixth
file.

Validation failures for session incident totals, ballot totals, disagreement flags, repeated
ballots, repeated session/incident IDs, and collection-operator conflicts identify the exact
pseudonymous row and declared/observed numeric mismatch. Messages never print ballot payloads,
process identity, archive paths, or analyzer output. Fix the draft packet and validate it again;
never hand-edit a corpus already published by `merge-session`.

For held-out sessions, a designated truth coordinator should build the packet from independently
fixed ballots using `inspect-truth` only. The analyzer developer must not inspect held-out prediction
output, choose rows based on predictions, or change code/configuration after seeing held-out truth.

Run `blackbox_dogfood_tool readiness <corpus>` throughout collection. It prints exact hardware,
natural-session, split-specific quiet-hour, truth, and independent-annotation counts plus every
unmet freeze requirement. It exits with code 3 until the corpus is ready. `validate` reports the
same status but returns success for a structurally valid collecting corpus.

For a reviewable operator surface, run
`blackbox_dogfood_tool campaign-status <corpus> <new-status-directory>`. The command atomically
publishes one exact schema-v1 directory containing a manifest, summary/profile/symptom/unmet TSVs,
and self-contained `status.html`. It uses only validated corpus declarations and the qualification
report, invokes no diagnosis, and declares itself prediction-free and evidence-neutral. It cannot
substitute for archives, ballots, natural sessions, freeze, or held-out evaluation.

## Freeze and split discipline

Development data may be inspected freely. Calibration data may select the confidence map and
assertion threshold. Held-out truth must not influence code, configuration, thresholds, corpus row
selection, or calibration.

`blackbox_dogfood_tool freeze <corpus> <v015-incidents.tsv>` refuses unless:

- scorable calibration and held-out truth each cover all nine symptom classes;
- all session/profile references and split assignments are exact;
- at least ten scorable calibration rows have a supported expected diagnosis;
- at least ten scorable held-out truth rows exist;
- at least three of the same hardware profiles contribute natural, quiet, and scorable evidence to
  both frozen splits, at least six sessions are natural, and quiet exposure totals ten hours;
- every truth count and disagreement flag is proven by distinct non-operator annotation ballots;
- no candidate incident key appears in the prior V0.15 held-out split;
- all identifiers, counts, values, and hard bounds are valid.

Freeze fingerprints every annotation and coarse hardware/session field. Later edits make the corpus
invalid. After all held-out inputs load successfully but before analysis, the evaluator exclusively
creates the `heldout-evaluation.lock` directory. `attempt.ini` fingerprints the corpus/configuration
and calibration artifact; `result.ini` is added only after atomically publishing a complete report
and records its content fingerprint and qualification outcome. A crash remains visibly `running`, a
quality failure remains complete and locked, and concurrent/repeated runs are refused before
analysis. Use `blackbox_dogfood_tool heldout-status <corpus>` to inspect the state. This is a
procedural guard, not tamper-proof security; published work retains the frozen corpus, calibration
artifact, report, binary version, and hashes together.

## Current local corpus status

The workspace corpus under `out/dogfood-v015/` is frozen evidence and is intentionally not part of
source or a release package. It contains 32 sessions/incidents across development, calibration, and
one-shot held-out splits. The checked-in aggregate results, hardware distribution, negative
findings, and limitations are in [`V015_DOGFOOD_RESULTS.md`](V015_DOGFOOD_RESULTS.md). The local
archive/corpus/results plus their matching historical evaluator remain required to reproduce that
exact report. The current prerelease evaluator intentionally rejects its older session header
because it lacks mandatory consent attestation; no compatibility reader or conversion is shipped.
