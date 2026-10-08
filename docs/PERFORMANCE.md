# Performance and overhead

The [product-default rehearsal](TESTING.md#separate-product-default-rehearsal) measures
working set, private bytes, handles and CPU around full-size captures. Final collection/jitter
percentiles describe at most 256 recent observations; explicit lifetime maxima preserve earlier
spikes within the configuration epoch. Snapshot timings include a lifetime maximum; writer
P99/maximum fields explicitly describe the retained write-timing window, including failed attempts.
Use `measure-ui-runtime.ps1 -UseProductDefaults` for separate visible, minimized, hidden and
background measurements with automatic detection and process paths enabled. Preserve
configuration and warmup. Short local
runs do not replace controlled long runs or lower-end/battery-hardware qualification.

BlackBox must measure its own cost. Targets below are engineering goals, not unverified claims.

## Initial targets

| Measure | Target |
|---|---:|
| Idle CPU | Ideally below 0.5% |
| Typical recording CPU | Ideally below 1% |
| Working set | 30-50 MB initially |
| Telemetry disk writes during normal recording | Effectively zero |
| Dropped normal samples | 0 |
| Default sample interval | 1 second |
| Default history | 5 minutes |

Results are invalid unless they include build type, commit, OS/build, hardware, power mode, sample configuration, process count, run duration, and measurement tool.

## Runtime diagnostics

The collector will maintain allocation-bounded counters/histograms for:

- configured interval and actual inter-sample interval
- sample collection duration: count, mean, maximum, P50/P95/P99
- scheduling jitter (`actual_start - scheduled_start`)
- collected, late, failed, and dropped sample counts
- per-tier and, when affordable, per-provider timing
- ring capacity, size, and overwrite count
- incident snapshot construction and writer queue latency
- incident transaction/serialization duration and failures
- process working set and CPU usage for BlackBox itself

Percentiles must use a bounded histogram or bounded diagnostic window; diagnostics cannot grow without limit. Timing uses `std::chrono::steady_clock`. Wall time is recorded only for correlation and reporting.

## Benchmark procedures

### Idle recorder

1. Build Release with symbols and default one-second/five-minute settings.
2. Reboot or document machine uptime and background workload; wait for startup activity to settle.
3. Run BlackBox for at least 30 minutes, UI hidden for the primary measurement.
4. Record BlackBox CPU time, working set/private bytes, system CPU, disk I/O bytes/operations, and diagnostic percentiles.
5. Verify that no SQLite incident writes occur and that dropped samples remain zero.
6. Repeat at least three times and report median plus range.

### Active UI

Repeat the idle procedure with the Live view visible. Record render frame rate and UI-thread CPU separately where tooling permits. Compare visible, minimized, and hidden states. Hidden/minimized UI CPU is a release gate once tray/background behavior exists.

### Desktop presentation

Measure visible, minimized, hidden and background operation separately with
`scripts/measure-ui-runtime.ps1 -UseProductDefaults`. Idle presentation is capped at
30 Hz and recent interaction raises it to 60 Hz. Hidden operation skips rendering
and dashboard snapshot construction. Collection cadence is independent.

Preserve revision, warmup, sample configuration and hardware with every result.
Compare repeated runs on the same machine before attributing a change to code.

### Process-scale matrix

Measure at approximately 50, 200, 500, and the highest practical process count. Report full collection latency and per-process cost. Include protected/inaccessible processes to verify failures do not cause retry storms or repeated path resolution.

### Sampling-rate matrix

Run at 1000 ms, 500 ms, and 250 ms only after those configurations are supported. Report CPU, jitter, P95/P99 collection time, and deadline misses. A supported interval requires P99 collection plus scheduling margin to fit reliably on representative hardware.

### Incident persistence

Capture representative 150-second incidents at each supported interval and process scale. Report snapshot time, queue delay, transaction latency, database growth, peak temporary memory, and collector jitter during the write. Force database lock, full disk, invalid path, and corruption/open failures in tests; collection must continue.

## Memory measurement

Measure working set and private committed bytes after warm-up, after the ring first reaches capacity, and after repeated incident captures. Verify steady state rather than startup alone. Attribute memory by major component when practical: executable/dependencies, UI, ring samples, process metadata cache, immutable incident work items, and SQLite.

Capacity models must be documented before recorder implementation:

```text
system ring bytes ~= system_sample_size * history_samples
process ring bytes ~= active_process_samples_per_tick * history_samples * process_sample_size
```

Avoid retaining duplicate process strings in each sample. Incident work queues must be bounded so slow storage cannot create unbounded memory growth.

## Dropped and late sample detection

Each scheduled tick receives a monotonically increasing sequence number and scheduled time. Diagnostics distinguish:

- late: collection started beyond the jitter threshold
- deadline miss: collection finished after the next scheduled tick
- dropped: a scheduled tick was intentionally skipped rather than creating a burst to catch up
- provider failure: tick occurred but one or more signals were unavailable

The scheduler does not perform back-to-back catch-up polling after a stall. V0.0.4 advances directly to the first future monotonic deadline and counts every skipped tick as dropped; exact-boundary and multi-tick stalls are unit-tested.

## Regression policy

CI runs deterministic unit tests on every change. Stable microbenchmarks and a short mock-recorder benchmark will be added as implementations land. Hardware-sensitive benchmarks run on controlled agents or manually before milestones; ordinary shared CI timing is informational only.

A change is investigated when the same controlled setup shows any of:

- more than 10% CPU or P95/P99 collection-time regression
- more than 5 MB steady-state memory regression
- new steady-state disk writes
- any normal-load dropped samples
- unbounded growth across a soak test

Intentional regressions require a recorded tradeoff, before/after measurements, and approval in the milestone notes. Benchmark raw output should be retained as an artifact; summarized results belong in a versioned report once telemetry exists.

## Historical reference measurement

The September 5, 2026 Windows development rehearsal used full 120/30-second capture
windows and automatic detection. It observed 82.2 MiB peak working set and 132.4 MiB
peak private bytes, with two snapshots taking at most 12.05 ms and two writes at
most 115.34 ms. Two captures cannot establish a tail distribution. Separate
30-second measurements averaged 0.418% total-machine CPU / 74.5 MiB working set
visible, and 0.016% / 59.8 MiB in background mode. These measurements precede the
October hardening changes and are not current-candidate qualification.

The memory results exceed the aspirational 30–50 MB target. Keep the target and
measure representative workloads; do not relax acceptance limits to fit a run.
Detailed historical benchmark diaries are retained in Git history. Current
validation belongs in [Current candidate](CURRENT_CANDIDATE.md).
