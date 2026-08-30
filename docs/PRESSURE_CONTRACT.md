# Portable responsiveness and resource-pressure contract

## Purpose

BlackBox needs cross-platform evidence for periods where runnable work is delayed even when ordinary
utilization counters do not explain the delay. This document fixes the semantic boundary before any
Linux PSI or future macOS source enters telemetry or schema V1.

Pressure is not an alias for utilization, latency, responsiveness, or Windows DPC/ISR activity. A
provider must expose only the dimensions its native source measures and must leave every other
dimension explicitly unsupported.

## Source-neutral dimensions

The future portable observation is a group of independent dimensions:

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

macOS currently has no accepted public source that exposes the cumulative stalled-time meanings above.
Memory-pressure notifications or levels describe state transitions, not stalled duration, so they
remain a possible separate event/state contract rather than a fabricated PSI equivalent.

## Privacy, persistence, and analysis

Pressure evidence contains only machine-wide durations/fractions and availability states. It contains
no PID, process name, cgroup, container, path, device, or workload identity. Per-cgroup PSI is out of
scope for V1.

When implemented, the fields enter the direct schema-V1 snapshot and incident models once; there is no
migration, legacy reader, dual writer, or compatibility branch. Analysis may cite pressure as observed
context but must calibrate any causal claim against representative held-out incidents. Runtime ML does
not gain access merely because the feature exists.

## Implementation gate

Linux PSI implementation may begin only with parser tests for missing/duplicate/unknown records,
numeric bounds, malformed input, counter reset, warm-up, partial dimensions, and suspend/resume. A
provider contract test, overhead benchmark, incident round-trip test, privacy review, UI availability
copy, and exact-revision Linux hosted evidence are required before the capability is described as
implemented. A macOS implementation requires a separately documented public source with the same
semantics; adjacent memory-pressure APIs do not satisfy that gate.
