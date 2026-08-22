# Recurring incident discovery

BlackBox groups repeated incident shapes locally so users can find similar captures and inspect
their shared evidence. Discovery is post-capture and optional. It runs on the existing viewer
worker, never on the collector or render thread, and does not change incident telemetry.

## Versioned feature vector

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

## Distance, grouping, and noise

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

## Cache and user overrides

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

## Confirmed symptom context

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

## Validation and measured cost

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
