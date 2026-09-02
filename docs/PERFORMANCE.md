# Performance and overhead

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

### V0.23 four-state desktop characterization

Measured 2026-09-01 with the MSVC 19.51 x64 Release `0.23.0` executable on Windows
10.0.26200, 12 logical processors, High performance power mode, default one-second/five-minute
recording, and approximately 289 host processes. Each 20-second run discarded the first five
seconds, sampled the exact launched process every 250 ms, and retained the application diagnostic
report beside the measurement. All four runs completed 21 collections with zero partial, failed,
dropped, late, or deadline-missed samples. The executable SHA-256 was
`b5881bf7c5bc0bd3ddec57abebb12f77ce4c2767f52b34d2f4355c82a349e523`.

| Shell state | Samples | Average/max total-machine CPU | Average/max working set | Average/max private bytes |
|---|---:|---:|---:|---:|
| Visible live dashboard | 63 | 1.730% / 4.114% | 68.86 / 72.30 MiB | 123.92 / 128.34 MiB |
| Minimized after visible startup | 63 | 0.155% / 2.051% | 64.02 / 65.20 MiB | 122.81 / 124.95 MiB |
| Runtime-hidden after visible startup | 63 | 0.033% / 1.026% | 64.64 / 66.06 MiB | 123.16 / 125.55 MiB |
| Start-hidden background | 64 | 0.080% / 0.515% | 53.71 / 54.99 MiB | 113.74 / 116.25 MiB |

This closes the missing full-app visible comparison and confirms that hidden/start-hidden average CPU
remains below the 1% release floor and working set remains below 80 MiB on this host. The visible
result includes SDL/ImGui command generation, renderer submission, presentation,
dashboard refresh, and native telemetry; it is deliberately not substituted with the existing CPU-
side ImGui microbenchmark. These are short dirty-worktree development measurements, not the required
three 30-minute clean-revision repetitions. Reproduce them with `scripts/measure-ui-runtime.ps1` in
`Visible`, `Minimized`, `Hidden`, and `Background` modes; the four outputs use an atomic `.partial`
publication path and record OS, processor capacity, executable hash, duration, and memory samples.

### V0.24 visible presentation optimization

The V0.23 loop refreshed the bounded dashboard projection at 4 Hz but rebuilt and presented an
identical ImGui command stream at monitor VSync between model changes. A 500-process CPU-side viewer
benchmark averaged 0.030 ms per frame with 0.073 ms P99, locating the avoidable cost in redundant
renderer presentation rather than recorder snapshots, dashboard projection, or view construction.

V0.24 adds an application-owned 33 ms visible-frame scheduler. It renders immediately on first show
or after restore, processes SDL events while waiting, bounds input-to-frame latency to one interval,
and advances past stale deadlines without catch-up bursts. Hidden/minimized behavior remains on its
existing 250 ms event wait and resets the visible deadline. The collector remains independent of UI
lifetime and frame rate.

The revised measurement harness stages and validates isolated direct-V1 product/recorder settings,
disables automatic capture and event sources, samples only the requested window, and records process
lifetime separately. This prevents user settings, incidental automatic triggers, and shutdown work
from contaminating CPU samples. On the same Windows 10.0.26200, 12-logical-processor host, the exact
V0.23 `8a35fc5` hosted package and the rebuilt V0.24 development executable each ran a 30-second
visible measurement with a five-second warm-up:

| Measure | V0.23 uncapped | V0.24 30 Hz | Change |
|---|---:|---:|---:|
| Average total-machine CPU | 1.667% | 0.349% | -79.1% |
| Maximum total-machine CPU | 4.613% | 1.533% | -66.8% |
| Maximum working set | 65.27 MiB | 66.00 MiB | +0.73 MiB |
| Maximum private bytes | 126.38 MiB | 125.75 MiB | -0.63 MiB |

Both runs completed 31 collections with zero partial, failed, dropped, late, deadline-missed, or
worker-failed samples and zero automatic triggers/captures. The before/after artifacts are retained
under `out/ui-performance-v024/`. The optimized executable was `local-uncommitted`; these results
establish the engineering decision but do not replace three controlled 30-minute repetitions on a
clean candidate revision.

After freezing, exact clean revision `85044988da5817fa4e759b6c57d014731ce7a528` repeated the same
30-second visible check at 0.395% average and 1.532% maximum total-machine CPU with a 66.52 MiB
maximum working set. It completed 31 collections with every failure/drop/deadline counter at zero.
This confirms the short characterization on the hosted-qualified binary but remains too short to
close the controlled-repetition gate.

V0.25 retains the measured 33 ms idle ceiling and adds a 16 ms presentation interval for 300 ms
after direct mouse, touch, text, or keyboard input. Repeated interaction extends that bounded window
but never renders more than once per interval. This addresses the visible tradeoff in the fixed cap:
30 Hz was adequate for clicks and static telemetry, but continuous scrolling, dragging, and graph
hover could feel less fluid than a 60 Hz desktop. Collection and dashboard projection rates are
unchanged, hidden/minimized behavior remains event-driven, and deterministic tests cover idle,
interaction, expiry, reset, and stale-deadline behavior. The controlled clean-revision runtime
comparison remains open until V0.25 is frozen.

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

