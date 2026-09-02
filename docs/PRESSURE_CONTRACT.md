# Portable responsiveness and resource-pressure contract

## Purpose

BlackBox needs cross-platform evidence for periods where runnable work is delayed even when ordinary
utilization counters do not explain the delay. This document fixes the semantic boundary used by the
implemented Linux PSI evidence and by any future native source entering direct schema V1.

Pressure is not an alias for utilization, latency, responsiveness, or Windows DPC/ISR activity. A
provider must expose only the dimensions its native source measures and must leave every other
dimension explicitly unsupported.

## Source-neutral dimensions

The portable observation is a group of independent dimensions:

- `cpu_some`: time in which at least one runnable non-idle task was delayed for CPU service.
- `memory_some`: time in which at least one non-idle task was delayed by memory reclaim or allocation.
- `memory_full`: time in which every non-idle task was delayed by memory pressure.
- `io_some`: time in which at least one non-idle task was delayed by I/O.
- `io_full`: time in which every non-idle task was delayed by I/O.

Each available dimension consists of a monotonic cumulative stalled duration from the native source.
The portable normalizer may derive an interval fraction only from two valid observations:

`interval_pressure = delta(stalled_duration) / delta(monotonic_observation_time)`

The result is clamped only for floating-point tolerance, not to conceal invalid native data. Counter
regression, duplicate observation time, overflow, source replacement, suspend/resume, or provider
restart resets that dimension to warming up. No moving-average field from a native API is persisted as
if it were an exact interval measurement.

## Availability and capability rules

- Every dimension has its own `MetricStatus`; partial native support is valid.
- A capability means the provider implements the exact cumulative-stall semantics above. File or API
  presence alone is not capability evidence.
- Permission denial is `inaccessible`; transient read/parse/reset conditions are
  `temporarily_unavailable`; a missing native semantic is `unsupported`.
- Zero is available evidence only after a successful native read and a valid delta. It is never a
  substitute for unavailable data.
- Collection is bounded, allocation-free after initialization, nonblocking, and performed through the
  platform telemetry provider. No pressure source may use the independent system-event thread.

## Platform mapping

Linux `/proc/pressure/{cpu,memory,io}` cumulative `total` values can implement the dimensions whose
files and records are present. PSI `some` and `full` stay distinct. The `avg10`, `avg60`, and `avg300`
fields are diagnostic source data, not the portable interval contract.

Windows DPC time, interrupt time, and DPC rate remain Windows responsiveness evidence under the
existing `dpc_isr` capability. They do not measure runnable-task stall time and must not populate any
pressure dimension.

macOS has no accepted public source that exposes the cumulative stalled-time meanings above.
`NSProcessInfo.thermalState` is therefore represented as a separate coarse
nominal/fair/serious/critical thermal state. Dispatch memory-pressure notifications are represented
by a second, separate normal/warning/critical memory-pressure state. Neither state is converted into
a stall fraction, CPU frequency, utilization value, or the other state. Memory pressure starts
temporarily unavailable until the first native transition is observed.

## Privacy, persistence, and analysis

Pressure evidence contains only machine-wide durations/fractions and availability states. It contains
no PID, process name, cgroup, container, path, device, or workload identity. Per-cgroup PSI is out of
scope for V1.

The fields enter the direct schema-V1 snapshot and incident models once; there is no migration,
legacy reader, dual writer, or compatibility branch. Analysis may cite pressure as observed
context but must calibrate any causal claim against representative held-out incidents. Runtime ML does
not gain access merely because the feature exists.

## Implementation evidence

Linux uses strict bounded parsers for missing/duplicate/unknown records, numeric bounds, malformed
input, overflow, partial dimensions, and `some`-only CPU files. Normalizer tests cover exact deltas,
warm-up, independent reset/failure, and impossible fractions; provider-contract, direct-V1 archive
round-trip, dataset/truth export, UI availability copy, and the existing all-tier overhead benchmark
cover the remaining boundaries. The native parser is also part of the bounded libFuzzer graph.
The V0.22 Linux implementation has exact-revision hosted evidence. A future macOS cumulative-stall
implementation still requires a separately documented public source with the same semantics; the
implemented thermal and memory-pressure levels do not satisfy that contract.
