# V0.6 contributor ranking

BlackBox ranks recorded process activity that is plausibly related to an incident. A rank is
correlation evidence, not a causal conclusion. The public model deliberately offers only
`potential` and `likely`; it has no “proven cause” state. Temporal role is separate: activity is
classified as preceding, marker-spanning/ambiguous, or a possible post-marker victim/reaction.
Only genuinely preceding activity can be promoted to likely.

## Inputs and bounds

Ranking consumes only the immutable incident and the portable V0.2/V0.3 analysis result. It does
not query the operating system, storage, UI, provider, recorder, or collector. The statistical
analyzer supplies at most 100 process anomalies from at most 512 evaluation candidates; the ranker
returns at most 20 rows. It scans the incident's already-bounded process rows once, so work remains
linear in the incident rather than multiplying samples by candidate count.

Only increased process CPU, working set, disk read, or disk write activity is eligible. A process
metric matches CPU, memory, or disk system evidence only when the system anomaly has the same
direction. Missing values stay missing.

## Inspectable score

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

## Explicit local attribution calibration

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

## Validation and limitations

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