## V0.0.1 baseline

V0.0.1 validates compilation, launch, and clean UI shutdown only. Recorder overhead cannot yet be measured because no collector exists. The placeholder plot uses fixed data and is not a telemetry benchmark. The first collection timing baseline is required by V0.0.3, with the full V0.1 profile serving as a release gate.

## V0.0.2 scalar normalization baseline

Measured 2026-08-17 with `blackbox_normalization_benchmark`, MSVC 19.51.36256 x64 Release, Windows 10.0.26200, an AMD64 Family 25 Model 33 processor, and 12 logical processors. Each trial performs 10,000,000 available cumulative-byte rate normalizations with a one-second interval.

| Trial | ns/normalization |
|---:|---:|
| 1 | 67.3718 |
| 2 | 65.9048 |
| 3 | 66.6763 |
| 4 | 69.6781 |
| 5 | 74.2434 |
| **Median** | **67.3718** |

Range: 65.9048-74.2434 ns/operation. The checksum was identical (`4.096e+10`) in all trials. This is a microbenchmark baseline, not an end-to-end collector result, and no regression threshold is enforced from a single machine. Scalar metric/unit types are trivially copyable, and normalization helpers are `noexcept`; their implementation contains no allocation path.

## V0.0.3 Windows provider baseline

Measured 2026-08-17 on the same Windows 10.0.26200, AMD64 Family 25 Model 33, 12-logical-processor host with MSVC 19.51.36256 x64 Release. The provider reads `GetSystemTimes` and `GlobalMemoryStatusEx`; timing includes both calls and writing the caller-owned raw snapshot.

One-second cadence measurements:

| Condition | Samples | Average | P95 | P99 | Maximum |
|---|---:|---:|---:|---:|---:|
| Ambient/reference comparison | 12 | 60.85 us | 222.60 us | 222.60 us | 222.60 us |
| Six-worker CPU load | 12 | 129.00 us | 187.90 us | 187.90 us | 187.90 us |
| Headless overhead run | 16 | 43.14 us | 107.20 us | 107.20 us | 107.20 us |

With only 12–16 observations, nearest-rank P95/P99 select the maximum or near-maximum; these are preliminary milestone figures, not stable distribution claims. The 256-entry diagnostic window is ready for longer V0.0.4 recorder runs.

Resource measurements over ten seconds after warm-up:

| Configuration | CPU, total machine capacity | Average working set | Maximum working set |
|---|---:|---:|---:|
| Headless provider + normalizer, one-second cadence | Below process CPU timer resolution (reported 0.000%) | 4.10 MiB | 4.10 MiB |
| Full native app, minimized after event-wait throttling | Below process CPU timer resolution (reported 0.000%) | 44.76 MiB | 45.01 MiB |

The headless process measurement isolates the current telemetry pipeline from SDL/ImGui. The minimized app continues collection but skips invisible rendering and waits up to 250 ms for SDL events. A zero reported CPU value means no increment was visible at the process timer resolution; it is not a claim of literally zero execution cost.

## V0.0.4 circular recorder baseline

Measured 2026-08-17 on the same Windows 10.0.26200, AMD64 Family 25 Model 33, 12-logical-processor host with MSVC 19.51.36256 x64 Release. `SystemSample` is 136 bytes on this build, so the default 300-sample ring payload is 40,800 bytes. The configuration cap of 86,400 samples bounds current system-sample payload to 11,750,400 bytes (about 11.2 MiB), excluding vector/mutex bookkeeping and snapshot copies.

Five trials performed 1,000,000 ring appends and 10,001 chronological snapshots of 300 samples. Timing summaries use the final bounded 256-observation diagnostic window and include clock measurement overhead.

| Operation | Median average | Median P95 | Median P99 | Median maximum |
|---|---:|---:|---:|---:|
| Append to full ring | 0.068 us | 0.100 us | 0.100 us | 0.100 us |
| Copy 300-sample snapshot | 6.524 us | 6.400 us | 12.500 us | 13.400 us |

A 16-second headless mock-provider run at the default one-second cadence collected 16 samples with zero dropped ticks, late starts, or deadline misses. Collection averaged 10.73 us with P95/P99/maximum 14.90 us. Scheduling jitter averaged 692.33 us with P95/P99/maximum 1,995.80 us. This is a short milestone validation, not the 30-minute release procedure.

The same headless run used 0.0080% total-machine CPU capacity, averaged 4.95 MiB working set (4.95 MiB maximum), and averaged 0.99 MiB private bytes. The Release desktop app, minimized while the collector continued, reported CPU below process timer resolution and averaged 45.87 MiB working set (45.98 MiB maximum). Normal collection has no file or SQLite write path; the headless target graph omits application, UI, storage dependencies, and analysis entirely.

