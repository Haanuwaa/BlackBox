# Telemetry specification

This matrix separates portable domain readiness from real collection. Windows sources marked **candidate** must be validated for accuracy, cost, supported versions, counter semantics, and behavior without administrator privileges. V0.0.2 defines normalized and mock data; it does not implement a Windows API source.

## Portable contract

- `MetricValue<T>` carries `available`, `unsupported`, `inaccessible`, or `temporarily_unavailable`. An unavailable default value is never a measured zero.
- `Ratio` is a dimensionless fraction; UI code multiplies by 100 only for display.
- `ByteCount` is bytes. `BytesPerSecond` is a double-precision rate calculated from the measured monotonic interval.
- Raw CPU contains cumulative busy and total ticks in the same provider-defined unit. Normalized CPU is `busy_delta / total_delta`, representing total machine-capacity utilization in `[0,1]`.
- Future process CPU uses the same total-machine convention: `1.0` means all logical machine capacity, not one fully occupied logical processor.
- `ProcessIdentity` is `(PID, creation_token)`. A PID alone is not a durable identity.
- Providers receive a tier set and caller-owned destination. Snapshot vector capacity is retained for reuse; tier selection does not prescribe scheduling frequency.
- `ProviderSampleResult` describes overall collection, while each metric preserves its own availability state.

