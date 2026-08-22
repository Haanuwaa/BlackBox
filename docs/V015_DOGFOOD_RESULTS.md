# V0.15 dogfood evaluation results

## Decision

V0.15 establishes that the native recorder/detector/analyzer path works end to end, but the
current diagnostic quality is not sufficient for a broad V1 claim. Pipeline version 3 is precise
only after a calibration threshold that abstains on most incidents. The results therefore gate
symptom classification and native ML off; they do not justify weakening `Unknown`, missing-data,
or correlation language.

## Frozen evidence

- Corpus: `local-host-2026q3`, protocol version 1, 32 sessions and 32 immutable incidents.
- Annotation fingerprint: `4801556224897119752`.
- Analyzer: pipeline version 3, configuration fingerprint `15102167315426489669`.
- Splits: 8 development, 14 calibration, and 10 one-shot held-out incidents.
- Truth: 32 confirmed, zero uncertain/disputed rows, one annotator, zero measured disagreements.
- Quiet exposure: three 65-second sessions (0.0542 hours), zero automatic captures.
- Hardware: one Windows 11 Pro build-26200 profile, AMD family 25, 12 logical processors,
  32--63 GiB memory bucket, NVIDIA RTX 30 family, balanced power mode.

The corpus contains one development capture for every predeclared symptom. Calibration contains
four CPU, three disk, three network, one hang, one frame-stutter, one audio-gap, and one quiet row.
Held-out contains two CPU, two disk, two network, one hang, one frame-stutter, one audio-gap, and
one quiet row. The game and audio cases are controlled visible/audible surrogates; they are not a
real game or device/driver disruption. Network truth is loopback TCP reset evidence, not WAN,
Wi-Fi, VPN, or ISP latency. There are no natural-session, multi-host, battery-mode, or independent
annotator results.

## Results

Rates below always show their exact eligible denominator. Development and calibration are raw
pipeline outputs. Held-out applies the frozen isotonic confidence model and 80%-minimum-precision
assertion threshold.

| Metric | Development | Calibration | Held-out |
|---|---:|---:|---:|
| Supported diagnosis accuracy | 3/4 (75.0%) | 8/10 (80.0%) | 2/6 (33.3%) |
| Unknown-truth abstention | 0/4 (0%) | 0/4 (0%) | 4/4 (100%) |
| Top-1 contributor | 3/6 (50.0%) | 7/10 (70.0%) | 4/7 (57.1%) |
| Top-3 contributor | 3/6 (50.0%) | 7/10 (70.0%) | 4/7 (57.1%) |
| Context accuracy | 6/8 (75.0%) | 14/14 (100%) | 10/10 (100%) |
| Automatic detector recall | 4/7 (57.1%) | 10/13 (76.9%) | 6/9 (66.7%) |
| Usefulness | 0/5 (0%) | unscored | unscored |
| Unknown output rate | 0% | 0% | 80.0% |
| False-assertion rate on Unknown truth | 100% | 100% | 0% |
| False captures/hour | 0 | 0 | 0 |
| Recurrence pair F1 | 0% | 29.4% | 20.0% |

The calibration fit used 14 rows. Its predeclared precision gate selected calibrated probability
1.0, asserting 3/14 calibration rows with observed precision 3/3. On held-out data it asserted
2/10 rows, both correct; consequently the reported asserted-row Brier score and ECE are zero, but
that tiny selective denominator is not evidence of broad calibration quality. Eight of ten
held-out rows became `Unknown`, including four of six supported-resource truths.

Context accuracy is also not a general context result: every calibration and held-out truth row was
desktop context. Per-process network attribution is not collected, so network contributor truth is
unscored. Contributor matches are temporal correlation, never causal proof. The zero quiet
false-capture rate covers only 195 seconds and must not be extrapolated to an hourly product claim.

## Findings retained from development

- Quiet, hang, frame-stutter, and audio-gap captures received confident incidental resource
  diagnoses before calibration. This motivated explicit Unknown-truth abstention and
  false-assertion metrics.
- Short-lived controlled CPU/disk processes initially disappeared behind immature baselines and
  statistically large but practically tiny process changes. Pipeline 3 retains bounded absolute
  activity as low-confidence `potential` evidence and applies process effect-size floors.
- Automatic trigger resource now disambiguates a nearby stronger incidental resource score. It
  changes the pressure category only; it cannot prove a process cause.
- The held-out gate shows that high precision is currently purchased with excessive abstention.
  V0.16 must improve feedback-driven suppression and evidence quality before symptom classification
  can satisfy its adoption gate.

## Reproduction and retained artifacts

Build `blackbox_dogfood_tool`, then use the commands in `DOGFOOD_PROTOCOL.md` and
`DIAGNOSTIC_EVALUATION.md`. The local `out/dogfood-v015/` directory retains the archive, frozen
corpus, calibration model, privacy-safe predictions, evaluation JSON, and one-shot held-out lock.
These build artifacts are intentionally excluded from source and release packages. A publication
must retain those artifacts and binary hashes together; this document is only the checked-in
summary and limitations record.