## V0.0.5 Windows disk/network baseline

Measured 2026-08-17 on the same Windows 10.0.26200, AMD64 Family 25 Model 33, 12-logical-processor host with MSVC 19.51.36256 x64 Release. The provider now includes `GetSystemTimes`, `GlobalMemoryStatusEx`, one persistent PDH physical-disk query, `GetIfTable2`, lifecycle aggregation, and raw snapshot writes.

An as-fast-as-possible 10,000-sample run measured the final bounded 256 observations:

| Samples | Average | P95 | P99 | Maximum | P99 margin to 1 s |
|---:|---:|---:|---:|---:|---:|
| 10,000 | 1.230 ms | 1.578 ms | 1.808 ms | 2.136 ms | 99.819% |

All 9,999 post-baseline disk and network rates were available. A separate five-sample one-second-cadence check produced four available disk/network rates with provider average 1.418 ms and P99/maximum 1.579 ms. A six-second real-collector run recorded six samples, 1.613 ms collection P99, 99.839% margin to the next one-second deadline, and zero failures, deadline misses, or dropped ticks. These short hardware-sensitive runs demonstrate budget margin but do not replace the 30-minute V0.1 release procedure.

The comparison benchmark attempted balanced `IOCTL_DISK_PERFORMANCE` probes for physical drives 0-31. The ordinary-user process could open no usable devices on this host (`native_devices=0`); a failed enumeration cycle averaged 121.154 us and produced no comparable byte totals. PDH remained available without elevation. This privilege result, plus the native IOCTL's stateful enable/disable reference semantics, supports selecting the persistent PDH source. The native failure timing is not claimed as a successful-source cost comparison.

The controlled 32 MiB unbuffered/write-through file test observed 64.26 MiB of physical-disk writes (2.008x payload), illustrating expected filesystem/storage-layer amplification. The accepted automated range is 0.9x-4x to tolerate host metadata and concurrent activity while still detecting a missing or wildly duplicated source. A completed 64 MiB TCP loopback transfer stayed below the 32 MiB hardware-aggregate exclusion threshold. Both fixtures pass in full Release and UI/storage-disabled Release test graphs.

## V0.0.6 Windows process baseline

Measured 2026-08-17 on the same Windows 10.0.26200, AMD64 Family 25 Model 33, 12-logical-processor host with MSVC 19.51.36256 x64 Release. The host had 286 Tool Help/PSAPI process entries; 113 supported limited-query sampling and 173 protected/system entries were inaccessible to the ordinary-user process.

Enumeration comparison, 100 iterations:

| Source | Observed entries | Average | Decision |
|---|---:|---:|---|
| PSAPI `EnumProcesses` | 286 | 0.017 ms | Fastest, but PID only |
| PDH `Process(*)\\ID Process` | 287 instances | 2.265 ms | Instance identity/aggregate ambiguity |
| Tool Help process snapshot | 286 | 4.263 ms | Selected: PID, parent PID, and base name |

One Tool Help walk plus creation-time, CPU, working-set, and I/O queries for every accessible process averaged 6.233 ms with P95 6.798 ms and P99 7.787 ms. The active native cache held 113 identities. Executable-path work was included only in the first slow observation; normal observations reused metadata.

Synthetic scale measurements keep source data deterministic. The query cycle repeatedly opens the benchmark process with limited-query rights and performs the same three counter APIs; it excludes Tool Help enumeration and protected-process failures. Normalization includes identity lookup and four normalized fields.

| Process rows | Normalization cycle | Counter-query cycle | Query cost/row |
|---:|---:|---:|---:|
| 50 | 0.022 ms | 0.137 ms | 2.75 us |
| 200 | 0.084 ms | 0.652 ms | 3.26 us |
| 500 | 0.221 ms | 1.357 ms | 2.71 us |

The complete Release provider, sampled 1,000 times as fast as possible with 113 accessible processes, averaged 7.343 ms with P95 8.838 ms, P99 9.781 ms, and maximum 10.923 ms. A six-second real-collector run that included its initial slow metadata observation recorded collection P99 14.393 ms, leaving 98.561% of the one-second deadline, with zero failures, deadline misses, or dropped ticks.

`ProcessSample` is 80 bytes and `ProcessFrame` is 32 bytes on this build. Process history has a hard 600,000-entry budget (45.8 MiB of sample payload) in addition to the frame-count bound. This permits 2,000 rows in each default one-second/five-minute frame and 500 rows in each 250 ms/five-minute frame. Metadata caches are capped at 8,192 identities. A real three-second child fixture allocated 32 MiB, consumed CPU, continuously rewrote a 32 MiB file, produced nonzero normalized CPU/write rates, returned cached name/path metadata, and exited without making provider status fail.