| Metric | Internal representation | Units | Tier / initial frequency | Windows source/API | Privilege | Normalization | Expected cost | Linux source | macOS source | Status |
|---|---|---:|---|---|---|---|---|---|---|---|
| Observation time | `MonotonicTimePoint` plus future wall-clock correlation | monotonic duration | Fast / 1 Hz | C++ monotonic clock contract; wall correlation TBD | None | None; monotonic time drives deltas | Negligible | C++ clocks | C++ clocks | Portable contract implemented V0.0.2 |
| Total CPU | `MetricValue<Ratio>` | fraction `[0,1]` | Fast / 1 Hz | `GetSystemTimes` selected | None | Busy tick delta / total tick delta; Windows kernel includes idle | Very low; measured in provider benchmark | `/proc/stat` | Mach host CPU ticks | Windows/Linux/macOS implemented; Windows >64-LP limitation |
| RAM used | `MetricValue<ByteCount>` | bytes | Normal / 1 Hz | `GlobalMemoryStatusEx` selected | None | `total - available`, with consistency guard | Very low; measured with CPU call | `/proc/meminfo` | Mach VM statistics plus `hw.memsize` | Windows/Linux/macOS implemented |
| RAM total | `MetricValue<ByteCount>` | bytes | Normal / 1 Hz | `GlobalMemoryStatusEx` selected | None | Gauge | Very low; measured with CPU call | `/proc/meminfo` | `sysctlbyname("hw.memsize")` | Windows/Linux/macOS implemented |
| RAM utilization | `MetricValue<Ratio>` | fraction `[0,1]` | Normal / 1 Hz | Derived | None | `used / total`, with zero guard | Negligible | Derived | Derived | Domain/normalization implemented V0.0.2 |
| Disk read throughput | `MetricValue<BytesPerSecond>` | bytes/s | Fast / 1 Hz | PDH `PhysicalDisk(*)\\Disk Read Bytes/sec` selected | None observed | Per-instance lifecycle-safe cumulative aggregation, then measured-time delta | Low; measured in provider benchmark | `/proc/diskstats` | IOKit statistics candidate | Windows implemented V0.0.5 |
| Disk write throughput | `MetricValue<BytesPerSecond>` | bytes/s | Fast / 1 Hz | PDH `PhysicalDisk(*)\\Disk Write Bytes/sec` selected | None observed | Same as read | Low; measured in provider benchmark | `/proc/diskstats` | IOKit statistics candidate | Windows implemented V0.0.5 |
| Disk latency/service time/queue | `MetricValue<Seconds>` / `MetricValue<double>` | seconds/op, requests | Fast / 1 Hz | Persistent PDH `PhysicalDisk(*)` formatted counters | None observed | Maximum valid non-`_Total` physical instance; gauge | Low; qualified with provider | `/proc/diskstats` candidate | IOKit candidate | Windows implemented V0.13 |
| Network receive | `MetricValue<BytesPerSecond>` | bytes/s | Fast / 1 Hz | `GetIfTable2` `InOctets` selected | None observed | Per-interface lifecycle-safe cumulative aggregation, then measured-time delta | Low; measured with provider | `/proc/net/dev` or rtnetlink | `getifaddrs` + routing/sysctl counters candidate | Windows implemented V0.0.5 |
| Network transmit | `MetricValue<BytesPerSecond>` | bytes/s | Fast / 1 Hz | `GetIfTable2` `OutOctets` selected | None observed | Same as receive | Low; measured with provider | `/proc/net/dev` or rtnetlink | Network interface counters candidate | Windows implemented V0.0.5 |
| Network connectivity/transitions | typed level plus interval count | level, events | Fast / 1 Hz | `GetNetworkConnectivityHint` + `GetIfTable2` | None observed | Aggregate hint and bounded lifecycle delta | Low | Candidate | Candidate | Windows implemented V0.13 |
| TCP retransmission/failure/reset | ratio plus interval counts | fraction, events | Fast / 1 Hz | `GetTcpStatisticsEx` IPv4+IPv6 | None observed | Counter deltas; retransmission ratio requires 8 segments | Low | Candidate | Candidate | Windows implemented V0.13 |
| Process identity | `ProcessIdentity` (PID + creation token) | opaque | Normal / 1 Hz | `EnumProcesses` plus cached `GetProcessTimes` creation `FILETIME` | Protected processes may be inaccessible | PID paired with creation time; new identity always warms up | Medium; measured | `/proc/<pid>/stat` start time | `proc_pid_rusage` start time | Windows/Linux/macOS implemented |
| Parent PID | `MetricValue<ProcessId>` | opaque | Normal / lifecycle | Slow-tier Tool Help metadata snapshot | Enumeration itself needs no elevation | Cached snapshot value; parent identity is not inferred | Slow-tier only | `/proc/<pid>/stat` | `proc_pidinfo` BSD info | Windows/Linux/macOS implemented |
| Process name | cached UTF-8 string | text | Normal / creation | Slow-tier `PROCESSENTRY32W.szExeFile` | Gaps only when identity cannot be opened | UTF-16 to UTF-8 once per identity | Low when cached | `/proc/<pid>/comm` | `proc_name` | Windows/Linux/macOS implemented |
| Executable path | cached UTF-8 string / unavailable | text | Slow / 30 s and creation lifecycle | `QueryFullProcessImageNameW` selected | Access may be denied | Resolve on slow tier; unavailable remains explicit | Bounded slow-tier work | `/proc/<pid>/exe` | `proc_pidpath` | Windows/Linux/macOS implemented |
| Process CPU | `MetricValue<Ratio>` | fraction of total machine capacity | Normal / 1 Hz | `GetProcessTimes` selected | Limited-query access may fail | CPU-time delta / measured wall interval / active logical CPU count | Medium across all accessible processes | `/proc/<pid>/stat` | `proc_pid_rusage` | Windows/Linux/macOS implemented |
| Process working set | `MetricValue<ByteCount>` | bytes | Normal / 1 Hz | `GetProcessMemoryInfo` selected | Limited-query access may fail | Gauge; no delta | Medium across all accessible processes | `/proc/<pid>/status` | `proc_pid_rusage` resident size | Windows/Linux/macOS implemented |
| Process I/O read | `MetricValue<BytesPerSecond>` | bytes/s | Normal / 1 Hz | `GetProcessIoCounters.ReadTransferCount` selected | Limited-query access may fail | Per-identity cumulative byte delta / measured seconds | Medium across all accessible processes | `/proc/<pid>/io` | `proc_pid_rusage` disk bytes read | Windows/Linux/macOS implemented |
| Process I/O write | `MetricValue<BytesPerSecond>` | bytes/s | Normal / 1 Hz | `GetProcessIoCounters.WriteTransferCount` selected | Limited-query access may fail | Same as process read | Medium across all accessible processes | `/proc/<pid>/io` | `proc_pid_rusage` disk bytes written | Windows/Linux/macOS implemented |
| GPU engine/memory | `MetricValue<Ratio>` plus two `MetricValue<ByteCount>` gauges | fraction `[0,1]`, bytes | Fast / 1 Hz | Persistent PDH `GPU Engine(*)` and `GPU Adapter Memory(*)` queries | None observed | Busiest physical engine after per-engine process summation; dedicated/shared bytes summed | Optional bounded PDH arrays; measured with provider | DRM/sysfs/vendor candidates | Metal/IOKit candidate | Windows implemented V0.14; capability gated |
| Foreground application/GPU correlation | `MetricValue<ProcessIdentity>` plus `MetricValue<Ratio>` | opaque identity, fraction `[0,1]` | Fast / 1 Hz when explicitly enabled | `GetForegroundWindow`, `GetWindowThreadProcessId`, `GetProcessTimes`, and GPU-engine PDH rows | Limited-query access may fail | Current `(PID, creation token)` plus maximum matching current-PID engine usage; correlation only | Optional; no title or content read | Desktop/session candidates | Workspace/accessibility candidates | Windows implemented V0.14; privacy gated |
| DPC/ISR responsiveness | two `MetricValue<Ratio>` plus `MetricValue<double>` | fractions `[0,1]`, DPC/s | Fast / 1 Hz | Persistent PDH `Processor Information(_Total)` DPC/interrupt counters | None observed | Percentages divided by 100 and clamped; nonnegative rate gauge | Optional bounded PDH query | `/proc/interrupts`/trace candidates | Instruments candidate | Windows implemented V0.14; context only |
| CPU frequency/thermal limit | four `MetricValue<double/Ratio>` gauges | MHz, fraction `[0,1]` | Normal / 1 Hz | `CallNtPowerInformation(ProcessorInformation)` | None observed | Mean current/max/limit MHz across active processors; limit/max ratio | Low bounded array | sysfs candidates | `sysctl`/IOKit candidates | Windows implemented V0.14; context only |
| Power/battery/uptime | typed source, battery/saver, uptime gauges | enum, fraction, boolean, seconds | Normal / 1 Hz | `GetSystemPowerStatus`, `GetTickCount64` | None | Direct gauges with explicit unsupported battery fraction | Very low | sysfs/proc candidates | IOKit candidates | Windows implemented V0.14; capability gated |
| Per-process network | optional rate | bytes/s | Unspecified | No low-cost stable choice selected | Varies | Identity-aware flow accounting | Unknown/high | eBPF/netlink candidates | Network Extension candidates | Unsupported / research |

