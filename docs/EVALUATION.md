# Diagnostic evaluation and offline ML

[Documentation index](README.md) · [Current candidate](CURRENT_CANDIDATE.md)

- [V0.15.1 local dogfood protocol](#v0151-local-dogfood-protocol)
- [Diagnostic evaluation and calibration](#diagnostic-evaluation-and-calibration)
- [Offline model research harness](#offline-model-research-harness)

## V0.15.1 local dogfood protocol

### Purpose and evidence level

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

### Current qualification state

The acquisition, validation, freeze, calibration, and one-shot evaluation machinery is implemented.
The qualifying multi-machine evidence collection and one-shot held-out result are not complete.
Runtime inference therefore remains the local statistical baseline; native ML has not been adopted.

The qualifying corpus must cover all nine current symptom classes: CPU starvation, disk stall,
network interruption, application crash, application hang, game stutter, audio interruption,
quiet, and ambiguous evidence. At least three coarse hardware profiles must each contribute natural,
quiet, and scorable evidence to both frozen splits. Two independent non-operator ballots support
each truth row, and total quiet exposure must reach ten hours. These live requirements replace dated
pre-release campaign/status snapshots; Git history retains those implementation-era reports.

### Directory contract

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

### Fixed vocabularies

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

### Collection

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

#### Transaction-safe session acquisition

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

### Freeze and split discipline

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

### Development corpus boundary

Workspace corpora under `out/` are local engineering evidence and are intentionally excluded from
source and release packages. Earlier development corpora do not satisfy the current direct-V1
consent, independent-annotation, analyzer-fingerprint, or hardware-distribution gates. No legacy
reader or conversion path is shipped; qualifying evidence must be collected in the current format.

## Diagnostic evaluation and calibration

### Predeclared primary metrics

The protocol fixes metric names before corpus freeze. Every rate is published with its numerator,
denominator, symptom counts, hardware-profile count, missing predictions, and excluded uncertain or
disputed rows.

| Metric | Eligible rows / denominator | Success or value |
|---|---|---|
| Supported-diagnosis recall | Confirmed/probable, non-disputed truth with a supported non-Unknown diagnosis | Exact diagnosis type; a missing prediction is a failure |
| Supported-diagnosis precision | Every emitted non-Unknown explanation on scorable truth | Exact supported diagnosis type |
| Unknown-truth abstention | Confirmed/probable truth whose defensible diagnosis is Unknown | Analyzer emits Unknown; a missing prediction is a failure |
| False-assertion rate | Same Unknown-truth rows | `1 - abstention rate` |
| Top-1 contributor | Rows with an incident-local contributor ordinal | First predicted ordinal matches; a missing prediction is a failure |
| Top-3 contributor | Rows with a contributor ordinal | Ordinal appears in first three predictions; a missing prediction is a failure |
| Unknown rate | All scorable truth rows | Fraction with an explicit Unknown prediction; a missing prediction is not credited as abstention |
| Miss rate | Rows marked `detector_should_capture=1` | `1 - automatic detection recall`; a missing prediction is a miss |
| False captures/hour | Quiet session exposure hours | Quiet automatic capture count / hours |
| Context accuracy | Rows with a non-Unknown expected workload | Exact workload context |
| Recurrence pair F1 | Truth-family and predicted-cluster pairs | Pair precision/recall harmonic mean |
| Usefulness | Human `useful` or `not_useful` ratings | Useful fraction; `unsure` is reported but unscored |
| Brier score | Every emitted assertion on scorable known or Unknown truth | Mean squared confidence error |
| ECE | Same assertions, ten fixed `[0,.1)...[.9,1]` bins | Weighted confidence/accuracy gap |

Expected Unknown rows are included as incorrect confidence outcomes whenever the analyzer makes a
resource assertion. This prevents quiet or fundamentally ambiguous false diagnoses from disappearing
out of calibration statistics.

The direct-V1 JSON publishes all nine symptom counts, the coarse hardware distribution, missing and
excluded rows, every rate numerator/denominator, quiet automatic-capture count and exposure hours,
all ten calibration bins, and recurrence-pair counts. Missing predictions cannot reduce a truth-based
denominator. Precision, Brier score, and ECE remain assertion-based and therefore publish their
emitted-assertion denominator separately.

### Component evaluation

`blackbox_dogfood_tool evaluate` loads immutable incidents from the local archive and evaluates the
same versioned components used by the viewer:

- detector outcome from recorded manual/automatic trigger provenance;
- workload context from the analyzer's complete probability assessment;
- personalized history from prior archive observations only;
- contributor ordering mapped to privacy-safe incident-local ordinals;
- recurrence from bounded version-2 feature extraction and complete-link clustering;
- final diagnosis and evidence confidence from pipeline version 5.

The tool is a separate executable linked to `BlackBox::Evaluation`, `BlackBox::Analysis`, and
`BlackBox::Storage`. The desktop executable does not link `BlackBox::Evaluation`; telemetry, the
recorder, and storage schema do not depend on it.

Calibration and held-out truth annotation must use `inspect-truth` or an ordinal-only
`export-truth` review, never the prediction-bearing `inspect` command. `export-truth` receives only
an immutable incident plus archive metadata and publishes raw normalized time series, system
events, local process ordinals, a blank ballot, and a self-contained local HTML viewer. It never
constructs or invokes an analyzer. The default artifact has blank PID/name columns; the explicit
`include-local-identities` mode is sensitive local working state and must not enter the corpus.
Both archive inspection and evaluation use explicit read-only storage access, so offline scoring
cannot initialize, annotate, profile, or append to retained evidence. Read-only access still
validates the direct schema-v1 marker and fails on missing, linked, or incompatible archives.

### Calibration-only fitting

Run calibration only after corpus freeze:

```powershell
blackbox_dogfood_tool evaluate <archive-map.tsv> <corpus> calibration none <new-output-directory>
```

At least ten eligible emitted assertions are required. The tool fits bounded monotonic isotonic
calibration using pair-adjacent-violators and at most 32 knots. It then selects the widest-coverage
assertion threshold whose observed calibration precision is at least 80%. If no threshold reaches
that predeclared precision, resource assertions are disabled rather than lowering the requirement.

The resulting `calibration.tsv` records corpus annotation fingerprint, analyzer configuration
fingerprint, source sample count, knots, selected threshold, coverage, and observed precision. It
cannot be loaded with another corpus or analyzer configuration. It has exactly one direct-V1 byte
representation: ordered fields, LF termination, round-trip-exact floating-point text, no blank
lines, at most 32 monotonic knots, a 64 KiB file bound, and a 4 KiB line bound. Linked or
non-regular inputs, inconsistent sample totals/assertion state, CRLF, reordered fields, and alternate
numeric spellings are rejected rather than normalized. New output refuses both occupied final and
sibling `.partial` paths, uses a same-directory rename, and is reloaded before success. There is no
older or permissive calibration reader. The CLI reloads and exactly compares `calibration.tsv` once
more after the complete evaluation directory reaches its final name.

Freeze is allowed only after `blackbox_dogfood_tool readiness` proves that the same three or more
hardware profiles contribute natural sessions, quiet exposure, and scorable truth to both
calibration and held-out splits. Independent truth is backed by two distinct non-operator ballots,
not a manually entered annotator count. Both splits must cover all nine symptom classes, ensuring
published denominators cannot be assembled from a single machine or development-only rows.

### One-shot held-out evaluation

```powershell
blackbox_dogfood_tool evaluate <archive-map.tsv> <corpus> held_out `
  <calibration-output>/calibration.tsv <new-heldout-output-directory>
```

Every held-out truth key must exist in its mapped archive. After archive provenance and immutable
snapshot loading succeed—but before clustering or diagnosis—the tool atomically creates the
exclusive `heldout-evaluation.lock` directory. Its immutable `attempt.ini` binds the annotation,
configuration, and exact calibration-artifact fingerprints. A concurrent or repeated command is
refused before analysis, so it cannot silently consume the split twice.

Calibration and held-out files are written into a sibling `<output>.partial` directory. The tool
requires every expected nonempty bounded artifact and atomically renames the directory to its final
name; the output parent must already exist, and either an existing final or partial directory blocks
the command. A write failure never makes a partial directory look complete. A completed held-out result
adds `result.ini` to the lock with qualification outcome and a content fingerprint over
`evaluation.json` and privacy-safe `predictions.tsv`. Both a passing and a complete failing quality
result consume the one-shot attempt.

Before publication, the offline evaluator parses its own canonical `predictions.tsv`, recomputes
the complete report from the frozen corpus without invoking the analyzer or opening an archive, and
requires byte-for-byte agreement with the direct-V1 `evaluation.json`. After the atomic rename it
repeats that verification against the final directory and rechecks the two-file fingerprint before
`result.ini` can mark the one-shot attempt complete. A mismatch leaves the attempt visibly running.
The report binds the exact calibration-artifact fingerprint as well as its assertion state and
threshold.

The same verifier can be run later without consuming another held-out attempt:

```powershell
blackbox_dogfood_tool verify-evaluation <frozen-corpus> <evaluation-output> `
  <calibration.tsv|none>
```

It strictly parses bounded canonical rows, rejects links, oversized rows/files, duplicate contributor
ordinals, excess prediction rows, wrong corpus or calibration provenance, recomputed metric changes,
and noncanonical or tampered output. The supplied calibration goes through the same canonical,
bounded, non-link direct-V1 loader and is fingerprinted so changing
only the declared threshold or calibration fingerprint cannot make a different artifact verify.

If the evaluator crashes or encounters an operational failure after acquisition, the lock remains
in the `running` state and partial output remains visibly suffixed. It is not automatically deleted
or reset. `blackbox_dogfood_tool heldout-status <corpus>` distinguishes `not_started`, `running`, and
`complete`; a running attempt requires documented investigation rather than an unrecorded rerun.

`archive-map.tsv` is a local two-column table with the exact header
`hardware_profile_id<TAB>archive_path`. It contains one unique archive for every hardware profile
represented by corpus sessions. Relative paths resolve beside the map. Evaluation opens all mapped
archives, requires existing regular files, refuses shared physical files and duplicate incident
keys, and verifies each truth-linked incident came from the profile declared by its session. Archive
paths stay local and are not added to the frozen corpus or evaluation output.

Each prediction row keeps the inferred symptom explanation separate from `observed_pressure`, its
practical pressure score, and the raw statistical score. An `unknown` explanation may therefore
coexist with strong observed pressure without presenting correlation as cause.

V0.15.1 passes only when supported-diagnosis precision is at least 80%, supported-diagnosis recall
(`supported_diagnosis_recall`) is at least 60%, Unknown-truth abstention is at least 90%, and top-3
contributor accuracy is at least 70%. Every denominator must be nonzero. The JSON records
`qualification_passed`; a complete failing evaluation is still written and one-shot locked, and
the command exits with code 3.

### Publication limits

No aggregate result is a production claim unless the report includes:

- separate controlled and natural-session results;
- counts for all nine symptom classes and Unknown truth;
- OS/build, CPU-count, memory, GPU-family, and power-mode distribution buckets;
- the frozen corpus and analysis/configuration fingerprints;
- all missing predictions, truth uncertainty, annotator disagreement, and confidence-bin counts;
- false-capture exposure hours and whether sessions were actively or quietly used;
- explicit explanation that contributor correlation is not causal proof.

One local host, controlled surrogates, fewer than the protocol minimums, or an unsigned developer
binary cannot establish V1 diagnostic accuracy. Real game, audio, WAN/VPN, varied hardware, and
natural ambiguous incidents remain required even when controlled mechanics are perfect. Native ML
remains outside the runtime until a separately frozen comparison proves material held-out benefit.

Earlier development runs were deliberately narrow and do not qualify the current analyzer. Their
reports remain in Git history rather than in the living product documentation.

## Offline model research harness

BlackBox does not ship a machine-learning model or runtime. The offline harness makes future model
experiments reproducible while preserving the recorder-first architecture and the frozen-corpus
rules. It is a development target and is not installed in the release package.

### Label-free feature export

Build `blackbox_offline_ml_tool`, then export one new matrix from an existing direct-v1 archive:

```text
blackbox_offline_ml_tool export-features <archive.sqlite3> <new-features.tsv>
```

The archive is opened read-only. The tool reuses `analysis::extract_incident_features`, so every row
contains the current version-2, 16-dimensional incident-shape vector used by recurrence analysis:
CPU, memory, disk, network, disk-quality, and network-quality peak/near-marker shapes; dominant
pre/post-marker shares; duration; and dominant-resource concentration. Available values are bounded
to `[0,1]`; unavailable values are the literal `NA`.

Direct feature-matrix format version 1 is LF-terminated, field ordered, sorted by the 32-character
lowercase incident export key, capped at 10,000 rows, written to a sibling `.partial`, renamed on the
same volume, and reread byte-for-byte after publication. It deliberately excludes:

- truth labels, diagnoses, confidence, feedback, and annotations;
- local database IDs and wall-clock timestamps;
- process identities, executable names/paths, free-form notes, and raw telemetry.

Training and experiment metadata belong in separate research tooling. Joining labels must follow the
frozen corpus split: development/calibration data may be used for fitting; the held-out split remains
one-shot evaluation evidence and must not be inspected to tune a candidate.

### Baseline comparison

Compare a candidate evaluation with the current statistical baseline:

```text
blackbox_offline_ml_tool compare <frozen-corpus-directory> \
  <baseline-evaluation-directory> <candidate-evaluation-directory>
```

Both directories must first pass the existing canonical evaluation verifier, which reparses their
prediction rows, recomputes every metric from the same frozen corpus, and requires exact direct-v1
artifact bytes. The comparison also requires the same split, truth population, and missing-prediction
count. Its default non-inferiority policy permits at most 0.02 regression in supported precision,
supported recall, Unknown-truth abstention, or top-three contributor accuracy, and at most 0.02
increase in false-assertion rate, Brier score, or expected calibration error. Exit code `3` means the
candidate regressed; it is not a product-release pass.

A future adoption proposal must separately show a material reproducible improvement, bounded native
latency and memory, subgroup behavior, calibration, abstention safety, and no dependency from
collection into analysis. Until representative held-out evidence clears that gate, runtime ML and
ONNX Runtime remain absent.
