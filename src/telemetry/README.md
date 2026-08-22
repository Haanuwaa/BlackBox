# Telemetry module

The platform-independent V0.0.2 domain is defined in `types.hpp`, `provider.hpp`, and `normalizer.hpp`. It contains no operating-system or UI dependencies. `mock/` provides deterministic development scenarios using the same provider contract.

Real operating-system implementations belong only in `windows/`, `linux/`, or `macos/`. V0.0.3 implements Windows CPU and physical-memory collection in `windows/`; unsupported metrics remain explicit through provider capabilities and metric availability. Providers write into caller-owned `RawTelemetrySnapshot` buffers so vector capacity can be reused; normalized scalar paths are allocation-free.

V0.0.4 adds `TelemetryCollector`, the platform-independent provider/normalizer scheduling loop. It writes normalized samples to the fixed-capacity recorder in `core`, exposes bounded immutable snapshots and diagnostics, and owns no UI, storage, analysis, or operating-system dependency.

V0.0.5 adds Windows physical-disk throughput through a persistent PDH query and physical-network throughput through `GetIfTable2`. An allocation-free per-entity lifecycle tracker produces synthetic cumulative totals without arrival/removal/reset spikes before the portable normalizer calculates measured-time rates. Selection policy, privilege behavior, controlled workload tolerance, and cost are published in `docs/TELEMETRY.md` and `docs/PERFORMANCE.md`.

V0.0.6 adds lifecycle-safe Windows process telemetry. Tool Help enumeration and limited-query Win32 calls emit cumulative per-identity counters; portable normalization produces total-machine CPU and I/O rates. A parallel bounded process-frame recorder and retention-bounded metadata cache keep process history separate from the system ring, while executable paths run only on the slow metadata tier.

V0.0.7 lets the collector complete a pending manual capture after its post-window. `incident_snapshot_builder` copies only the bounded newest frames that can intersect the window, filters by observation time, converts normalized values into the core incident domain, flattens process frames, and preserves only referenced metadata. Snapshot cost is included in collector timing; construction failure is observable and never terminates sampling.

V0.4 adds an optional `AutomaticIncidentDetector` over normalized system samples. It uses fixed
rolling storage, conservative threshold/statistical confirmation, and cooldown deduplication, then
submits a core capture trigger to the same bounded coordinator as manual capture. It has no analysis,
storage, UI, platform, or OS dependency and is removed when
`BLACKBOX_ENABLE_AUTOMATIC_DETECTION=OFF`.