## Counter rules

- Compute deltas with monotonic observation times, never the nominal configured interval.
- The first cumulative observation establishes a baseline and emits no fabricated rate.
- A decreasing counter is a reset, wrap, interface replacement, or process replacement. Discard that delta unless the counter width and wrap are proven.
- Treat elapsed time `<= 0` as invalid. Track it as a diagnostic rather than dividing.
- Use sufficiently wide integer counters and convert to floating point only for division/display.
- Do not silently substitute zero for inaccessible or unsupported data. Preserve availability so the UI and analysis can distinguish “zero” from “unknown.”
- Aggregate only intentionally selected disks/interfaces. Document loopback, virtual adapters, removable media, and logical-vs-physical disk policy after validation.

## Sampling and metadata

V0.1 starts with a simple one-second schedule, but provider and scheduler contracts must allow independent fast, normal, and slow/event-driven tiers. Static process metadata is keyed by process identity and cached. Cache entries expire only after process exit plus any retention needed by the ring buffer; this bounds metadata while keeping historical samples resolvable.

Collection output records capability and availability changes. A provider failure should degrade the affected metric and increment diagnostics without terminating collection of healthy signals.

## Linux system/process engineering provider

Linux has a development-only `LinuxTelemetryProvider` behind the same portable provider contract.
It reads bounded kernel pseudo-files without administrator privileges:

| Metric | Source | Tier | Portable representation |
|---|---|---|---|
| CPU | aggregate `cpu` row in `/proc/stat` | Fast | cumulative busy and total kernel ticks |
| Physical memory | `MemTotal` and `MemAvailable` in `/proc/meminfo` | Normal | total and available bytes |
| Logical processors | numbered `cpuN` rows in `/proc/stat` | Fast | available count |
| Physical-device I/O | `/sys/block/<device>/stat` entries with a native `device` link | Fast | cumulative read/write bytes |
| Network I/O | non-loopback rows in `/proc/net/dev` | Fast | cumulative receive/transmit bytes |
| Process identity and CPU | `/proc/<pid>/stat` | Normal | PID plus kernel start-time token and cumulative CPU ticks |
| Process memory | `VmRSS` in `/proc/<pid>/status` | Normal | working-set bytes |
| Process I/O | `/proc/<pid>/io` | Normal | cumulative read/write bytes |
| Process metadata | stat name/parent plus `/proc/<pid>/exe` | Normal; path only on Slow | portable metadata with explicit availability |