## V0.0.7 incident snapshot baseline

Measured 2026-08-18 on the same Windows 10.0.26200, AMD64 Family 25 Model 33, 12-logical-processor host with MSVC 19.51.36256 x64 Release. `blackbox_incident_snapshot_benchmark` constructs a complete 150-frame incident from already-copied recorder snapshots. Each of ten iterations filters system/process times, converts explicit value states and units into the core incident domain, flattens process frames, deduplicates referenced identities, and copies only matching metadata. Source enumeration and SQLite are deliberately excluded.

| Processes/frame | Frames | Process rows | Average | P95 | P99 | Maximum |
|---:|---:|---:|---:|---:|---:|---:|
| 50 | 150 | 7,500 | 2.786 ms | 2.849 ms | 2.849 ms | 2.849 ms |
| 200 | 150 | 30,000 | 12.594 ms | 13.351 ms | 13.351 ms | 13.351 ms |
| 500 | 150 | 75,000 | 35.500 ms | 37.505 ms | 37.505 ms | 37.505 ms |

With ten observations, nearest-rank P95 and P99 both select the maximum. The 500-process result uses 3.75% of a one-second interval before provider cost, leaving ample milestone margin. A repeated-reserve implementation initially measured 148.284 ms maximum at 500 processes; reserving the selected row count once reduced that maximum to 37.505 ms. The optimized value is the milestone baseline.

`IncidentSystemSample` is 136 bytes and flattened `IncidentProcessSample` is 88 bytes on this build. The 500-process fixture therefore retains about 6.29 MiB of process-row payload per immutable incident, excluding vector and metadata allocation. A theoretical incident spanning the entire 600,000-entry process-history cap retains about 50.35 MiB of flattened row payload. The two-item FIFO consequently bounds retained process-row payload at about 100.7 MiB even if overlapping triggers extend both incidents to full history. Normal rolling telemetry still allocates no incident payload before a trigger.

A separate mock-collector run used a deliberately aggressive 2 ms cadence, 100 ms ring, 50 ms pre-window, and 20 ms post-window. Snapshot construction took 64.0 us; scheduling jitter P95/P99 was 1.044/1.347 ms with zero deadline misses and zero dropped ticks. This small one-process run validates that capture completion and enqueue do not stop subsequent collection. The scale table above represents construction cost at realistic process cardinalities; a full one-second native-provider capture remains part of the V0.1 soak procedure.

## V0.0.8 SQLite persistence baseline

Measured 2026-08-18 on the same Windows 10.0.26200, AMD64 Family 25 Model 33, 12-logical-processor host with MSVC 19.51.36256 x64 Release and SQLite 3.53.1. `blackbox_storage_benchmark` creates fresh WAL/`synchronous=FULL` archives and stores synthetic 150-frame incidents. Each scale ran five independent whole-incident transactions; with five values, nearest-rank P95 and P99 both select the maximum.

| Processes/frame | Process rows | Average write | P95/P99/max | Logical DB size | Max temporary working-set delta |
|---:|---:|---:|---:|---:|---:|
| 50 | 7,500 | 27.870 ms | 29.020 ms | 0.85 MiB | 1.27 MiB |
| 200 | 30,000 | 141.451 ms | 165.385 ms | 3.20 MiB | 2.56 MiB |
| 500 | 75,000 | 373.351 ms | 390.383 ms | 8.22 MiB | 3.13 MiB |

All 15 incidents committed successfully. Temporary memory is the largest process working-set increase observed from immediately before `store` through completion, sampled every 250 us; it is a coarse process-wide peak, not an allocator-only measurement. Logical database size is `page_count * page_size` and excludes transient WAL/shared-memory sidecars.

The asynchronous check used the real SQLite writer and mock collector at an intentionally aggressive 2 ms cadence. Its single incident write took 2.297 ms; collector scheduling-jitter P99 was 1.522 ms with zero deadline misses and zero dropped ticks. The archive's allocated logical size was 73,728 bytes both before and after this small capture because existing schema pages had room for the rows. A separate 25-tick integration test instrumented the archive boundary and observed exactly zero store attempts without an incident trigger, verifying that normal rolling samples do not enter the database path.

## V0.0.9 incident viewer baseline

Measured 2026-08-18 on the same Windows 10.0.26200, AMD64 Family 25 Model 33, 12-logical-processor host with MSVC 19.51.36256 x64 Release and SQLite 3.53.1. `blackbox_viewer_benchmark` used an in-memory schema-v1 archive containing 200 representative incidents and one 150-frame/500-process incident with 75,000 process rows.

