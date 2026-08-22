# V0.8 probabilistic workload context

BlackBox recognizes broad workload context only after an immutable incident has been captured. The
recognizer is part of optional `blackbox_analysis`; it has no telemetry, recorder, storage, UI, OS,
or filesystem dependency. Disabling analysis removes it, and disabling only workload context leaves
the existing statistical/personalized analyzer active with unchanged scores.

## Outputs and explainable signals

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

## Soft anomaly adjustment

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

## Bounds, privacy, and failure behavior

Recognition makes one pass over the already bounded incident system samples and one pass over at
most 512 process metadata records. It creates eight probabilities and at most eight evidence rows.
It does not persist context, change the storage schema, resolve executable paths, query the OS, start a
thread, or perform I/O. The existing viewer worker performs recognition; neither the collector nor
render thread can reach the analyzer.

Process-name patterns can reveal or misclassify activity. BlackBox therefore keeps context local,
does not store the derived label, calls it probabilistic in the UI, exposes its uncertainty and
signals, and caps its influence. A mistaken context can change ordering by at most the configured
reduction; it cannot erase or rewrite recorded evidence.

## Validation and measured cost

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
