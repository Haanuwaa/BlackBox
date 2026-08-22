# Diagnostic evaluation and calibration

## Predeclared primary metrics

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

## Component evaluation

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

## Calibration-only fitting

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

## One-shot held-out evaluation

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

## Publication limits

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

The first frozen V0.15 run and its deliberately narrow conclusions are published in
[`V015_DOGFOOD_RESULTS.md`](V015_DOGFOOD_RESULTS.md).