| Operation | Trials | Average | P95 | P99 | Maximum |
|---|---:|---:|---:|---:|---:|
| List 50 of 201 incidents | 100 | 0.074 ms | 0.143 ms | 0.171 ms | 0.184 ms |
| Search 20 matching labels/notes | 100 | 0.230 ms | 0.387 ms | 0.403 ms | 0.411 ms |
| Load incident with 75,000 process rows | 5 | 65.479 ms | 73.459 ms | 73.459 ms | 73.459 ms |
| Build 50-process view model | 5 | 1.261 ms | 1.497 ms | 1.497 ms | 1.497 ms |
| Build 200-process view model | 5 | 5.240 ms | 6.314 ms | 6.314 ms | 6.314 ms |
| Build 500-process view model | 5 | 12.296 ms | 13.883 ms | 13.883 ms | 13.883 ms |
| ImGui frame, 500-process detail | 100 | 0.120 ms | 0.144 ms | 0.171 ms | 0.187 ms |

The 500-process view-model build's maximum sampled temporary process working-set increase was 143,360 bytes (0.137 MiB), sampled every 250 us after the source incident was already resident. The frame measurement covers CPU-side ImGui/ImPlot command generation for the complete dashboard and viewer at 1920x1080; it excludes SDL renderer submission, GPU work, and presentation. Timeline series are capped at 2,048 min/max-envelope points and the visible process table at 500 identities.

All SQLite and large view-model operations run on the viewer worker, so the 73.459 ms worst measured load is not render-loop time. A failure-injected 75 ms viewer query ran concurrently with a 1 ms mock collector; collection advanced by at least 20 samples with zero failed samples. Actual ImGui smoke tests render both representative and 75,000-process-row database fixtures.

## V0.1 Windows release profile

Measured 2026-08-18 on Windows build 10.0.26200, AMD64 Family 25 Model 33, 12 logical
processors, balanced power mode, MSVC 19.51.36256 x64 Release, default one-second/five-minute
configuration, and 117 accessible processes. Both profiles ran for 65 seconds after warm-up as an
ordinary token (`elevated=0`). These are V0.1 engineering-flight measurements, not the three
30-minute controlled repetitions required for a statistically stable hardware baseline.

| Mode | CPU, total-machine capacity | Average/max working set | Average/max private bytes |
|---|---:|---:|---:|
| Headless real Windows recorder | 0.130% | 14.42 / 15.14 MiB | 6.87 / 7.62 MiB |
| Packaged app, minimized | 0.042% | 52.41 / 53.34 MiB | 56.26 / 57.21 MiB |

The packaged executable launched from the generated ZIP against an isolated `%LOCALAPPDATA%`,
created a schema-v1 77,824-byte empty archive, recorded while minimized, accepted `WM_CLOSE`,
drained, and exited with code zero. Its working set is 2.41 MiB above the upper edge of the initial
30-50 MiB target; this is a visible follow-up target, not hidden as a pass. CPU is below the 0.5%
idle goal and the 1% typical-recording goal. The headless process reported 30 process-write bytes,
all from its diagnostic console log; the target graph contains no storage or file-writing target.
The automated no-continuous-writes integration test observed zero archive calls.

| Collector measure | Average | P95 | P99 | Maximum/other |
|---|---:|---:|---:|---:|
| Collection latency | 9.720 ms | 11.264 ms | 13.546 ms | 98.645% P99 deadline margin |
| Scheduling jitter | 0.664 ms | 1.310 ms | 1.514 ms | bounded 256-value window |
| Samples | 66 | - | - | 0 failed, 0 deadline misses, 0 dropped |

The collector profile had no resume event. Resume behavior is tested deterministically at the
five-second boundary: gaps reset both delta normalizers, restart cadence at wake, refresh slow
metadata, and are excluded from ordinary jitter while retaining separate skipped/event diagnostics.
The accelerated concurrency soak collected 2,000 one-millisecond samples through 400 ring wraps
while another thread continuously requested bounded snapshots. A separate writer test persisted
100 sequential accepted captures with a two-item queue bound. Provider exceptions, partial/lost
capabilities, SQLite busy/full/corrupt/path failures, viewer failure, writer drain/cancel, transient
writer recovery, and startup/package failure paths are automated.

The Release storage benchmark was repeated for V0.1 with SQLite 3.53.1, five independent
transactions per scale:

| Processes/frame | Rows | Average | P95/P99/max | Logical DB size |
|---:|---:|---:|---:|---:|
| 50 | 7,500 | 26.858 ms | 31.275 ms | 0.85 MiB |
| 200 | 30,000 | 107.753 ms | 113.207 ms | 3.21 MiB |
| 500 | 75,000 | 321.969 ms | 328.827 ms | 8.22 MiB |

The generated three-incident representative archive (explicit-unavailable, 50-process, and
500-process fixtures) is 9,728,000 bytes (9.28 MiB). The asynchronous capture check recorded
1.921 ms writer P99, 1.725 ms collector-jitter P99, zero deadline misses/drops, and no logical
database growth because the small incident fit existing pages.

Against the initial targets: CPU, dropped samples, one-second cadence, five-minute history, bounded
memory growth, and RAM-only normal telemetry pass. Full-app working set is slightly above target and
remains the only measured V0.1 target miss. Longer three-run profiling and a clean stable Windows
support matrix remain release-process improvements; they do not change collection correctness.