CPU total sums Linux user, nice, system, idle, I/O wait, IRQ, soft IRQ, and steal counters. Guest
fields are not added because Linux already includes them in user/nice. Busy subtracts idle and I/O
wait from total; the shared normalizer still computes `delta(busy) / delta(total)`. Memory values
must use the kernel's `kB` unit, are converted as KiB (`* 1024`), and require
`0 < available <= total`. Both parsers reject missing/duplicate fields, noncanonical numbers,
overflow, impossible relationships, and input above the provider's 1 MiB bound. A failed requested
source becomes `temporarily_unavailable`. Block-sector conversion uses the kernel accounting unit of
512 bytes and rejects multiplication overflow. Disk and interface aggregation use the same bounded
lifecycle semantics as Windows: a new/reappearing entity establishes a baseline, removal contributes
no negative delta, and a counter reset invalidates only that channel for one sample. Loopback is
excluded so local traffic is not presented as host network transport.

The process walk accepts only numeric `/proc` entries, caps each observation at 8,192 identities,
uses PID plus kernel start time to prevent reuse collisions, and treats exit/access races as explicit
diagnostics or per-metric unavailability. CPU ticks are converted to cumulative nanoseconds using
`_SC_CLK_TCK`; the shared normalizer derives rates using the measured interval. Executable paths are
resolved only when the slow tier is requested. GPU, event, power, platform-shell, and crash metrics
remain `unsupported`.

`blackbox_telemetry_linux` is built only on Linux and has no SQLite, UI, storage, analysis, or
platform-shell dependency. Hosted Linux sanitizer and coverage builds compile it, and a native test
samples the runner's real `/proc` and `/sys` files before applying the backend-independent provider
contract. Portable parser tests also run on Windows. A separate hosted step builds the complete Linux
desktop target and runs a bounded diagnostic smoke under Xvfb. Reused pseudo-file buffers avoid
reallocating provider scratch storage on each observation. A native all-tier benchmark retains direct-v1
timing/RSS aggregates and fails if P95 exceeds 250 ms, any observation exceeds one second, the process
inventory is empty, or a provider sample fails. CI creates TGZ and native DEB/RPM engineering
previews, validates their desktop/RPATH/package metadata, extracts them, and launches the packaged
executable under Xvfb. The Linux platform layer also uses the session D-Bus notification service
through a bounded asynchronous queue and requests global shortcut registration through
`org.freedesktop.portal.GlobalShortcuts`; unavailable services remain explicit and a missing tray
host keeps the window reachable. This is engineering evidence only: BlackBox does not claim physical
Linux product qualification.

## macOS system/process engineering provider

The macOS-only `MacosTelemetryProvider` uses the same caller-owned snapshot and capability contract.
Mach host CPU ticks provide cumulative busy/total counters; `host_statistics64` plus
`sysctlbyname("hw.memsize")` provide physical-memory gauges; and `hw.logicalcpu` reports capacity.
The process collector enumerates bounded PIDs with libproc, keys each identity by PID plus the
microsecond process start token from `proc_pid_rusage`, and collects cumulative CPU time, resident
memory, and disk read/write bytes. Name, parent PID, and executable path remain metadata, with path
resolution limited to slow-tier requests. Access races and protected identities stay per-process
gaps instead of failing the provider. Disk, network, GPU, event, power, platform-shell, and crash
telemetry metrics are explicitly unsupported.

`blackbox_telemetry_macos` is built only on Apple hosts and links only core, portable telemetry, and
the native libproc boundary. Hosted Apple Silicon and Intel jobs build/test the complete desktop
graph, exercise the real-host provider contract, collect 64 bounded benchmark observations, and
create an unsigned TGZ engineering preview containing a native `.app` bundle. The separate macOS
platform adapter owns the per-user instance lock, SDL menu-bar tray, current ServiceManagement login
item, bounded permission-aware UserNotifications requests, and AppKit increased-contrast/reduced-motion
preferences. None of those adapters runs on the collection thread. Hosted tests exercise the lock and
no-service fallback, while bundle checks validate identifier, version, deployment floor, and packaged
layout. That evidence does not qualify physical client behavior, global shortcuts, signing,
notarization, or product support.

## Deterministic mock scenarios

The mock provider emits one stable process (`PID 4242`, creation token `1`) and advances counters once per requested snapshot. The anomaly interval is the fifth observation:

