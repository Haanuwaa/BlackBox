# V0.2 statistical incident analysis

BlackBox V0.2 ranks what was unusual inside a captured incident. It does not claim root cause and
does not use machine learning. `analysis::IIncidentAnalyzer` accepts only an immutable
`core::IncidentSnapshot` and returns ranked evidence plus uncertainty. The analysis target depends
only on core; telemetry collection, normalization, recording, capture, and storage do not depend on
it.

## Incident-local windows

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
`PERSONALIZATION.md` for identity, aging, persistence, and cold-start policy.

## Robust statistics and score

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

## Ranking

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

## Optional boundary

Configure with `-DBLACKBOX_ENABLE_ANALYSIS=OFF` to omit the analysis target and tests. The viewer
then states that analysis is disabled. CI's headless collection graph disables application, SQLite,
UI, and analysis together. Analysis is recomputed on the viewer worker when an incident is loaded;
V0.2 does not alter the archive schema or write scores continuously. V0.3 adds bounded profile
observations that are queried and updated only on the viewer worker when an incident is opened;
normal recording remains RAM-only.

Scores mean “unusual relative to this incident's baseline,” never “proved cause.” A high score can
reflect an expected phase change, and a zero score can reflect missing/cold data or a consistently
high workload. The UI shows evidence and confidence beside every ranking and repeats this caveat.

## V0.6 contributor ranking

The analyzer now derives at most 20 contributor candidates after statistical and personalized
process scoring. The score combines anomaly magnitude, marker-relative timing, same-direction
system-resource evidence, anomalous duration, prior recurrence, and explicit evidence coverage.
Anomalous sample balance now separates genuinely preceding activity, marker-spanning ambiguity,
and wholly post-marker possible victims/reactions. Only the first can receive the `likely` label.
Missing process metrics reduce score and confidence. Exact weights, thresholds, bounds, measured
latency, and limitations are documented in `CONTRIBUTOR_RANKING.md`.

V0.16 can apply bounded explicit contributor-attribution history after incident-local ranking.
This downstream value-only calibration preserves the original score, never creates a candidate or
promotes marker-spanning or post-marker activity, accepts positive learning only from a prior
preceding row, and cannot use noticed-problem feedback as causal evidence. See
`CONTRIBUTOR_RANKING.md` for consensus, reset, poisoning, and inspection rules.

## V0.7 recurring incident discovery

The optional analysis module also extracts versioned, scaled system-shape vectors and groups at
most 512 archived incidents on the viewer worker. Automatic complete-link groups require at least
two members within a fixed distance threshold; unmatched incidents remain explicit noise. Generic
storage cache records and manual group labels are mapped only by the application composition
boundary. Exact dimensions, missingness distance, cache invalidation, override semantics,
benchmarks, and limitations are documented in `RECURRING_INCIDENTS.md`.