## V0.2 statistical analysis baseline

Measured 2026-08-18 on the same Windows build 10.0.26200, AMD64 Family 25 Model 33, 12-logical-
processor host with MSVC 19.51.36256 x64 Release. Each fixture contains 150 one-second frames and
stable normal system/process values. Ten complete analyses include candidate selection, rolling
robust baseline construction, six system metrics, four metrics per selected process, deterministic
ranking, and result allocation.

| Processes/frame | Process rows | Average | P95/P99/max | Peak temporary working-set delta | Maximum normal score |
|---:|---:|---:|---:|---:|---:|
| 50 | 7,500 | 4.523 ms | 5.125 ms | 0.50 MiB | 0.000 |
| 200 | 30,000 | 20.731 ms | 23.102 ms | 1.69 MiB | 0.000 |
| 500 | 75,000 | 56.637 ms | 62.965 ms | 4.88 MiB | 0.000 |

Peak working set is sampled every 250 us from immediately before analysis through result creation;
it is process-wide and coarse. With ten trials, nearest-rank P95/P99 both select the maximum. All
30 analyses succeeded. Normal fixtures produced no false positive above the 3.5 robust-z threshold.
Separate deterministic fixtures inject CPU, memory, disk, network, and process-CPU spikes; each
injected resource or full process identity ranks first with score above 0.99 and high confidence.

The 500-process P99 remains below the prior 73.459 ms SQLite load and far from the one-second sample
interval. Both operations run sequentially on the viewer worker, never the collector. An integration
test analyzes that 75,000-row fixture while a 1 ms mock collector advances at least ten samples with
zero collection failures. The analysis-disabled target graph builds/tests independently, proving
that V0.2 adds zero collection-path CPU, memory, disk I/O, or dependency overhead when omitted.

## V0.3 personalized profile baseline

Measured 2026-08-18 on the same Windows/MSVC Release host. Each scale uses ten distinct incident
updates for the same executable set, then queries all eligible history for a later incident. Times
include validation, deterministic ordering, prepared SQLite work, bounded trimming, result
allocation, and `FULL`-synchronous transaction commit. Logical size includes the schema, 20 small
incident fixtures, executable identities, and ten observations per identity.

| Executables | Update average | Update P95/max | Query average | Query P95/max | Logical DB size |
|---:|---:|---:|---:|---:|---:|
| 50 | 2.643 ms | 3.168 ms | 0.677 ms | 1.097 ms | 0.24 MiB |
| 200 | 5.404 ms | 6.996 ms | 2.456 ms | 4.756 ms | 0.69 MiB |
| 500 | 8.348 ms | 10.605 ms | 6.221 ms | 11.391 ms | 1.59 MiB |

All 30 update/query trials succeeded. Final row counts were exactly 10 observations per executable:
500/2,000/5,000 observations. A separate cardinality soak exceeds 2,048 executable keys and 64
observations for one key, then verifies both limits and the global 131,072-observation ceiling.
Profile operations run only on the viewer worker after incident persistence, so these costs add no
normal-recording CPU, memory, or disk I/O.

## V0.4 automatic detector baseline

Measured 2026-08-18 on the same Windows/MSVC x64 Release host with
`blackbox_automatic_detector_benchmark`. Each trial sends 2,000,000 normal, fully available system
samples through the same virtual `observe` call used by the collector. The detector updates four
fixed rolling baselines, evaluates thresholds/statistics, and allocates no per-sample storage.

| Trial | Nanoseconds per collector sample |
|---:|---:|
| 1 | 270.832 |
| 2 | 279.181 |
| 3 | 267.028 |
| 4 | 279.205 |
| 5 | 284.518 |
| **Median** | **279.181** |

Range: 267.028-284.518 ns/sample, with zero triggers. At the default one-second cadence the median
is about 0.000028% of one logical processor between samples; provider collection remains orders of
magnitude larger. The live detector object uses four 60-double arrays (1,920 bytes of value storage)
plus fixed bookkeeping.

The deterministic false-positive budget is zero automatic incidents across an 86,400-observation
smooth normal fixture (one day at the default cadence); the test meets it. Hard/statistical fixtures
for CPU, memory, disk, and network trigger on the third qualifying observation. A 1,000-sample
continuous severe storm emits at most nine triggers under the 120-second cooldown. Every emitted
request still passes through the two-slot source FIFO; the writer can retain only one additional
in-flight snapshot, and SQLite refuses growth past its configured 1 GiB logical cap.

## V0.7 recurring discovery baseline

Measured 2026-08-19 on the Windows/MSVC x64 Release host with
`blackbox_incident_clustering_benchmark`. Twenty trials use eight deterministic shape families,
version-1 twelve-dimensional inputs, complete-link threshold grouping, shared-characteristic
construction, and stable result comparison.