| Scenario | Anomaly interval behavior |
|---|---|
| `normal` | CPU 25%, disk read 1 MiB/s, disk write 256 KiB/s, RX 128 KiB/s, TX 64 KiB/s |
| `cpu_spike` | CPU becomes 90%; other system signals remain normal |
| `disk_spike` | Disk read becomes 64 MiB/s and write 32 MiB/s |
| `network_drop` | RX and TX counters do not advance, producing measured zero rates |
| `process_spike` | Mock process adds 800 ms CPU time, 8 MiB read, and 4 MiB written |

The provider does not advance the injected clock. Tests or a future caller control elapsed time explicitly.

## Windows V0.0.3 source decisions

### CPU: `GetSystemTimes`

Selected because it is a direct Kernel32 API, requires no administrator privilege, returns cumulative 100-nanosecond idle/kernel/user ticks, and is inexpensive enough for the initial one-second interval. Microsoft documents that kernel time includes idle time, so conversion is:

```text
total = kernel + user
busy  = total - idle
usage = delta(busy) / delta(total)
```

Microsoft also documents an important topology limit: up to 64 logical processors the values sum all processors; above 64 they cover only the primary processor group of the calling thread. BlackBox does not label this whole-machine CPU on such systems. Source: [GetSystemTimes documentation](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-getsystemtimes).

V0.0.3 comparison against Windows `\Processor(_Total)\% Processor Time`, using aligned one-second multi-sample means on the validation host:

| Condition | BlackBox mean | Windows counter mean | Absolute difference |
|---|---:|---:|---:|
| Ambient activity, 11 intervals | 12.863% | 12.775% | 0.089 percentage points |
| Six-worker CPU load, 11 intervals | 63.542% | 61.777% | 1.765 percentage points |

The milestone tolerance is 2 percentage points for these local validation runs. Individual samples are not expected to match exactly because the two collectors do not share identical interval boundaries.

### Memory: `GlobalMemoryStatusEx`

