# Analysis reference

[Documentation index](README.md) · [Current candidate](CURRENT_CANDIDATE.md)

- [V0.2 statistical incident analysis](#v02-statistical-incident-analysis)
- [Intelligent analysis pipeline](#intelligent-analysis-pipeline)
- [V0.3 personalized process baselines](#v03-personalized-process-baselines)
- [V0.6 contributor ranking](#v06-contributor-ranking-1)
- [Recurring incident discovery](#recurring-incident-discovery)
- [V0.8 probabilistic workload context](#v08-probabilistic-workload-context)

## V0.2 statistical incident analysis

BlackBox V0.2 ranks what was unusual inside a captured incident. It does not claim root cause and
does not use machine learning. `analysis::IIncidentAnalyzer` accepts only an immutable
`core::IncidentSnapshot` and returns ranked evidence plus uncertainty. The analysis target depends
only on core; telemetry collection, normalization, recording, capture, and storage do not depend on
it.

### Incident-local windows

For an event at `T`, the default policy is:

```text
baseline:   T - 90 s through T - 30 s (exclusive end)
evaluation: T - 30 s through the incident's actual end
```

The baseline is clipped to available incident history. Each metric retains at most 256 recent
baseline values and requires eight available observations. Fewer observations produce explicit
`insufficient_baseline`; no score is invented. Missing/unsupported/inaccessible values are counted
and excluded rather than converted to zero.

These incident-local baselines remain the fallback in V0.3. When enough executable history exists,
personalized process evidence replaces the corresponding incident-local process metric score. See
`ANALYSIS.md` for identity, aging, persistence, and cold-start policy.

### Robust statistics and score

For each metric, BlackBox reports minimum, P05, P25, median, P75, P95, maximum, median absolute
deviation (MAD), and the empirical percentile of the strongest evaluated observation. Its scale is:

```text
scale = max(1.4826 * MAD, IQR / 1.349, abs(median) * 0.01, 1e-9)
robust_z = (observation - median) / scale
```

The IQR and relative floors make flat/tiny baselines deterministic without division by zero. The
bounded anomaly score is:

```text
score = 0                                      when abs(robust_z) <= 3.5
score = 1 - exp(-(abs(robust_z) - 3.5) / 3)  otherwise
```

Direction (`higher`/`lower`), raw observation, baseline median/P95, robust z, percentile, sample
counts, and missing counts remain available as evidence. CPU and memory are fractions; memory-size
and throughput evidence retain bytes and bytes/second internally. The UI converts units only for
display.

### Ranking

System resources are CPU, memory, disk (read/write throughput plus physical read/write latency,
service time, and queue depth), and network (receive/transmit plus connectivity disruption,
interface transitions, TCP retransmission, failed attempts, and resets). Connectivity enums are
mapped to an explicit disruption severity before comparison; an unknown hint is weak evidence and
not equivalent to a disconnection. A resource's
score is its strongest metric score. Ties use stable enum/identity ordering, making fixed incidents
bit-for-bit deterministic.

Process ranking keys on full `(PID, creation token)` identity. To bound work on the 600,000-row
maximum incident, the analyzer first selects at most 512 evaluation-window identities: the highest
absolute consumers from each of CPU, working set, disk read, and disk write, then deterministic
identity fill. It computes robust baselines for those candidates, returns at most 100, and the UI
shows 20. This preselection can miss a low-absolute process whose relative change is large; that is
a documented V0.2 tradeoff, not evidence that the process was normal.

Confidence describes evidence coverage, not causal certainty:

| Confidence | Meaning |
|---|---|
| High | At least 30 baseline and 3 evaluation observations for the strongest evidence |
| Moderate | The configured minimum baseline is met, but high-coverage thresholds are not |
| Low / cold start | At least one metric lacks the minimum baseline |
| Unavailable | No evaluable value exists |

If every system resource lacks a usable baseline, the whole result is marked cold start. Analysis
failure affects only the viewer result; the incident remains viewable and the recorder continues.

### Optional boundary

Configure with `-DBLACKBOX_ENABLE_ANALYSIS=OFF` to omit the analysis target and tests. The viewer
then states that analysis is disabled. CI's headless collection graph disables application, SQLite,
UI, and analysis together. Analysis is recomputed on the viewer worker when an incident is loaded;
V0.2 does not alter the archive schema or write scores continuously. V0.3 adds bounded profile
observations that are queried and updated only on the viewer worker when an incident is opened;
normal recording remains RAM-only.

Scores mean “unusual relative to this incident's baseline,” never “proved cause.” A high score can
reflect an expected phase change, and a zero score can reflect missing/cold data or a consistently
high workload. The UI shows evidence and confidence beside every ranking and repeats this caveat.

### V0.6 contributor ranking

The analyzer now derives at most 20 contributor candidates after statistical and personalized
process scoring. The score combines anomaly magnitude, marker-relative timing, same-direction
system-resource evidence, anomalous duration, prior recurrence, and explicit evidence coverage.
Anomalous sample balance now separates genuinely preceding activity, marker-spanning ambiguity,
and wholly post-marker possible victims/reactions. Only the first can receive the `likely` label.
Missing process metrics reduce score and confidence. Exact weights, thresholds, bounds, measured
latency, and limitations are documented in `ANALYSIS.md`.

V0.16 can apply bounded explicit contributor-attribution history after incident-local ranking.
This downstream value-only calibration preserves the original score, never creates a candidate or
promotes marker-spanning or post-marker activity, accepts positive learning only from a prior
preceding row, and cannot use noticed-problem feedback as causal evidence. See
`ANALYSIS.md` for consensus, reset, poisoning, and inspection rules.

### V0.7 recurring incident discovery

The optional analysis module also extracts versioned, scaled system-shape vectors and groups at
most 512 archived incidents on the viewer worker. Automatic complete-link groups require at least
two members within a fixed distance threshold; unmatched incidents remain explicit noise. Generic
storage cache records and manual group labels are mapped only by the application composition
boundary. Exact dimensions, missingness distance, cache invalidation, override semantics,
benchmarks, and limitations are documented in `ANALYSIS.md`.

## Intelligent analysis pipeline

Version 0.9 composes the existing incident-local anomaly, personalized executable,
workload-context, recurring-pattern, and contributor components into one coherent local
diagnosis. It does not change collection, normalization, recording, capture, or the archive
schema.

### Boundary and data flow

`IntelligentIncidentAnalyzer` implements the portable `IIncidentAnalyzer` interface and consumes
an immutable `IncidentSnapshot` plus optional caller-supplied portable context. The application
maps process-profile and recurring-group data into that context on the viewer worker. Analysis has
no storage, UI, platform, thread, or I/O dependency and remains completely removable from the
build. Collection never calls analysis.

The result records:

- pipeline version `13`, evidence-model version `12`, and stable configuration fingerprint
  `6701770989141957614`;
- observed CPU, memory, storage, or network pressure only when both robust deviation and a
  unit-specific practical-effect floor are cleared;
- a separate CPU, memory, storage, network, or multi-resource symptom explanation only when
  pressure aligns with an independent capture, resource-quality, or preceding-contributor signal;
- an exact Windows-reported application-crash, application-hang, DNS-resolution-timeout,
  display-timeout-recovery, or storage-I/O-retry symptom only when its
  canonical normalized event is aligned with the marker;
- the highest ranked preceding contributor correlated with the selected resource;
- calibrated numeric confidence, its categorical label, evidence coverage, and the explicit
  correlated-evidence penalty; and
- at most eight typed links back to resource, process, contributor, workload, recurrence,
  automatic-trigger, or system-event evidence already present in the incident/result.

Quiet or insufficiently aligned incidents remain `unknown`; the pipeline does not promote nearby
resource activity into a symptom cause. A strong raw statistical deviation may remain visible with
a zero practical-pressure score. Link validation rejects non-finite scores, out-of-range indices,
unavailable trigger/event/recurrence references, user-created recurrence as evidence, and resource
explanations without recorded practical pressure. Event-aligned explanations must cite
an in-range normalized system event, automatic explanations must cite the recorded trigger, and
statistical explanations must cite a resource anomaly. The viewer shows observed pressure and
symptom explanation as separate sections and labels contributors as correlations rather than
proven causes.

Contributor timing keeps two concepts separate. The first anomalous process sample remains the
score-bearing activity onset. Optional exact `(PID, creation token)` lifecycle events may add
recorded process-start/exit offsets after source/kind/window/order validation, but cannot change a
rank, confidence, strength, evidence link, diagnosis, or feedback-calibration input. Missing
lifecycle context is never interpreted as a long-running process.

### Determinism and calibration

For a fixed incident, portable context, and configuration, ordering, tie-breaking, fingerprints,
scores, evidence links, and the final result are deterministic. The configuration fingerprint is
computed field-by-field rather than from object bytes, so padding and address identity cannot
affect it.

A resource explanation requires a practical pressure score of at least `0.35` plus an aligned
automatic capture, direct disk/network quality metric, or strong preceding contributor. Two
resources become a multi-resource explanation only when both score at least `0.65`, differ by no
more than `0.08`, and both have independent alignment. A Windows Application Hang event within
five seconds of the marker is direct bounded symptom evidence and does not require resource
pressure. A normalized Windows DNS Client event 1014 with the matching network source within five
seconds is likewise direct evidence only for the precise reported timeout symptom. It does not
identify the cause and cannot request automatic capture. Mismatched source/kind/ID combinations
are ignored, and an independently aligned resource diagnosis outranks a coincidental DNS timeout.
A canonical graphics/`display_driver_recovery`/4101 tuple within five seconds is direct evidence
that Windows performed display timeout recovery. It may also be the recorded automatic trigger,
but it identifies neither a driver/application/GPU fault nor any other root cause. This exact
OS-confirmed symptom outranks generic resource interpretations.
A canonical storage/`storage_io_retry`/153 tuple within five seconds is direct evidence only that
Windows reported retrying a timed-out storage I/O request. It can be the disk-scoped automatic
trigger, but does not identify overload, a device, path, controller, driver, media, firmware,
application, hardware failure, or any other root cause. This exact OS-reported symptom outranks
generic resource interpretations.
The bounded confidence composition is:

```text
combined = 0.68 * resource
         + 0.14 * contributor * contributor-confidence-weight
         + 0.12 * evidence-coverage
         + 0.02 * workload-context
         + 0.02 * automatic-recurrence
         + 0.02 * matching-automatic-trigger
         - 0.08 * min(resource, weighted-contributor)

calibrated = clamp01(combined * (0.65 + 0.35 * evidence-coverage))
```

The contributor confidence weight is `0.0/0.5/0.8/1.0` for unavailable/low/moderate/high.
The penalty acknowledges that a process anomaly and its matching system-resource anomaly are
correlated observations. The process link therefore contributes zero additional confidence and is
retained only for inspection. Automatic recurrence is bounded by occurrence count, cluster
cohesion, and shared-feature support; a manual grouping contributes nothing. A high label requires
score and coverage of at least `0.75` and a non-cold-start result; moderate requires score `0.50`
and coverage `0.45`; other supported diagnoses are low confidence.

### Local feedback calibration

Pipeline thirteen can conservatively reduce a future automatic-trigger assertion after repeated user
answers that the same trigger signature did not correspond to a noticed problem. This is
post-capture analysis only. It cannot change detector thresholds, request or cancel capture, alter
resource/process scores, reorder contributors, or mutate the immutable incident.

Eligibility is deliberately narrow: only prior answered automatic incidents with the exact
resource and signal pair are considered. The current incident, later incidents, duplicates,
manual captures, mismatched signatures, and observations older than 90 days are ignored. At most
256 observations enter calibration and at least four exact matches are required. A symmetric
`Beta(1,1)` prior prevents a tiny history from behaving as certainty. Suppression starts only when
the smoothed false-positive share is at least 75%; the ordinary reduction is capped at 55%, and a
hard-validation path remains capped at 75%.

The multiplier applies only to an otherwise available automatic-capture diagnosis. If the adjusted
confidence falls below the configured assertion floor, the diagnosis abstains as `Unknown` while
all observed pressure, process anomalies, contributor evidence, and calibration counts remain
inspectable. Conflicting noticed-problem feedback raises the smoothed denominator and can keep the
profile stable.

The viewer exposes the profile revision, matching counts, smoothed decision, and multiplier. A
confirmed reset advances a direct-v1 cutoff so older answers stop influencing later analysis but
remain stored with their incidents. One rollback restores the previous cutoff. Privacy purge clears
incidents, executable profiles, and the feedback-control state together. These controls are local
and never enter the collector dependency graph.

### Confirmed similar-incident context

Pipeline thirteen also summarizes prior user-confirmed symptoms inside the current incident's
automatic recurrence cluster. This is an inspectable historical note only: it never creates or
changes a diagnosis, confidence value, evidence link, resource/process score, or contributor rank.
User-created recurrence groups are categorically excluded so manual organization cannot teach the
pipeline.

The evaluator considers at most the latest 32 distinct prior members from the last 90 days and
applies the same feedback-profile reset cutoff. It excludes the current incident, later incidents,
duplicates, invalid values, stale rows, and unanswered rows from agreement. Reuse requires at least
two matching categorized problem confirmations, at least 75% noticed-problem answers, and at least
75% agreement on one symptom category. Sparse evidence remains cold and disagreement remains
explicitly conflicting; neither is reused. Reset and one-step rollback immediately remove and
restore eligible history without rewriting any annotation or sample.

### Native ML adoption gate

Native ML and ONNX Runtime were evaluated as an optional extension, not assumed as a requirement.
No representative held-out labeled dataset currently demonstrates a material quality gain over
the deterministic statistical pipeline. Adopting a runtime under that condition would add model
loading, compatibility, memory, binary-size, and failure paths without established user benefit.
The decision for V0.9 is therefore **not adopted**:

- no ONNX Runtime or other inference dependency is in the manifest or runtime graph;
- no model artifact, training code, Python runtime, or training tool ships in the application;
- provenance explicitly reports `not_adopted`; and
- statistical diagnosis and collection are the normal path, not an ML fallback path.

Reconsideration requires a versioned representative dataset, held-out comparison against this
pipeline, a material predeclared quality improvement, and measured latency, peak memory, binary
size, startup/model-load, missing/corrupt/incompatible-model behavior, and analysis-disabled
operation. Training and dataset tools must remain outside the shipped runtime even if that gate is
eventually satisfied.

### V0.9 validation

The controlled Release fixture diagnosed all four labeled resource incidents correctly and made
zero diagnoses for the quiet fixture (`4/4`, `0/1` false positives). Repeated runs produced
byte-equivalent result values. The complete Debug and Release matrices each pass all 145 CTest
tests; the analysis-disabled configuration passes 106 tests and the headless configuration passes
67.

Twenty-trial Release measurements compare the existing component analysis with the composed
pipeline. Negative overhead values are ordinary timer noise and mean no measurable composition
overhead in that trial:

| Processes | Components avg | Pipeline avg | Measured overhead | Pipeline P95 | Pipeline max | Peak temporary memory |
|---:|---:|---:|---:|---:|---:|---:|
| 50 | 4.928 ms | 4.897 ms | -0.031 ms | 5.284 ms | 5.339 ms | 552,960 B |
| 200 | 21.298 ms | 21.171 ms | -0.127 ms | 21.991 ms | 22.025 ms | 1,798,144 B |
| 500 | 57.404 ms | 57.300 ms | -0.104 ms | 58.630 ms | 58.798 ms | 4,485,120 B |

The Release application is 10,066,944 bytes. The prior packaged V0.8 executable was 9,923,072
bytes, so the incremental V0.9 pipeline/UI cost is 143,872 bytes (140.5 KiB). An
analysis-disabled Release application is 1,071,616 bytes, confirming that the optional analysis
graph remains removable and that collection builds without any ML runtime. The standalone
benchmark executable is 852,992 bytes.

These are controlled regression measurements, not population-level claims. Wider diagnosis
quality claims require the representative dataset specified by the adoption gate.

## V0.3 personalized process baselines

Personalization lets BlackBox distinguish “unusual for this executable” from merely high absolute
or incident-local activity. It remains deterministic statistical analysis: there is no model,
training runtime, background learner, or collection-path dependency.

### Learning and scoring policy

When an archived incident is opened, the viewer worker loads prior observations for executable
identities present in that incident. The personalized analyzer first runs the V0.2 incident-local
analysis, then compares each process candidate's strongest evaluation-window CPU fraction, working
set bytes, disk-read rate, and disk-write rate with that executable's prior incident observations.

Eight available prior observations are required for a metric. With enough history, the executable
profile's median/MAD/IQR robust score replaces the incident-local score for that metric. This is
what allows repeated compiler-like CPU activity to become normal for the compiler while the same
CPU change remains anomalous for an unrelated executable. Metrics without enough history retain
their incident-local evidence, and the process explicitly reports profile cold start. Confidence
is low during cold start, moderate from 8 observations, and high from 30 observations.

After scoring, one observation per executable and incident is stored. Multiple process instances
with the same executable identity contribute the maximum evaluated value per metric. The
`(executable_key, incident_id)` primary key makes reopening an incident idempotent. An incident is
never included in its own baseline, and older incidents cannot use observations from newer ones.

### Identity policy

Identity is normalized without filesystem access so analysis remains portable and reproducible:

1. An available executable path is preferred. ASCII letters are lowercased, `/` becomes `\`,
   repeated separators and a Windows `\\?\` prefix are removed, and trailing separators are
   trimmed. The key is namespaced as `path:`.
2. If the path is inaccessible, unsupported, or empty, an available process name is trimmed,
   ASCII-lowercased, and namespaced as `name:`. Path and name namespaces never merge.
3. If neither value is usable, that process remains incident-local and no profile is learned.
4. Keys longer than 2,048 bytes are not personalized rather than truncated or ambiguously hashed.

A rename or path move creates a cold new identity. An in-place upgrade at the same normalized path
shares history. V0.3 does not collect or use file hashes, versions, publishers, or signatures, so
it cannot distinguish two binaries that replace one another at the same path. Non-ASCII case
folding is intentionally not attempted without a platform/filesystem identity service.

### Aging and bounds

Only observations from the preceding 30 days are eligible. Each executable retains its newest 64
incident observations. SQLite retains at most 2,048 executable identities; inserting a new one at
the cap evicts the least-recently-seen key, with lexical key order as a deterministic tie-breaker.
At most 512 keys are queried or updated for one incident. These limits bound profile storage to
131,072 observation rows plus 2,048 identity rows, independent of recorder uptime or archive size.

The profile tables share the incident archive's transaction, WAL, busy timeout, logical size cap,
and error categories. Profile query/update failures degrade only personalization for that viewer
result; incident-local analysis and recording continue. Learning happens only on explicit incident
view, never during normal rolling telemetry.

### Interpretation and poisoning risk

Profiles describe recurrence, not correctness. Repeated faults can eventually become statistically
normal, and shared paths can combine behavior from replaced binaries. The UI therefore labels the
baseline scope and continues to state that scores are correlation, not proven causation. The
30-day/64-observation aging window limits stale influence but does not solve poisoned baselines;
future feedback and executable-signature policy must be evaluated before stronger claims.

## V0.6 contributor ranking

BlackBox ranks recorded process activity that is plausibly related to an incident. A rank is
correlation evidence, not a causal conclusion. The public model deliberately offers only
`potential` and `likely`; it has no “proven cause” state. Temporal role is separate: activity is
classified as preceding, marker-spanning/ambiguous, or a possible post-marker victim/reaction.
Only genuinely preceding activity can be promoted to likely.

### Inputs and bounds

Ranking consumes only the immutable incident and the portable V0.2/V0.3 analysis result. It does
not query the operating system, storage, UI, provider, recorder, or collector. The statistical
analyzer supplies at most 100 process anomalies from at most 512 evaluation candidates; the ranker
returns at most 20 rows. It scans the incident's already-bounded process rows once, so work remains
linear in the incident rather than multiplying samples by candidate count.

Only increased process CPU, working set, disk read, or disk write activity is eligible. A process
metric matches CPU, memory, or disk system evidence only when the system anomaly has the same
direction. Missing values stay missing.

### Inspectable score

Each candidate retains five normalized factors:

```text
raw = 0.35 * anomaly magnitude
    + 0.25 * timing
    + 0.20 * same-resource match
    + 0.15 * anomalous duration
    + 0.05 * prior recurrence

score = raw * (0.5 + 0.5 * evidence coverage)
```

Timing is 1.0 for preceding activity beginning within 10 seconds before the marker, 0.8 within 30
seconds, 0.5 for older preceding activity, 0.35 for marker-spanning activity whose anomalous
samples are mostly post-marker, and 0.15 for wholly post-marker activity. Duration reaches 1.0 at three
seconds. Recurrence reaches 1.0 after three earlier matching personalized-profile observations.
Coverage is the available observation fraction across all four process metrics; unavailable
metrics reduce both score and confidence rather than creating synthetic zeros or ranks.

“Likely contributor (correlation only)” requires a score of at least 0.75, anomaly magnitude of at
least 0.75, same-resource score of at least 0.5, non-low confidence, and predominantly preceding
activity. Marker-spanning rows say “Ambiguous correlate across marker,” wholly post-marker rows say
“Possible victim/reaction (not a causal rank),” and weaker preceding rows remain potential
contributors. The UI shows every factor, pre/post anomalous-sample counts, coverage, missing-metric
count, onset relative to the marker, and anomalous duration.

When opt-in process lifecycle recording supplied an exact `(PID, creation token)` start or exit
inside the immutable incident, the row also shows that timestamp separately. “Activity began” is
the first anomalous recorded process sample and is the only onset used by the timing score. “Process
started” is a lifecycle event and never changes the score. Attachment requires the process source,
matching event kind and full durable identity, an event inside the incident window, a start no later
than the first anomalous sample, or an exit no earlier than the last anomalous sample. Reused PIDs,
wrong kinds/sources, inconsistent ordering, and out-of-window events are ignored. Absence is not
interpreted as evidence that a process was already running because collection may have been disabled
or the start may predate the event-ring window.

### Explicit local attribution calibration

V0.16 adds a separate causal-attribution question to each ranked row: `Unsure`, `Confirmed
contributor`, or `Not a contributor`. This is intentionally independent of the incident-level
“did you notice a problem?” answer. Symptom feedback cannot teach contributor ranking, and an
attribution never changes the incident on which it was entered.

For a future incident, the portable analyzer considers at most 256 newest attribution rows from
the preceding 90 days. A row must have an exact normalized executable key and resource match, be
strictly earlier than the current incident, have been entered no later than the current incident,
and fall after the active profile-reset cutoff. The current incident, future/stale rows, repeated
rows for one incident, mismatched keys/resources, and invalid timestamps cannot contribute. Four
distinct exact matches and at least 75% agreement are required; conflicting evidence remains
inspectable and has no effect.

Confirmed consensus can increase an already-observed preceding candidate by at most 15%; it never
promotes marker-spanning or post-marker activity. Each stored vote retains the source row's temporal
role, and a confirmation originating from anything except genuinely preceding activity is excluded
from positive learning. Rejected consensus can reduce an existing candidate by at most
30%. Laplace smoothing keeps a small unanimous set away from either cap. Calibration never creates
a process candidate, changes raw resource/process evidence, or lets a sub-threshold incident-local
candidate manufacture symptom alignment. The original score, multiplier, counts, and resulting
score remain visible, and reset/one-step rollback reuse the common feedback-profile cutoff.

### Validation and limitations

Labeled CPU, memory, and disk fixtures each place the intended preceding process above equally
anomalous marker-spanning and post-marker rows (3/3 top-rank accuracy). Missing-metric and
recurring-history fixtures verify confidence degradation and the recurrence cap. UI tests lock the
calibrated wording and all three temporal roles.

V0.16 controlled tests additionally cover positive and negative consensus, bounded reranking,
marker-spanning/post-marker non-promotion, source-timing provenance, exact identity/resource
matching, duplicate/current/future/stale/reset exclusions, conflicting evidence, bounded scanning,
direct-schema-V1 persistence, and asynchronous
viewer updates. These prove the safety contract and deterministic mechanics, not population-level
causal accuracy; the V0.15.1 representative held-out gate is still required for that claim.

Lifecycle-context tests additionally lock exact full-identity matching, start/exit ordering,
incident-window bounds, reused-PID rejection, activity-versus-process-start terminology, and the
invariant that adding valid lifecycle context leaves the candidate score unchanged.

Twenty-trial Visual Studio 2026 x64 Release measurements after V0.16 calibration metadata was
added were:

| Processes | Process rows | Average | P95 | P99 / maximum | Returned |
|---:|---:|---:|---:|---:|---:|
| 50 | 7,500 | 0.828 ms | 0.969 ms | 1.119 ms | 20 |
| 200 | 30,000 | 3.390 ms | 3.712 ms | 3.761 ms | 20 |
| 500 | 75,000 | 8.258 ms | 8.998 ms | 9.306 ms | 20 |

These fixtures validate deterministic ranking behavior, not real-world causal accuracy. A late or
imprecise marker can invert the temporal interpretation; a common upstream cause can make several
processes correlate; unavailable protected-process data can hide a contributor; and recurrence can
represent a normal repeated workload. Users should inspect the linked timeline evidence.

## Recurring incident discovery

BlackBox groups repeated incident shapes locally so users can find similar captures and inspect
their shared evidence. Discovery is post-capture and optional. It runs on the existing viewer
worker, never on the collector or render thread, and does not change incident telemetry.

### Versioned feature vector

Feature version 2 has sixteen dimensions, each scaled to `[0,1]` with an independent availability
bit:

| Dimensions | Definition and scaling |
|---|---|
| CPU peak / near marker | Maximum available CPU fraction overall and within ±15 seconds of the marker |
| Memory peak / near marker | Maximum available memory fraction overall and within ±15 seconds |
| Disk peak / near marker | Read plus write bytes/s, scaled as `log1p(value) / log1p(1 GiB/s)` |
| Network peak / near marker | Receive plus transmit bytes/s with the same logarithmic scaling |
| Disk-quality peak / near marker | Strongest latency/service value scaled at 200 ms or queue depth scaled at 16 requests |
| Network-quality peak / near marker | Strongest connectivity disruption, retransmission fraction, or bounded transition/failure/reset event signal |
| Dominant pre/post share | Pre- and post-marker peaks divided by their sum for the dominant resource |
| Duration | Actual incident duration divided by 150 seconds |
| Dominant concentration | Dominant resource peak divided by the sum of available resource peaks |

All results are clamped to `[0,1]`. Missing values retain `available=false`; they are not zeros.
Changing any definition, scale, or order requires a new feature version and invalidates old cache
rows automatically.

### Distance, grouping, and noise

Distance is root-mean-square difference across dimensions. A dimension available on only one side
adds a fixed `0.35` missingness penalty. Fewer than four mutually available dimensions produces
distance `1.0`. Automatic membership requires every pair in a group to be at or below the
documented `0.20` threshold (deterministic complete-link threshold grouping). Groups require at
least two incidents; unmatched singletons are explicit noise/unique incidents.

Inputs are sorted deterministically and hard-capped to the newest 512 valid incidents. Group keys,
member order, shared-characteristic order, and noise order are stable for a fixed archive and
feature version. The three highest supported median features are displayed with coverage, along
with occurrence count and maximum pair distance. Adding an incident can legitimately reorganize an
ambiguous automatic group; the feature version and threshold make that decision reproducible.

### Cache and user overrides

The pre-release schema-v1 baseline stores generic per-dimension feature cache rows and an optional recurring-group override
on the incident. A refresh loads cached version-2 vectors and computes only missing or stale ones;
version-1 rows are invalidated rather than misinterpreted.
The cache is derived data; failure to update it does not change the incident and merely causes a
later recomputation. At most 512 vectors of at most 32 dimensions can be written in one transaction,
and cache rows outside the newest 512 incidents are pruned transactionally.

A non-empty override is an explicit user decision. Incidents with exactly the same override text
are shown together even when their statistical distance exceeds `0.20`; a single overridden
incident remains inspectable as a one-member user group. Overrides are limited to 64 UTF-8 bytes.
Clearing the text returns the incident to automatic grouping. Automatic and user-overridden groups
are labeled separately so a manual decision is never presented as algorithmic evidence.

### Confirmed symptom context

When a loaded incident belongs to an automatic group, the viewer can show a bounded summary of
earlier members whose users both noticed a problem and selected a symptom category. This reuses
feedback only as historical context. It does not change the current diagnosis, confidence,
contributor ranking, group membership, or immutable telemetry, and no recurring member is described
as causal proof.

At most 32 distinct prior members within 90 days are considered. The current/future incident,
duplicates, rows at or before the feedback reset cutoff, manual groups, and invalid classifications
are excluded. Readiness requires two matching confirmations, at least 75% noticed-problem answers,
and 75% category consensus. Sparse or conflicting feedback remains visible as cold/conflicting and
is not reused. The existing confirmed reset and one-step rollback apply to this context as well as
automatic-trigger calibration.

### Validation and measured cost

Deterministic CPU and disk families captured at different UTC dates form separate three-member
groups; a balanced pattern remains noise. Reversing 512 inputs produces an identical result.
Cache replacement, restart persistence, worker-thread execution, shared evidence,
manual override, and return-to-automatic behavior are automated.

Twenty-trial Visual Studio 2026 x64 Release measurements on the V0.7 development host were:

| Incidents | Average | P95 | Maximum | Input object payload | Peak temporary working-set delta | Stable trials |
|---:|---:|---:|---:|---:|---:|---:|
| 32 | 0.078 ms | 0.100 ms | 0.124 ms | 5,376 B | 143,360 B | 20/20 |
| 128 | 0.526 ms | 0.589 ms | 0.785 ms | 21,504 B | 20,480 B | 20/20 |
| 512 | 7.240 ms | 8.007 ms | 11.538 ms | 86,016 B | 16,384 B | 20/20 |

Working-set deltas are process-wide and page-granular, so the non-monotonic samples are expected;
the explicit input payload shows linear growth. The algorithm allocates no distance matrix and its
incident count is fixed at 512. A first refresh may load every uncached bounded
incident; later refreshes reuse the cache.

These groups describe telemetry shape, not root cause. Logarithmic throughput scaling, missing
signals, short capture windows, and the fixed threshold can merge or split scenarios imperfectly.
Users should inspect member timelines and shared evidence, and use overrides only as their own
organizational judgment.

## V0.8 probabilistic workload context

BlackBox recognizes broad workload context only after an immutable incident has been captured. The
recognizer is part of optional `blackbox_analysis`; it has no telemetry, recorder, storage, UI, OS,
or filesystem dependency. Disabling analysis removes it, and disabling only workload context leaves
the existing statistical/personalized analyzer active with unchanged scores.

### Outputs and explainable signals

Every enabled assessment contains probabilities for Unknown, Idle, Gaming, Development,
Compilation, Video playback/call, Heavy download, and Desktop. The highest probability is a display
summary, not a hard decision. The full distribution feeds ranking. Unknown competes explicitly with
the seven recognized classes and wins when support is weak or the strongest two workloads have a
small margin. An Unknown result reports high uncertainty rather than inventing user intent.

The deterministic version-1 heuristic uses incident-wide averages of normalized CPU, memory, disk,
and network telemetry plus a bounded process-name lexicon. It recognizes names only from recorded
metadata, lowercases at most 260 bytes per name without locale or filesystem calls, and inspects at
most 512 metadata records. Signal contributions are retained as portable enum/value rows; the viewer
renders at most eight. Names themselves are not copied into context evidence.

Each workload receives a support value in `[0,1]`. Supports and an ambiguity-derived Unknown support
are converted to weights with `exp(5 * support) - 1`, then normalized to a probability distribution.
This sharpening keeps a strong explainable workload distinct while preserving probability mass for
alternatives. The implementation is deterministic for a fixed incident and configuration.

### Soft anomaly adjustment

Raw statistical or personalized evidence and its score remain unchanged and inspectable. A separate
ranking score uses:

```text
multiplier = 1 - 0.20 * sum(context_probability * expected_resource_weight)
rank_score = raw_score * multiplier
```

The default maximum reduction is 20% and validation forbids a value above 25%. Unknown and Idle have
zero expected-resource weights, so uncertainty naturally removes most or all adjustment. Approximate
expectation weights are:

| Workload | CPU | Memory | Disk | Network |
|---|---:|---:|---:|---:|
| Gaming | 0.90 | 0.55 | 0.15 | 0.20 |
| Development | 0.35 | 0.45 | 0.30 | 0.10 |
| Compilation | 1.00 | 0.35 | 0.80 | 0.05 |
| Video playback/call | 0.45 | 0.30 | 0.20 | 0.75 |
| Heavy download | 0.10 | 0.10 | 0.65 | 1.00 |
| Desktop | 0.20 | 0.25 | 0.15 | 0.10 |
| Idle / Unknown | 0.00 | 0.00 | 0.00 | 0.00 |

Process ranks use the weight for their strongest available metric. Resource and process rows expose
both raw score and applied multiplier through the view model. Contributor ranking runs afterward,
so it consumes the same softly contextualized ranks while retaining its causal-language limits.

### Bounds, privacy, and failure behavior

Recognition makes one pass over the already bounded incident system samples and one pass over at
most 512 process metadata records. It creates eight probabilities and at most eight evidence rows.
It does not persist context, change the storage schema, resolve executable paths, query the OS, start a
thread, or perform I/O. The existing viewer worker performs recognition; neither the collector nor
render thread can reach the analyzer.

Process-name patterns can reveal or misclassify activity. BlackBox therefore keeps context local,
does not store the derived label, calls it probabilistic in the UI, exposes its uncertainty and
signals, and caps its influence. A mistaken context can change ordering by at most the configured
reduction; it cannot erase or rewrite recorded evidence.

### Validation and measured cost

Two independently named deterministic fixtures for each of the eight classes form a 16-case labeled
confusion suite. The matrix is 16/16 correct, including 2/2 ambiguous Unknown fixtures. The fixture
suite also checks normalization, determinism, hard bounds, the context-off path, raw-evidence
preservation, and minimum multiplier.

Six held-out ranking scenarios place a slightly larger anomaly on a workload-expected resource and
the labeled symptom on a less-expected resource. Baseline top-one accuracy is 0/6; probabilistic
context is 6/6. A paired protected set begins with the labeled symptom clearly first and records 0/6
regressions. These synthetic results validate mechanics and guardrails, not real-world calibration;
future labeled captures must broaden names, mixed workloads, and hardware distributions.

On the V0.8 development host, 4,000 Visual Studio 2026 x64 Release recognitions over the 150-sample
labeled fixtures measured 11.178 microseconds average, 13.600 microseconds P95, 20.300 microseconds
P99, and 42.400 microseconds maximum. Run `blackbox_context_recognition_benchmark` to reproduce the
full confusion matrix, overhead distribution, held-out ranking gain, and protected regression count.