| Incidents | Average | P95 | Maximum | Input object payload | Peak temporary working-set delta | Stable |
|---:|---:|---:|---:|---:|---:|---:|
| 32 | 0.078 ms | 0.100 ms | 0.124 ms | 5,376 B | 143,360 B | 20/20 |
| 128 | 0.526 ms | 0.589 ms | 0.785 ms | 21,504 B | 20,480 B | 20/20 |
| 512 | 7.240 ms | 8.007 ms | 11.538 ms | 86,016 B | 16,384 B | 20/20 |

Working-set deltas are process-wide/page-granular and therefore noisy; the input object payload
shows linear growth. The implementation allocates no pairwise distance matrix and refuses work
beyond the newest 512 valid inputs. Cached viewer refreshes recompute zero unchanged features.
Deterministic CPU and disk families remain separate, a balanced singleton remains noise, and input
reversal produces the identical 512-incident result.

## V0.10 pre-release engineering baseline

Measured 2026-08-19 on Windows build 10.0.26200, AMD64 Family 25 Model 33, 12 logical processors,
MSVC 19.51/Visual Studio 2026 x64 Release. These controlled results qualify this host and provide
regression evidence; the clean Windows 10/11 client runs in `RELEASE_READINESS.md` remain official
publishing gates.

A 30.023-second ordinary-user native headless run at the conservative one-second profile observed
138 active processes and 31 collections. It used 0.134448% of total machine CPU capacity, averaged
14,985,154 bytes working set (15,351,808 maximum), and averaged 6,923,582 private bytes (7,307,264
maximum). Collection average/P95/P99 was 8.927/9.754/13.755 ms; scheduling jitter
average/P95/P99 was 0.734/1.307/1.522 ms. There were zero deadline misses, drops, failed samples, or
resume events. Process write-transfer growth was 30 bytes from benchmark/log output; the collector
has no archive or telemetry-file write path.

One million fixed-ring appends averaged 0.058 microseconds. Ten thousand 300-sample snapshots
averaged 6.37 microseconds with 13.2 microseconds P99. A separate deliberately unsupported 1 ms
scheduler stress collected 1,712 samples in two seconds and recorded 286 misses/drops; it validates
observable overload accounting and is not a supported user profile. The seven-day accelerated
default-cadence test performed 604,800 appends with exactly 300 retained frames and 604,500
overwrites. The matching smooth detector fixture emitted zero automatic captures across all seven
days.

Incident snapshot construction and SQLite persistence remained below the V1 gates:

| Processes | Snapshot avg/P99 | SQLite avg/P99 | Database size | SQLite peak temporary memory |
|---:|---:|---:|---:|---:|
| 50 | 2.809/3.165 ms | 25.865/26.675 ms | 950,272 B | 1,335,296 B |
| 200 | 12.512/13.363 ms | 106.006/109.172 ms | 3,420,160 B | 2,813,952 B |
| 500 | 33.601/34.257 ms | 312.013/315.641 ms | 8,679,424 B | 3,506,176 B |

The asynchronous persistence check measured 2.263 ms writer P99 and 1.647 ms collector-jitter P99
with zero misses/drops and no logical database growth for the page-fitting capture. Viewer results
were 0.073 ms average for a 50-of-201 list, 0.222 ms search, 49.206/50.701 ms average/P99 for a
75,000-row load, 10.059/10.386 ms to build the 500-process model, and 0.098/0.159 ms for the
500-process primitive frame. Peak temporary model memory was 126,976 bytes.

The intelligent pipeline diagnosed 4/4 controlled resource fixtures with zero quiet-fixture
diagnoses. For 50/200/500 processes its average was 4.927/22.104/58.571 ms, P95
4.996/22.472/60.489 ms, and peak temporary memory 483,328/1,806,336/4,530,176 bytes. All repeated
results were deterministic. Native ML remains absent. The full Release executable is 10,160,640
bytes; the analysis-disabled full app is 1,115,136 bytes, confirming optional removal.

## V0.11 background-shell baseline

Measured 2026-08-20 on Windows build 10.0.26200, AMD64 Family 25 Model 33, 12 logical processors,
MSVC 19.51/Visual Studio 2026 x64 Release, ordinary user, balanced power mode, and the conservative
one-second/five-minute recorder profile. Each 30-second measurement followed five seconds of
warm-up and sampled memory every 250 ms. Both used isolated `%LOCALAPPDATA%` roots and exited
through the application lifecycle with code zero.

| Background mode | CPU, total-machine capacity | Average/max working set | Average/max private bytes |
|---|---:|---:|---:|
| Hidden from startup | 0.133784% | 49.034 / 49.590 MiB | 57.224 / 57.770 MiB |
| Minimized after UI warm-up | 0.133812% | 56.086 / 56.426 MiB | 62.767 / 63.059 MiB |