Selected because it is a direct system gauge, requires no administrator privilege, and returns physical-memory totals in bytes. `ullTotalPhys` is physical memory available to the OS; `ullAvailPhys` is memory immediately reusable without first writing it to disk, including standby, free, and zero lists. BlackBox derives used memory as `total - available` and rejects `available > total`. Source: [MEMORYSTATUSEX documentation](https://learn.microsoft.com/windows/win32/api/sysinfoapi/ns-sysinfoapi-memorystatusex).

The provider function table exists only inside the Windows boundary and permits deterministic failure injection. CPU or memory read failure marks that metric temporarily unavailable; one failure yields a partial sample, both yield a temporarily failed sample, and later calls continue normally. The app logs only changes in overall provider state.

## Windows V0.0.5 disk and network decisions

### Disk throughput: persistent PDH physical-disk query

BlackBox selects the English PDH wildcard counters `PhysicalDisk(*)\\Disk Read Bytes/sec` and `PhysicalDisk(*)\\Disk Write Bytes/sec`. The query is opened and primed once; sampling reads each instance's raw cumulative `FirstValue`. `_Total` is excluded, so it is never added to its own component instances. Numeric physical-disk instance prefixes form lifecycle identities. Logical-disk counters are excluded; removable and virtual storage is included only when Windows exposes it as a physical-disk instance. Storage virtualization can amplify or redistribute bytes, so this metric describes I/O at the Windows physical-disk counter layer rather than exact application payload.

The alternative native `IOCTL_DISK_PERFORMANCE` probe opened zero physical drives without elevation on the validation host. Microsoft also documents that each `IOCTL_DISK_PERFORMANCE` request increments a driver's performance-counter enable reference and must be balanced with `IOCTL_DISK_PERFORMANCE_OFF`. The benchmark balances every successful probe, but repeated device discovery, privilege gaps, and stateful enablement make it a poorer production source. The persistent PDH query was available to the ordinary user and was selected. Sources: [PDH raw counter arrays](https://learn.microsoft.com/windows/win32/api/pdh/nf-pdh-pdhgetrawcounterarrayw), [`DISK_PERFORMANCE`](https://learn.microsoft.com/windows/win32/api/winioctl/ns-winioctl-disk_performance), and [`IOCTL_DISK_PERFORMANCE`](https://learn.microsoft.com/windows-hardware/drivers/ddi/ntdddisk/ni-ntdddisk-ioctl_disk_performance).

A 32 MiB unbuffered, write-through fixture produced 64.26 MiB at the selected physical counter layer on the validation host. Automated acceptance requires at least 90% and no more than 4x the payload so metadata, unrelated host activity, and storage-layer amplification remain visible without confusing them with source failure. The test deletes its temporary file and never runs in the recorder path.

V0.13 supersedes the earlier unsupported latency decision with a separately
capability-gated physical-layer query. Its exact aggregation, caveats, passive
network sources, and no-RTT decision are specified in `WINDOWS_FORENSICS.md`.

### Network throughput: physical transport interfaces

`GetIfTable2` supplies 64-bit cumulative `InOctets` and `OutOctets`. BlackBox includes only operational interfaces marked as hardware and excludes filter, endpoint, software-loopback, and tunnel interfaces. This policy counts traffic at the physical transport layer once: VPN and virtual-switch traffic is observed when it traverses selected hardware but is not added again at a virtual layer. Purely virtual transports are intentionally absent. The locally unique interface LUID is the lifecycle identity; interface indices are not used because Windows permits them to change after disable/enable. Sources: [`GetIfTable2`](https://learn.microsoft.com/windows/win32/api/netioapi/nf-netioapi-getiftable2) and [`MIB_IF_ROW2`](https://learn.microsoft.com/windows/win32/api/netioapi/ns-netioapi-mib_if_row2).

The lifecycle tracker has a fixed 128-entity capacity and allocates no sampling-path memory. First sight and reappearance establish baselines; disappearance subtracts nothing; stable entities contribute only deltas. A decrease or aggregate overflow marks only the affected channel temporarily unavailable, updates its baseline, and recovers on the next valid observation. Duplicate identities and capacity overflow reject the observation. Source failure also forces one warm-up interval on recovery. These rules cover adapter/disk churn and counter reinitialization after power transitions without emitting a false rate spike.

A controlled 64 MiB TCP loopback fixture validates the exclusion policy: the transfer completes but does not appear in the hardware aggregate. Windows does not consistently increment software-loopback MIB octets for local TCP, so the completed payload—not that MIB row—is the control. Positive interface deltas and zero-traffic intervals are covered by deterministic function-table fixtures and real-provider integration sampling.

## Windows V0.0.6 process decisions

### Enumeration and identity

Normal-tier collection uses bounded PSAPI `EnumProcesses` PID discovery and reuses identity-bound
handles opened with limited-query plus synchronize rights. Each cached handle must still refer to a
live process object, and `GetProcessTimes` creation `FILETIME` must match the cached identity before
reuse; PID recycling therefore cannot join unrelated processes. Parent PID and base executable name
are refreshed with Tool Help only on the independent slow tier, alongside executable paths. The
8,192-PID enumeration fails closed if the fixed buffer fills instead of silently returning an
incomplete inventory.

On the same validation host, the untouched all-tier path averaged 11.366 ms (P95 13.660 ms); the
normal-tier PSAPI/identity-handle implementation averaged 3.246 ms (P95 3.633 ms, P99 4.181 ms), a
roughly 71% average reduction while preserving full creation-token identity. The final warm sample
reused 192 live handles and explicitly reported 132 inaccessible identities. Tool Help remains the
metadata source, but no longer taxes every normal observation. Sources: [EnumProcesses](https://learn.microsoft.com/windows/win32/api/psapi/nf-psapi-enumprocesses), [CreateToolhelp32Snapshot](https://learn.microsoft.com/windows/win32/api/tlhelp32/nf-tlhelp32-createtoolhelp32snapshot), and [process access rights](https://learn.microsoft.com/windows/win32/procthread/process-security-and-access-rights).

### Counters and availability

For each process whose creation time is readable, BlackBox collects cumulative kernel+user time with `GetProcessTimes`, working set with `GetProcessMemoryInfo`, and cumulative read/write transfer bytes with `GetProcessIoCounters`. CPU is normalized as process CPU-time delta divided by measured wall time and active logical processor count, so `1.0` means all machine capacity. I/O is Windows process I/O accounting; it should not be interpreted as bytes proven to reach a physical disk. Microsoft describes the working set as physical memory mapped into the process and `IO_COUNTERS` as process I/O accounting. Sources: [GetProcessMemoryInfo](https://learn.microsoft.com/windows/win32/api/psapi/nf-psapi-getprocessmemoryinfo), [process memory semantics](https://learn.microsoft.com/windows/win32/psapi/process-memory-usage-information), and [`IO_COUNTERS`](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-io_counters).

Protected and system processes are expected gaps, not whole-provider failures. On the validation host, 113 of 286 enumerated processes were queryable and 173 were explicitly counted as inaccessible. A process that exits between enumeration and query increments an exit diagnostic and is skipped or carries unavailable metrics; collection continues. Individual metric access failures retain `inaccessible` or `temporarily_unavailable`. Provider status becomes partial only when the process snapshot itself fails, not because ordinary-user access excludes individual processes.

### Metadata, cadence, and bounds

Base name and parent PID are refreshed from the slow-tier Tool Help snapshot. Executable paths use
`QueryFullProcessImageNameW`, which accepts limited-query handles on supported Windows versions.
Path resolution runs on that same independent 30-second tier; the collector's initial observation
includes it, while a process born later may wait until the next slow observation. Success and access
denial are terminal for that identity; only transient failures retry on a future slow tier. Normal
one-second samples perform neither Tool Help nor path queries. Source:
[QueryFullProcessImageNameW](https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-queryfullprocessimagenamew).

The native metadata cache holds active identities only and is capped at 8,192. The portable collector cache retains exited metadata for the configured history so recorded frames remain resolvable, also capped at 8,192; it evicts the oldest inactive entry if full and never evicts an active entry merely to admit another. Process time series use a parallel `CircularRecorder<ProcessFrame>` and never enter `SystemSample` or the UI's 300-system-sample copy. A global 600,000-process-entry history budget yields 2,000 rows/frame at the default 300 frames and 500 rows/frame at 1,200 frames (250 ms for five minutes). Excess rows are omitted from history with an observable truncation count; the current active view remains complete.

The active-process UI copies only the current frame and its active metadata, sorts display-ready rows by total-machine CPU, and shows at most 50. The UI has no telemetry or Win32 dependency. V0.0.7 incident capture requests only the newest bounded frame count capable of intersecting its pre/post window, filters again by monotonic time, flattens selected rows into the core incident domain, and copies only metadata referenced by those rows.

## Windows V0.14 extended evidence

V0.14 adds capability-gated GPU engine/memory, optional foreground identity/GPU correlation,
DPC/ISR load, CPU current/max/thermal-limit MHz, power/battery state, and uptime to the normal
system sample. Discrete power/device/audio/selected-Windows events use an independent provider,
thread, callback queue, and fixed event ring; they are copied into the main domain only while an
immutable incident is constructed. Missing optional sources do not degrade core CPU/memory/I/O
provider health.

An independent opt-in process-lifecycle gate reuses the already-required Tool Help process
enumeration, so it adds no second OS poll. The first complete inventory, the first observation after
enable/reconfigure/resume, and any enumeration/query uncertainty are treated as resynchronization
and cannot emit transitions. Later durable identities emit normalized start/exit context into the
same bounded event ring. These informational records never trigger capture and are not causal proof.

Source selection, exact event IDs, privacy exclusions, bounded queue policy, independent
disablement, and rejected frame/audio probes are specified in `WINDOWS_EVENT_EVIDENCE.md`. DNS
Client event 1014 crosses this boundary only as a normalized numeric timeout record; its queried
hostname, Event Log message, and payload never enter the portable domain.
Display event 4101 likewise crosses only as the canonical graphics/display-recovery numeric record;
its message, driver name, adapter identity, and payload are absent. Unlike the common DNS warning,
this OS-confirmed recovery symptom may use the existing bounded automatic-capture coordinator when
automatic detection is enabled. It remains symptom evidence, never a driver, application, GPU, or
root-cause attribution.
Application Error event 1000 likewise crosses only as the canonical application/crash numeric
record. Application/module names, exception codes, fault paths, messages, and payloads are absent.
With automatic detection enabled, it may use the bounded coordinator and can support only the exact
Windows-reported crash symptom, never the crashing program's identity, defect, or root cause.
Provider `disk` event 153 crosses only as the canonical storage/I/O-retry numeric record. Storage
LBAs, device paths, PDO identities, messages, and payloads are absent. With automatic detection
enabled, the OS-reported retry may use the bounded coordinator with a disk resource trigger, but it
never becomes an overload, cable, controller, driver, media, firmware, application, hardware, or
other root-cause attribution.
