# Offline model research harness

BlackBox does not ship a machine-learning model or runtime. The offline harness makes future model
experiments reproducible while preserving the recorder-first architecture and the frozen-corpus
rules. It is a development target and is not installed in the release package.

## Label-free feature export

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

## Baseline comparison

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
