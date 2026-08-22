# Intelligent analysis pipeline

Version 0.9 composes the existing incident-local anomaly, personalized executable,
workload-context, recurring-pattern, and contributor components into one coherent local
diagnosis. It does not change collection, normalization, recording, capture, or the archive
schema.

## Boundary and data flow

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

## Determinism and calibration

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

## Local feedback calibration

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

## Confirmed similar-incident context

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

## Native ML adoption gate

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

## V0.9 validation

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