V0.11 publishes controlled-host background bounds of less than 0.5% total-machine CPU, at most
60 MiB working set, and at most 65 MiB private bytes for both states. The results pass those bounds
and the future V1 80 MiB gate. The hidden-start working set also meets the early aspirational
30-50 MiB range; the warm/minimized UI does not, so reducing retained SDL/ImGui allocations remains
an optimization opportunity rather than being reported as a pass against that older goal.

The hidden and minimized paths execute the same branch: they perform no dashboard snapshot,
process-table copy, ImGui frame construction, renderer submission, or presentation. The UI thread
uses a 250 ms bounded event wait, so the application schedules at most four idle UI timeout
opportunities per second; the default collector schedules one sample per second. The tray,
incident-writer, and viewer threads block on messages or condition variables rather than polling.
The UI timeout refresh can call `set_status` four times per second, but unchanged finite shell state
produces no native message; only recording/capture/pause/retry/error transitions wake the tray thread.
Thus the published default scheduling bound is five application-owned timed wake opportunities per
second, excluding OS delivery and the work performed inside the one collector observation. Faster
user-selected recorder profiles raise only the collector portion to two or four samples per second.

## V0.13 storage/network forensic-signal baseline

Measured 2026-08-20 on Windows build 10.0.26200, AMD64 Family 25 Model 33, 12 logical processors,
MSVC 19.51/Visual Studio 2026 x64 Release, ordinary user (`elevated=0`), and the conservative
one-second recorder profile. This run includes the separate physical-disk quality PDH query,
interface/connectivity state, and IPv4+IPv6 TCP statistics.

A 30.016-second native headless run recorded 31/31 samples with zero failures, deadline misses,
drops, or resume events while observing 140 active processes. It used 0.134475% total-machine CPU,
averaged 14,666,063 bytes working set (14,983,168 maximum), and averaged 7,613,642 private bytes
(8,032,256 maximum). Collection average/P95/P99 was 9.150/9.676/13.754 ms; scheduling jitter
average/P95/P99 was 0.653/1.141/1.515 ms. The 30 process write-transfer bytes are benchmark/logger
output; no archive was attached to the collector.

A bounded 100-sample zero-delay provider microbenchmark averaged 8.079 ms with 9.102 ms P95,
10.757 ms P99, and 13.706 ms maximum. This stress rate is not a supported cadence; the measured
one-second run above is the user profile qualification. The two-million-observation detector soak
emitted zero captures for the smooth quiet fixture and averaged 309.778 ns per collector sample.

The native workload suite passed as an ordinary user: a 32 MiB unbuffered/write-through file was
observed inside the physical-layer amplification bound and returned finite, nonnegative read/write
latency, service time, queue depth, and device identity; the 64 MiB TCP loopback control remained
excluded from hardware throughput while passive connectivity/TCP counters remained available.
Real adapter disable/enable was intentionally not automated because it would disrupt the user's
network; bounded lifecycle fixtures cover arrival, removal, reappearance, counter reset, and state
transition instead. Long wall-clock multi-adapter/sleep churn remains a V0.17 release gate.

## V0.14 GPU/responsiveness/power/event baseline

Measured 2026-08-20 on Windows build 10.0.26200, AMD64 Family 25 Model 33, 12 logical processors,
MSVC 19.51/Visual Studio 2026 x64 Release, ordinary user (`elevated=0`), and the conservative
one-second recorder profile. The run includes persistent GPU/DPC/ISR quality counters, CPU
frequency/thermal-limit context, power/battery/uptime gauges, and the prior storage/network sources.

A 30.020-second native headless run recorded 31/31 samples with zero failures, deadline misses,
drops, or resume events while observing 167 active processes. It used 0.065061% total-machine CPU,
averaged 16,214,680 bytes working set (17,076,224 maximum), and averaged 8,887,278 private bytes
(9,818,112 maximum). Collection average/P95/P99 was 10.390/11.731/15.473 ms; scheduling jitter
average/P95/P99 was 0.596/1.291/1.364 ms. The 30 process write-transfer bytes are benchmark/logger
output; no archive was attached to the collector.

The event-provider benchmark enabled exactly one source per run for two seconds and polled it 40
times. The fully disabled baseline, power, device, audio, service, Defender, Windows Update, and
application-hang runs all started and polled successfully with zero native drops. Idle CPU rounded
to zero for every run except the selected-service subscription, which measured 0.064595% of
total-machine capacity. No event happened during these quiet windows; deterministic callback,
overflow, privacy, filter, recovery, and reconfiguration tests provide the event-content coverage.

The full Release, analysis-disabled, and fully headless graphs were qualified serially because the
unbuffered-disk workload and deliberate 1 ms scheduling stress are hardware-sensitive and contend
when separate build matrices execute simultaneously. Their serial results pass; this is a test-run
isolation requirement, not a supported 1 ms product cadence. Long wall-clock sleep/device/audio
churn and representative game/UI/audio incidents remain V0.15/V0.17 evidence gates.
