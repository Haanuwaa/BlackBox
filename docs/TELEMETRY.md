# Telemetry reference

[Documentation index](README.md) · [Current candidate](CURRENT_CANDIDATE.md)

- [Telemetry specification](#telemetry-specification)
- [V0.14 Windows GPU, responsiveness, power, and event evidence](#v014-windows-gpu-responsiveness-power-and-event-evidence)
- [V0.13 Windows storage and network forensics](#v013-windows-storage-and-network-forensics)
- [Portable responsiveness and resource-pressure contract](#portable-responsiveness-and-resource-pressure-contract)

## Telemetry specification

This matrix separates portable semantics from native source availability. Implemented sources still
require capability, accuracy, overhead, privilege, reset, and lifecycle evidence on each platform;
an adjacent native counter is never relabelled to fill a portable gap.

### Portable contract

- `MetricValue<T>` carries `available`, `unsupported`, `inaccessible`, or `temporarily_unavailable`. An unavailable default value is never a measured zero.
- `Ratio` is a dimensionless fraction; UI code multiplies by 100 only for display.
- `ByteCount` is bytes. `BytesPerSecond` is a double-precision rate calculated from the measured monotonic interval.
- Raw CPU contains cumulative busy and total ticks in the same provider-defined unit. Normalized CPU is `busy_delta / total_delta`, representing total machine-capacity utilization in `[0,1]`.
- Process CPU uses the same total-machine convention: `1.0` means all logical machine capacity, not one fully occupied logical processor.
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
| Disk read throughput | `MetricValue<BytesPerSecond>` | bytes/s | Fast / 1 Hz | PDH `PhysicalDisk(*)\\Disk Read Bytes/sec` selected | None observed | Per-instance lifecycle-safe cumulative aggregation, then measured-time delta | Low; measured in provider benchmark | `/sys/block/*/stat` | IOKit `IOBlockStorageDriver` Statistics | Windows/Linux/macOS implemented |
| Disk write throughput | `MetricValue<BytesPerSecond>` | bytes/s | Fast / 1 Hz | PDH `PhysicalDisk(*)\\Disk Write Bytes/sec` selected | None observed | Same as read | Low; measured in provider benchmark | `/sys/block/*/stat` | IOKit `IOBlockStorageDriver` Statistics | Windows/Linux/macOS implemented |
| Disk latency/service time/queue | `MetricValue<Seconds>` / `MetricValue<double>` | seconds/op, requests | Fast / 1 Hz | Persistent PDH `PhysicalDisk(*)` formatted counters | None observed | Lifecycle-safe interval deltas; worst valid physical device | Low; qualified with provider | `/sys/block/*/stat` cumulative operation/time/weighted-time counters | IOKit operation and total completion-time counters; queue unavailable | Windows/Linux/macOS latency and service implemented; Linux queue implemented; macOS queue unsupported |
| Network receive | `MetricValue<BytesPerSecond>` | bytes/s | Fast / 1 Hz | `GetIfTable2` `InOctets` selected | None observed | Per-interface lifecycle-safe cumulative aggregation, then measured-time delta | Low; measured with provider | `/proc/net/dev` | `getifaddrs` interface counters | Windows/Linux/macOS implemented |
| Network transmit | `MetricValue<BytesPerSecond>` | bytes/s | Fast / 1 Hz | `GetIfTable2` `OutOctets` selected | None observed | Same as receive | Low; measured with provider | `/proc/net/dev` | `getifaddrs` interface counters | Windows/Linux/macOS implemented |
| Network connectivity/transitions | typed level plus interval count | level, events | Fast / 1 Hz | `GetNetworkConnectivityHint` + `GetIfTable2` | None observed | Aggregate hint and bounded lifecycle delta | Low | active non-loopback `AF_PACKET` interfaces | active non-loopback link interfaces | Windows implemented; Linux/macOS local-link evidence implemented |
| TCP retransmission/failure/reset | ratio plus interval counts | fraction, events | Fast / 1 Hz | `GetTcpStatisticsEx` IPv4+IPv6 | None observed | Counter deltas; retransmission ratio requires 8 segments | Low | `/proc/net/snmp` TCP MIB | `net.inet.tcp.stats`; exact established-reset field unsupported | Windows/Linux implemented; macOS supported subset implemented |
| Process identity | `ProcessIdentity` (PID + creation token) | opaque | Normal / 1 Hz | `EnumProcesses` plus cached `GetProcessTimes` creation `FILETIME` | Protected processes may be inaccessible | PID paired with creation time; new identity always warms up | Medium; measured | `/proc/<pid>/stat` start time | `proc_pid_rusage` start time | Windows/Linux/macOS implemented |
| Parent PID | `MetricValue<ProcessId>` | opaque | Normal / lifecycle | Slow-tier Tool Help metadata snapshot | Enumeration itself needs no elevation | Cached snapshot value; parent identity is not inferred | Slow-tier only | `/proc/<pid>/stat` | `proc_pidinfo` BSD info | Windows/Linux/macOS implemented |
| Process name | cached UTF-8 string | text | Normal / creation | Slow-tier `PROCESSENTRY32W.szExeFile` | Gaps only when identity cannot be opened | UTF-16 to UTF-8 once per identity | Low when cached | `/proc/<pid>/comm` | `proc_name` | Windows/Linux/macOS implemented |
| Executable path | cached UTF-8 string / unavailable | text | Slow / 30 s and creation lifecycle | `QueryFullProcessImageNameW` selected | Access may be denied | Resolve on slow tier; unavailable remains explicit | Bounded slow-tier work | `/proc/<pid>/exe` | `proc_pidpath` | Windows/Linux/macOS implemented |
| Process CPU | `MetricValue<Ratio>` | fraction of total machine capacity | Normal / 1 Hz | `GetProcessTimes` selected | Limited-query access may fail | CPU-time delta / measured wall interval / active logical CPU count | Medium across all accessible processes | `/proc/<pid>/stat` | `proc_pid_rusage` | Windows/Linux/macOS implemented |
| Process working set | `MetricValue<ByteCount>` | bytes | Normal / 1 Hz | `GetProcessMemoryInfo` selected | Limited-query access may fail | Gauge; no delta | Medium across all accessible processes | `/proc/<pid>/status` | `proc_pid_rusage` resident size | Windows/Linux/macOS implemented |
| Process I/O read | `MetricValue<BytesPerSecond>` | bytes/s | Normal / 1 Hz | `GetProcessIoCounters.ReadTransferCount` selected | Limited-query access may fail | Per-identity cumulative byte delta / measured seconds | Medium across all accessible processes | `/proc/<pid>/io` | `proc_pid_rusage` disk bytes read | Windows/Linux/macOS implemented |
| Process I/O write | `MetricValue<BytesPerSecond>` | bytes/s | Normal / 1 Hz | `GetProcessIoCounters.WriteTransferCount` selected | Limited-query access may fail | Same as process read | Medium across all accessible processes | `/proc/<pid>/io` | `proc_pid_rusage` disk bytes written | Windows/Linux/macOS implemented |
| GPU engine/memory | `MetricValue<Ratio>` plus two `MetricValue<ByteCount>` gauges | fraction `[0,1]`, bytes | Normal / 1 Hz | Persistent PDH `GPU Engine(*)` and `GPU Adapter Memory(*)` queries | None observed | Busiest physical device/engine; dedicated bytes checked-summed; unsupported siblings remain explicit | Bounded native arrays and optional dynamic vendor API | AMD `gpu_busy_percent`/VRAM sysfs; runtime-loaded NVIDIA NVML | Passive whole-system gauge unavailable | Windows and capability-driven Linux implemented; macOS explicitly unsupported |
| Foreground application/GPU correlation | typed process identity or session-scoped opaque application key plus `MetricValue<Ratio>` | opaque identity, fraction `[0,1]` | Normal / 1 Hz when explicitly enabled | `GetForegroundWindow`, `GetWindowThreadProcessId`, `GetProcessTimes`, and GPU-engine PDH rows | Limited-query access may fail | A durable process identity where supported; a non-correlatable application key otherwise; correlation only for process identity | Optional; no title, name, bus address, or content retained | X11 process identity plus readable DRM `fdinfo`; wlroots active state plus immediately hashed `app_id` without PID correlation | Workspace process identity; GPU value unavailable | Windows/macOS process identity, X11 process identity/DRM, and compositor-specific wlroots application key implemented; privacy gated |
| DPC/ISR responsiveness | two `MetricValue<Ratio>` plus `MetricValue<double>` | fractions `[0,1]`, DPC/s | Fast / 1 Hz | Persistent PDH `Processor Information(_Total)` DPC/interrupt counters | None observed | Percentages divided by 100 and clamped; nonnegative rate gauge | Optional bounded PDH query | `/proc/interrupts`/trace candidates | Instruments candidate | Windows implemented V0.14; context only |
| CPU frequency/thermal limit | four `MetricValue<double/Ratio>` gauges | MHz, fraction `[0,1]` | Normal / 1 Hz | `CallNtPowerInformation(ProcessorInformation)` | None observed | Mean current/max/limit MHz across active processors; limit/max ratio | Low bounded array | weighted CPUFreq policies | unavailable | Windows/Linux frequency implemented; Windows thermal-limit fraction implemented |
| Pressure / thermal state | five independent `MetricValue<Ratio>` interval gauges plus separate coarse thermal and memory-pressure enums | fraction `[0,1]`, enum | Fast PSI / Normal coarse states | cumulative-stall semantics unavailable; existing DPC/ISR stays separate | Source dependent | exact cumulative stalled-time delta / measured elapsed time; coarse states remain separate | Bounded reads or native transition callback | `/proc/pressure/{cpu,memory,io}` totals | public `NSProcessInfo.thermalState` plus Dispatch memory-pressure transitions | Linux PSI and both macOS coarse states implemented; no fabricated equivalence |
| Power/battery/uptime | typed source, battery/saver, uptime gauges | enum, fraction, boolean, seconds | Normal / 1 Hz | `GetSystemPowerStatus`, `GetTickCount64` | None | Direct gauges; unsupported fields remain explicit | Very low | `/sys/class/power_supply`, `/proc/uptime`, optional platform profile | IOKit power sources, continuous clock, Low Power Mode | Windows/Linux/macOS implemented; optional saver state remains capability-gated |
| Per-process network | optional rate | bytes/s | Unspecified | No low-cost stable choice selected | Varies | Identity-aware flow accounting | Unknown/high | eBPF/netlink candidates | Network Extension candidates | Unsupported / research |

### Counter rules

- Compute deltas with monotonic observation times, never the nominal configured interval.
- The first cumulative observation establishes a baseline and emits no fabricated rate.
- A decreasing counter is a reset, wrap, interface replacement, or process replacement. Discard that delta unless the counter width and wrap are proven.
- Treat elapsed time `<= 0` as invalid. Track it as a diagnostic rather than dividing.
- Use sufficiently wide integer counters and convert to floating point only for division/display.
- Do not silently substitute zero for inaccessible or unsupported data. Preserve availability so the UI and analysis can distinguish “zero” from “unknown.”
- Aggregate only intentionally selected disks/interfaces. Document loopback, virtual adapters, removable media, and logical-vs-physical disk policy after validation.

### Sampling and metadata

V0.1 starts with a simple one-second schedule, but provider and scheduler contracts must allow independent fast, normal, and slow/event-driven tiers. Static process metadata is keyed by process identity and cached. Cache entries expire only after process exit plus any retention needed by the ring buffer; this bounds metadata while keeping historical samples resolvable.

Collection output records capability and availability changes. A provider failure should degrade the affected metric and increment diagnostics without terminating collection of healthy signals.

### Linux system/process engineering provider

Linux has a development-only `LinuxTelemetryProvider` behind the same portable provider contract.
It reads bounded kernel pseudo-files without administrator privileges:

| Metric | Source | Tier | Portable representation |
|---|---|---|---|
| CPU | aggregate `cpu` row in `/proc/stat` | Fast | cumulative busy and total kernel ticks |
| Physical memory | `MemTotal` and `MemAvailable` in `/proc/meminfo` | Normal | total and available bytes |
| Logical processors | numbered `cpuN` rows in `/proc/stat` | Fast | available count |
| Physical-device I/O | `/sys/block/<device>/stat` entries with a native `device` link | Fast | cumulative read/write bytes, operations, completion time, and weighted in-flight time |
| Network I/O | non-loopback rows in `/proc/net/dev` | Fast | cumulative receive/transmit bytes |
| Network state and TCP quality | `getifaddrs` plus `/proc/net/snmp` | Fast | local link level, interface transitions, cumulative TCP counters |
| Power and uptime | `/sys/class/power_supply/*/uevent` plus `/proc/uptime` | Normal | typed source, optional battery fraction, seconds |
| CPU/memory/I/O pressure | `/proc/pressure/{cpu,memory,io}` cumulative totals | Fast | independent exact interval stall fractions with warm-up/reset state |
| CPU frequency/profile | CPUFreq policy sysfs plus optional ACPI platform profile | Normal | membership-weighted requested frequency and conservative saver state |
| GPU | AMD sysfs, optional runtime-loaded NVIDIA NVML, readable DRM `fdinfo` | Normal | capability-driven whole-device gauges plus separately scoped foreground activity |
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
excluded so local traffic is not presented as host network transport. Connectivity considers only
non-loopback interfaces whose kernel flags are both up and running. Presence proves a local link, not
Internet access, so Linux emits only `disconnected` or `local`; a fixed-capacity membership tracker
reports cumulative interface arrivals/removals without order sensitivity or hot-path allocation.
`/proc/net/snmp` supplies TCP `OutSegs`, `RetransSegs`, `AttemptFails`, and `EstabResets` as strict
cumulative counters for the shared normalizer.

`/proc/uptime` is parsed as a strict finite nonnegative seconds gauge. Power-supply uevents are
bounded and classify online mains/USB sources, UPS, battery-only operation, or unknown state.
Battery fraction is the arithmetic mean of valid present-battery capacities; hosts without a battery
retain an explicit unsupported fraction. When the optional generic ACPI platform-profile interface
is present, only its exact `low-power` value maps to the saver state; absent or unknown values remain
unsupported or temporarily unavailable rather than guessed.

The process walk accepts only numeric `/proc` entries, caps each observation at 8,192 identities,
uses PID plus kernel start time to prevent reuse collisions, and treats exit/access races as explicit
diagnostics or per-metric unavailability. CPU ticks are converted to cumulative nanoseconds using
`_SC_CLK_TCK`; the shared normalizer derives rates using the measured interval. Executable paths are
resolved only when the slow tier is requested. GPU evidence is capability-driven: AMD sysfs and
runtime-loaded NVIDIA NVML provide whole-device gauges, while readable DRM `fdinfo` activity is
explicitly foreground-client scope rather than complete host utilization.

Linux PSI reads are bounded to 4 KiB per file. The strict parser accepts the documented `some` and
optional `full` records, validates every average and cumulative total, and rejects duplicates,
unknown fields, overflow, malformed input, and missing `some`. The provider exports only cumulative
microseconds; the shared normalizer derives exact interval fractions independently per dimension and
warms up after first sight, reset, invalid elapsed time, source recovery, or suspend/resume.

Linux disk quality uses the kernel's cumulative block accounting without relabeling adjacent fields.
Read and write latency are their respective completion-time deltas divided by completed-operation
deltas; combined service time divides the summed completion-time delta by summed operations. The
interval-average in-flight queue is the weighted-I/O-time delta divided by elapsed monotonic time.
First sight, reappearance, a counter decrease, an empty-operation interval, duplicate identity, or
capacity overflow cannot fabricate a value. The provider selects the physical device with the worst
valid latency, breaking ties with queue depth, and keeps each unavailable channel explicit.

The independent Linux event provider listens to bounded kernel kobject uevents and accepts only
device `add`/`remove` actions. It maps the subsystem to one broad numeric class and discards device
paths, names, serials, modaliases, interfaces, properties, and all other payload. Queue truncation and
kernel receive-buffer loss remain diagnostic counters. The source can add context to a later incident,
but cannot trigger a capture or claim a symptom, affected identity, or cause.

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

### macOS system/process engineering provider

The macOS-only `MacosTelemetryProvider` uses the same caller-owned snapshot and capability contract.
Mach host CPU ticks provide cumulative busy/total counters; the full `host_statistics64` contract
plus `sysctlbyname("hw.memsize")` provide physical-memory gauges, compressed-memory state, and
cumulative pageout/swap/compression/decompression activity; and `hw.logicalcpu`, `hw.physicalcpu`,
and `hw.activecpu` provide explicit processor topology rather than frequency.
The fast tier also reads active non-loopback BSD interface byte counters and feeds their stable
interface indexes through the fixed-capacity lifecycle tracker, so interface arrival/removal cannot
become a false throughput spike. A separate fixed-capacity membership tracker records active-link
transitions and emits only disconnected/local state; it makes no Internet-reachability claim. IOKit
`IOBlockStorageDriver` Statistics dictionaries provide cumulative read/write bytes, completed
operations, and total completion times keyed by stable registry-entry identity. The shared disk
quality tracker derives read/write mean completion time, combined service time, and interval-average
I/O service concurrency from interval deltas. Service concurrency is summed service duration divided
by observation duration; it is neither instantaneous queue depth nor waiting work. IOKit does not
expose the exact interval queue counter used by the portable queue metric, so macOS reports that
channel as unsupported rather than estimating it. `net.inet.tcp.stats` provides
outgoing original segments, retransmissions,
and connection/drop failures. Apple exposes no exact equivalent of the shared established-reset
field, which therefore remains explicitly unsupported. Sleep-inclusive monotonic uptime comes from
the native continuous clock. The normal tier reads IOKit's current power-source snapshot and exposes
battery fraction when the host reports a valid capacity. `NSProcessInfo` supplies Low Power Mode and
the separate coarse nominal/fair/serious/critical thermal-pressure state. A Dispatch memory-pressure
source publishes only normal/warning/critical transitions and remains temporarily unavailable until
its first event. A separate one-second utility Dispatch timer records only wake-up delay beyond its
declared leeway and re-warms after suspend-sized gaps. VM counter deltas become byte rates using the
measured sample interval. Neither the scheduler observer, VM activity, nor either coarse state is
converted into CPU frequency, utilization, or Linux PSI semantics; unavailable sources remain explicit.
The independent macOS event provider consumes IOKit `IOMedia` first-match and termination
notifications. It drains the initial inventory without emitting events and publishes only broad
storage-media added/removed context; BSD names, registry paths, UUIDs, serials, properties, and
payloads never cross the native boundary. This is not application crash, service, storage-retry,
power, or general device evidence, and it cannot trigger capture.

The process collector enumerates bounded PIDs with libproc, keys each identity by PID plus the
microsecond process start token from `proc_pid_rusage`, and collects cumulative CPU time, resident
memory, and disk read/write bytes. Name, parent PID, and executable path remain metadata, with path
resolution limited to slow-tier requests. Access races and protected identities stay per-process
gaps instead of failing the provider. When foreground collection is opted in,
`NSWorkspace.frontmostApplication` contributes only a PID, which must match a process identity from
the same bounded sample before the PID-plus-creation-token identity crosses the portable boundary.
Names, titles, bundle identifiers, and native application objects do not cross. Public Metal device
inventory and BlackBox renderer health remain distinct from passive whole-system utilization. The
application records allocation-free frame-build, present, P95, maximum, failure, and hitch aggregates
for its own SDL renderer. MetricKit asynchronously contributes only delayed BlackBox cumulative
CPU/GPU duration and hang count/duration; payload call stacks and other diagnostic content are
discarded. Passive whole-system GPU utilization, Windows-style DPC/ISR responsiveness, current CPU
frequency, exact disk queue depth, and exact cumulative-stall pressure are explicitly unsupported;
VM activity and coarse pressure states remain available under separate semantics.

`blackbox_telemetry_macos` is built only on Apple hosts and links only core, portable telemetry,
libproc, and IOKit. Hosted Apple Silicon and Intel jobs build/test the complete desktop
graph, exercise the real-host provider contract, collect 64 bounded benchmark observations, and
create an unsigned TGZ engineering preview containing a native `.app` bundle. The separate macOS
platform adapter owns the per-user instance lock, SDL menu-bar tray, current ServiceManagement login
item, bounded permission-aware UserNotifications requests, and AppKit increased-contrast/reduced-motion
preferences. The macOS shortcut adapter uses AppKit local and global key-down monitors, requests the
public event-listening/Input Monitoring permission, suppresses repeats, and never consumes the local
event. It is passive observation, not conflict-aware reservation; denial stays visible and retryable.
None of those adapters runs on the collection thread. Hosted tests exercise the lock and
no-service fallback, while bundle checks validate identifier, version, deployment floor, and packaged
layout. That evidence does not qualify physical client behavior, global shortcuts, signing,
notarization, or product support.

### Linux power, foreground, and responsiveness decisions

The Linux fast tier reads each bounded CPUFreq policy's `scaling_cur_freq`, maximum frequency, and
affected CPU list. It computes CPU-membership-weighted current and maximum MHz while preserving the
kernel interface's limitation: `scaling_cur_freq` often reflects the last requested P-state and is
not guaranteed to be an exact instantaneous hardware clock. Missing CPUFreq support is `unsupported`;
malformed or unreadable advertised policy data is temporary failure. The normal tier reads the
generic ACPI `platform_profile` interface when present and maps only `low-power` to the portable
battery-saver state. Unknown vendor values fail closed rather than being guessed.

On X11, the optional native reader accepts only EWMH `_NET_ACTIVE_WINDOW` plus `_NET_WM_PID`, verifies
`WM_CLIENT_MACHINE` against the local host to reject remote-X PID collisions, and correlates the PID
to the current process sample's creation identity. It never reads a window title or process name.
Generic Wayland process identity remains `unsupported`: the public XDG Desktop Portal API has no
standardized active-window identity interface. When a compositor advertises the wlroots unstable
foreign-toplevel-management protocol, a separate worker can publish an opaque, session-scoped
application key. It ignores titles, immediately hashes a bounded `app_id` with an in-memory random
key, never exports the raw value, and never guesses a PID or correlates GPU/process evidence. Protocol
absence, ambiguity, loss, reconnect warm-up, and capacity exhaustion remain explicit. GNOME and KDE's
private protocol are not treated as supported sources.

Linux GPU collection is capability-driven rather than falsely universal. A bounded inventory scans
DRM card/render nodes without exporting names, serials, PCI locations, or client identifiers. AMD's
documented sysfs gauges provide busy percentage and VRAM use. NVIDIA support dynamically loads the
installed `libnvidia-ml.so.1`; no SDK or driver library becomes a package dependency. Multi-device
utilization keeps the existing busiest-device semantics and dedicated memory uses a checked sum.
Missing libraries/files are `unsupported`, permissions are `inaccessible`, malformed/reset/overflow
observations are temporary, and slow-tier inventory refresh handles device churn.

For Intel and other DRM drivers that publish standardized `fdinfo`, BlackBox reads only the already
opted-in foreground PID. It retains fixed-capacity one-way identities and cumulative engine counters,
warms new/replaced clients, rejects resets, removes departed clients, groups activity by engine, and
emits only the maximum bounded fraction. It never scans window titles or exports PID, client id,
engine label, or PCI location from this component, and it never presents readable-client coverage as
whole-system utilization. Compositor-specific opaque Wayland application evidence cannot unlock this
process/GPU correlation.

macOS exposes only public, non-identifying Metal inventory counts and default render-device
availability. The UI separately reports the successfully initialized SDL renderer backend. Public
Metal counters measure command buffers or passes owned by BlackBox, so passive whole-system and
foreground GPU utilization remain `unsupported`; private IOReport/IORegistry properties and
privileged `powermetrics` parsing are not used. Linux PSI is implemented as separate cumulative-stall
pressure evidence; it is not a substitute for Windows DPC/ISR semantics.

### Portable system-event parity

System events remain separate from periodic telemetry: each native provider is polled by the bounded
event collector and never inserts OS callbacks or journal work into the one-second sampler. Linux
normalizes uevent classes for audio, block storage, DRM display changes, network link changes, and
generic device churn; all identity-bearing uevent fields are ignored. Logind contributes
`PrepareForSleep`, systemd Manager contributes only a normalized job result class, and an optional
libsystemd journal cursor matches only the documented coredump message ID. It never retrieves the
journal record's executable, command line, UID, path, signal, message, or dump fields.

macOS normalizes CoreAudio device/default changes, CoreGraphics display reconfiguration,
SystemConfiguration IPv4/IPv6 global changes, IOKit media/power changes, and `NSWorkspace` application
launch/termination notifications. Display IDs and notification user-info are ignored. A termination
notification is lifecycle context and is never relabeled as a crash. No public general launchd event
stream has been accepted, so service events remain unsupported there. `SMAppService` status is used
only by the background shell to explain BlackBox's own login-item state; it is not recorded as a
system event.

GPU inventory now includes integrated, discrete, and unknown type counts whose sum must equal the
total. Windows DXGI filters software adapters and reports every remaining adapter as unknown type;
dedicated-memory size is not used as a classification heuristic. Linux DRM/NVML devices are likewise
reported as unknown type because driver name is not a reliable integrated/discrete classification;
macOS retains Metal's public low-power classification and exposes zero unknown devices.
The pressure design that prevents PSI/DPC semantic conflation is in `TELEMETRY.md`.

### Deterministic mock scenarios

The mock provider emits one stable process (`PID 4242`, creation token `1`) and advances counters once per requested snapshot. The anomaly interval is the fifth observation:

| Scenario | Anomaly interval behavior |
|---|---|
| `normal` | CPU 25%, disk read 1 MiB/s, disk write 256 KiB/s, RX 128 KiB/s, TX 64 KiB/s |
| `cpu_spike` | CPU becomes 90%; other system signals remain normal |
| `disk_spike` | Disk read becomes 64 MiB/s and write 32 MiB/s |
| `network_drop` | RX and TX counters do not advance, producing measured zero rates |
| `process_spike` | Mock process adds 800 ms CPU time, 8 MiB read, and 4 MiB written |

The provider does not advance the injected clock. Tests or a future caller control elapsed time explicitly.

### Windows V0.0.3 source decisions

#### CPU: `GetSystemTimes`

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

#### Memory: `GlobalMemoryStatusEx`

Selected because it is a direct system gauge, requires no administrator privilege, and returns physical-memory totals in bytes. `ullTotalPhys` is physical memory available to the OS; `ullAvailPhys` is memory immediately reusable without first writing it to disk, including standby, free, and zero lists. BlackBox derives used memory as `total - available` and rejects `available > total`. Source: [MEMORYSTATUSEX documentation](https://learn.microsoft.com/windows/win32/api/sysinfoapi/ns-sysinfoapi-memorystatusex).

The provider function table exists only inside the Windows boundary and permits deterministic failure injection. CPU or memory read failure marks that metric temporarily unavailable; one failure yields a partial sample, both yield a temporarily failed sample, and later calls continue normally. The app logs only changes in overall provider state.

### Windows V0.0.5 disk and network decisions

#### Disk throughput: persistent PDH physical-disk query

BlackBox selects the English PDH wildcard counters `PhysicalDisk(*)\\Disk Read Bytes/sec` and `PhysicalDisk(*)\\Disk Write Bytes/sec`. The query is opened and primed once; sampling reads each instance's raw cumulative `FirstValue`. `_Total` is excluded, so it is never added to its own component instances. Numeric physical-disk instance prefixes form lifecycle identities. Logical-disk counters are excluded; removable and virtual storage is included only when Windows exposes it as a physical-disk instance. Storage virtualization can amplify or redistribute bytes, so this metric describes I/O at the Windows physical-disk counter layer rather than exact application payload.

The alternative native `IOCTL_DISK_PERFORMANCE` probe opened zero physical drives without elevation on the validation host. Microsoft also documents that each `IOCTL_DISK_PERFORMANCE` request increments a driver's performance-counter enable reference and must be balanced with `IOCTL_DISK_PERFORMANCE_OFF`. The benchmark balances every successful probe, but repeated device discovery, privilege gaps, and stateful enablement make it a poorer production source. The persistent PDH query was available to the ordinary user and was selected. Sources: [PDH raw counter arrays](https://learn.microsoft.com/windows/win32/api/pdh/nf-pdh-pdhgetrawcounterarrayw), [`DISK_PERFORMANCE`](https://learn.microsoft.com/windows/win32/api/winioctl/ns-winioctl-disk_performance), and [`IOCTL_DISK_PERFORMANCE`](https://learn.microsoft.com/windows-hardware/drivers/ddi/ntdddisk/ni-ntdddisk-ioctl_disk_performance).

A 32 MiB unbuffered, write-through fixture produced 64.26 MiB at the selected physical counter layer on the validation host. Automated acceptance requires at least 90% and no more than 4x the payload so metadata, unrelated host activity, and storage-layer amplification remain visible without confusing them with source failure. The test deletes its temporary file and never runs in the recorder path.

V0.13 supersedes the earlier unsupported latency decision with a separately
capability-gated physical-layer query. Its exact aggregation, caveats, passive
network sources, and no-RTT decision are specified in `TELEMETRY.md`.

#### Network throughput: physical transport interfaces

`GetIfTable2` supplies 64-bit cumulative `InOctets` and `OutOctets`. BlackBox includes only operational interfaces marked as hardware and excludes filter, endpoint, software-loopback, and tunnel interfaces. This policy counts traffic at the physical transport layer once: VPN and virtual-switch traffic is observed when it traverses selected hardware but is not added again at a virtual layer. Purely virtual transports are intentionally absent. The locally unique interface LUID is the lifecycle identity; interface indices are not used because Windows permits them to change after disable/enable. Sources: [`GetIfTable2`](https://learn.microsoft.com/windows/win32/api/netioapi/nf-netioapi-getiftable2) and [`MIB_IF_ROW2`](https://learn.microsoft.com/windows/win32/api/netioapi/ns-netioapi-mib_if_row2).

The lifecycle tracker has a fixed 128-entity capacity and allocates no sampling-path memory. First sight and reappearance establish baselines; disappearance subtracts nothing; stable entities contribute only deltas. A decrease or aggregate overflow marks only the affected channel temporarily unavailable, updates its baseline, and recovers on the next valid observation. Duplicate identities and capacity overflow reject the observation. Source failure also forces one warm-up interval on recovery. These rules cover adapter/disk churn and counter reinitialization after power transitions without emitting a false rate spike.

A controlled 64 MiB TCP loopback fixture validates the exclusion policy: the transfer completes but does not appear in the hardware aggregate. Windows does not consistently increment software-loopback MIB octets for local TCP, so the completed payload—not that MIB row—is the control. Positive interface deltas and zero-traffic intervals are covered by deterministic function-table fixtures and real-provider integration sampling.

### Windows V0.0.6 process decisions

#### Enumeration and identity

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

#### Counters and availability

For each process whose creation time is readable, BlackBox collects cumulative kernel+user time with `GetProcessTimes`, working set with `GetProcessMemoryInfo`, and cumulative read/write transfer bytes with `GetProcessIoCounters`. CPU is normalized as process CPU-time delta divided by measured wall time and active logical processor count, so `1.0` means all machine capacity. I/O is Windows process I/O accounting; it should not be interpreted as bytes proven to reach a physical disk. Microsoft describes the working set as physical memory mapped into the process and `IO_COUNTERS` as process I/O accounting. Sources: [GetProcessMemoryInfo](https://learn.microsoft.com/windows/win32/api/psapi/nf-psapi-getprocessmemoryinfo), [process memory semantics](https://learn.microsoft.com/windows/win32/psapi/process-memory-usage-information), and [`IO_COUNTERS`](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-io_counters).

Protected and system processes are expected gaps, not whole-provider failures. On the validation host, 113 of 286 enumerated processes were queryable and 173 were explicitly counted as inaccessible. A process that exits between enumeration and query increments an exit diagnostic and is skipped or carries unavailable metrics; collection continues. Individual metric access failures retain `inaccessible` or `temporarily_unavailable`. Provider status becomes partial only when the process snapshot itself fails, not because ordinary-user access excludes individual processes.

#### Metadata, cadence, and bounds

Base name and parent PID are refreshed from the slow-tier Tool Help snapshot. Executable paths use
`QueryFullProcessImageNameW`, which accepts limited-query handles on supported Windows versions.
Path resolution runs on that same independent 30-second tier; the collector's initial observation
includes it, while a process born later may wait until the next slow observation. Success and access
denial are terminal for that identity; only transient failures retry on a future slow tier. Normal
one-second samples perform neither Tool Help nor path queries. Source:
[QueryFullProcessImageNameW](https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-queryfullprocessimagenamew).

The native metadata cache holds active identities only and is capped at 8,192. The portable collector cache retains exited metadata for the configured history so recorded frames remain resolvable, also capped at 8,192; it evicts the oldest inactive entry if full and never evicts an active entry merely to admit another. Process time series use a parallel `CircularRecorder<ProcessFrame>` and never enter `SystemSample` or the UI's 300-system-sample copy. A global 600,000-process-entry history budget yields 2,000 rows/frame at the default 300 frames and 500 rows/frame at 1,200 frames (250 ms for five minutes). Excess rows are omitted from history with an observable truncation count; the current active view remains complete.

The active-process UI copies only the current frame and its active metadata, sorts display-ready rows by total-machine CPU, and shows at most 50. The UI has no telemetry or Win32 dependency. V0.0.7 incident capture requests only the newest bounded frame count capable of intersecting its pre/post window, filters again by monotonic time, flattens selected rows into the core incident domain, and copies only metadata referenced by those rows.

### Windows V0.14 extended evidence

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
disablement, and rejected frame/audio probes are specified in `TELEMETRY.md`. DNS
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

## V0.14 Windows GPU, responsiveness, power, and event evidence

V0.14 adds passive ordinary-user evidence for game/UI stutter, audio-device changes, thermal or
power transitions, and background Windows activity. It preserves two independent paths:

```text
WindowsTelemetryProvider -> Normalizer -> system ring
WindowsSystemEventProvider -> event collector -> event ring
```

The main collector never polls the event provider. Completed snapshots join only records inside
the requested monotonic window. Both rings are fixed-capacity and overwrite oldest entries while
reporting drops/overwrites.

### One-second gauges

- A persistent English PDH `GPU Engine(*)\\Utilization Percentage` query groups instances by
  physical engine and reports the busiest engine. It does not sum engines into an impossible
  utilization percentage. `GPU Adapter Memory(*)` dedicated/shared usage is summed across exposed
  adapters. Foreground GPU is the maximum engine instance belonging to the foreground PID.
- `GetForegroundWindow`, `GetWindowThreadProcessId`, and `GetProcessTimes` produce a durable
  `(PID, creation token)` only when the privacy control is enabled. No title, class, command line,
  or screen content is read or stored. Foreground correlation identifies the visible application,
  not the cause of a stall.
- Persistent Processor Information PDH counters expose `_Total` DPC/ISR time and DPC rate. High
  values can corroborate scheduling pressure relevant to real-time audio/video work, but they do
  not identify a driver or prove that an audible glitch occurred.
- `CallNtPowerInformation(ProcessorInformation)` records average current MHz, maximum MHz, and
  `MhzLimit`; the last is Windows' thermal-throttle ceiling. `GetSystemPowerStatus` records AC vs
  battery, battery fraction when available, and battery-saver state. `GetTickCount64` supplies
  uptime. Frequency and limit values describe the sampled system, not a specific core or reason.

PDH is sampled at the existing one-second normal cadence. Microsoft explicitly cautions that PDH
is not intended for collection more frequently than once per second, so these counters are not
promoted to the faster recorder profiles' fast tier.

### Discrete events

Native callbacks enqueue only source, normalized kind, severity, native event ID, optional source
UTC milliseconds, and a small numeric detail:

- `PowerRegisterSuspendResumeNotification`: suspend and automatic/user resume;
- `CM_Register_Notification`: device enumerate/start/remove;
- `IMMNotificationClient`: audio endpoint add/remove/state/default-device changes;
- `EvtSubscribe`, future events only: Service Control Manager 7031/7034/7036/7040, Defender
  1000/1001/1116-1119/5007, Windows Update 19/20/25/31/34/41/43/44, Application Error 1000,
  Application Hang 1002,
  DNS Client resolution timeout 1014, Display timeout recovery 4101, and `disk` I/O retry 153.

The callback queue holds 1,024 events, the portable ring defaults to 4,096, one drain is capped at
256, and the configurable ring hard cap is 65,536. Native callback drops, provider failures and
recoveries, poll timing, and ring overwrites are visible in Diagnostics.
The collector also maintains one aggregate counter for each bounded source family (power, device,
audio, service control manager, Defender, Windows Update, application, network, graphics, storage,
and opt-in process lifecycle). These counts
contain no native message text or identifiers and must sum exactly to the recorded-event total. They support
wall-clock device-churn qualification without weakening the privacy boundary.

### Privacy and disablement

All new privacy-sensitive sources default off for fresh product settings. Settings
separately control foreground identity, process lifecycle identity, power/device events, audio endpoint events, and selected
Windows Event Log evidence. Disabling every discrete source performs no native registration and
the base recorder continues unchanged. Reconfiguration stops and joins the event worker, clears
the event epoch/queue, applies the new gates, and restarts only if recording was active.

Process lifecycle reuses the existing one-second process enumeration and does not register another
native source. It suppresses initial inventory, post-resume/reconfigure resynchronization, and failed
or incomplete enumeration observations. Accepted start/exit rows contain only the durable PID and
creation token already used by process samples; they are context only and cannot trigger capture.

The core event type cannot store Event Log messages, device instance IDs, endpoint IDs, storage
LBAs/device paths/PDO identities, service names, threat names, update titles, queried hostnames,
crashing application/module names, exception codes, fault paths,
window titles, or any free-form payload. Dataset v1 excludes foreground identity and converts
lifecycle identity to an incident-local ordinal. Existing immutable incidents retain whatever the user previously
chose to capture until explicit retention or privacy purge.

### Researched non-adoptions

BlackBox does not use `IsHungAppWindow` or active `SendMessageTimeout` probes. Microsoft says
`IsHungAppWindow` is not intended for general use, and active probing would alter application
behavior. Future-only Application Hang event 1002 is passive and is presented as a recorded
symptom.

With automatic detection enabled, a recorded event 1002 requests capture through the same bounded
two-slot coordinator used by manual and resource triggers. The incident header retains the
`application_hang` signal, the event remains in the bounded event stream, and pipeline v13 can emit
a Windows-reported application-hang explanation without inventing resource pressure. Request,
merge, and rejection counts are visible in Diagnostics.

Future-only Application Error event 1000 is normalized as `application_crash` with only its source,
kind, level, numeric ID, and time. The application name, faulting module, exception code, fault path,
message, and event payload never enter the portable record. With automatic detection enabled, the
OS-reported crash may request capture through the same bounded coordinator. Pipeline v13 may report
only that Windows reported a nearby application crash; it cannot name the application or module or
infer the defect or any other root cause. Exact application/`application_crash`/1000 matching within
five seconds is required.

DNS Client event 1014 is recorded only as the normalized `dns_resolution_timeout` kind, warning
level, numeric event ID, and time. Its Event Log message and queried hostname are never rendered or
retained. When it falls within five seconds of an incident marker, pipeline v13 can report the
precise Windows-observed symptom. It does not call that event the root cause and does not request
automatic capture: resolver timeouts are too common and context-dependent to satisfy the current
false-positive gate by themselves. If the incident has an independently aligned resource diagnosis,
that stronger explanation wins and the DNS event remains visible context.

Future-only Display event 4101 is normalized as `display_driver_recovery` with the graphics source,
warning level, numeric event ID, and time. The event message, driver name, adapter identity, and
payload are never read into the portable event. Microsoft documents Timeout Detection and Recovery
as Windows detecting an apparently frozen GPU task, resetting the graphics stack/GPU, restoring the
desktop, and logging that the display driver stopped responding and recovered. Accordingly, with
automatic detection enabled, this OS-confirmed symptom can request capture through the same bounded
two-slot coordinator. Pipeline v13 may report only the exact Windows display-timeout-recovery
symptom; it cannot identify a faulty driver, application, GPU, or other root cause. Mismatched
source/kind/ID tuples are ignored.

Future-only provider `disk` event 153 is normalized as `storage_io_retry` with the storage source,
its numeric level/ID, and time. Its LBA, device path, PDO identity, Event Log message, and payload are
never read into the portable record. Microsoft documents event 153 as Storport reporting that a
request timed out and the I/O operation was retried. With automatic detection enabled, that exact
OS-reported symptom may request a disk-scoped capture through the bounded two-slot coordinator.
Pipeline v13 can report only that Windows retried a storage I/O operation; it cannot infer overload,
cabling, controller, driver, media, firmware, application, or hardware failure. Exact
storage/`storage_io_retry`/153 matching is required.

Continuous frame-presentation ETW and high-rate audio ETW were not adopted for the always-on base
recorder. They materially increase event volume/overhead, have workload- and driver-dependent
semantics, and would require a separate validated opt-in design. Audio endpoint transitions plus
DPC/ISR pressure are defensible context, not a direct audio-glitch counter. The UI therefore reports
frame pacing and audio-glitch automatic capture as unsupported. Endpoint transitions and DPC/ISR
pressure remain inspectable context and never become a frame/audio symptom claim.

The low-rate `DwmGetCompositionTimingInfo(NULL, ...)` alternative was also evaluated and rejected.
On the current ordinary-user Windows validation host its detailed displayed/late/dropped/missed
counters remained zero across 19 normalized intervals, and Microsoft's compatibility guidance
[*discourages the API after Windows 8*](https://learn.microsoft.com/windows/compatibility/queued-present-model-is-being-deprecated)
because its detailed queued-present model was retired. BlackBox
therefore stores none of these fields and makes no desktop-wide frame-quality claim. A future frame
signal still requires a separately bounded, opt-in design with current presentation semantics and
real workload/quiet calibration.

Primary API semantics are documented by Microsoft: [performance counters](https://learn.microsoft.com/windows/win32/perfctrs/about-performance-counters),
[audio device events](https://learn.microsoft.com/windows/win32/coreaudio/device-events),
[configuration notifications](https://learn.microsoft.com/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_register_notification),
[suspend/resume notifications](https://learn.microsoft.com/windows/desktop/api/Powerbase/nf-powerbase-powerregistersuspendresumenotification),
[Event Log subscriptions](https://learn.microsoft.com/windows/win32/wes/subscribing-to-events),
and [`PROCESSOR_POWER_INFORMATION`](https://learn.microsoft.com/windows/win32/power/processor-power-information-str).
Microsoft's [WDDM timeout detection and recovery documentation](https://learn.microsoft.com/windows-hardware/drivers/display/timeout-detection-and-recovery)
defines the recovery semantics used for the Display 4101 normalization.
Microsoft's [disk-error troubleshooting guidance](https://learn.microsoft.com/troubleshoot/windows-server/backup-and-storage/troubleshoot-data-corruption-and-disk-errors)
defines the timeout-and-retry semantics used for the `disk` event 153 normalization.
Microsoft's [application/service crash troubleshooting guidance](https://learn.microsoft.com/troubleshoot/windows-server/performance/troubleshoot-application-service-crashing-behavior)
defines Application Error event 1000 as the crash symptom used by this normalization.

## V0.13 Windows storage and network forensics

V0.13 adds ordinary-user, passive evidence for short physical-storage stalls and
network disruptions. These signals extend the existing fixed one-second system
sample; they do not create a high-rate stream, perform active probes, inspect
application payloads, or move Windows APIs outside `telemetry/windows`.

### Physical-storage layer

One persistent English PDH query reads each non-`_Total` `PhysicalDisk` instance:

- `Avg. Disk sec/Read`
- `Avg. Disk sec/Write`
- `Avg. Disk sec/Transfer`
- `Current Disk Queue Length`

The portable sample records the maximum valid value across physical instances
and an opaque numeric identity for the instance with the worst service time
(queue depth breaks ties). The query is separate from the throughput query, so
a missing or warming quality counter cannot disable disk-byte recording. Values
are seconds per operation and requests at the Windows physical-disk counter
layer. They include the effects visible at that layer—queueing, drivers,
caching, virtualization, and unrelated processes—and are not application I/O
latency or proof that a displayed process caused the stall.

PDH formatted values are accepted only when every selected counter for an
instance is valid, finite, and nonnegative. `_Total`, malformed identities, and
incomplete rows are excluded. If no instance remains, every storage-quality
field is explicitly temporarily unavailable. This source requires no elevation
on the supported validation host.

Microsoft describes these physical-disk performance-history counters as being
measured by the partition manager over the interval at the physical layer:
[Physical disk performance history](https://learn.microsoft.com/windows-server/storage/storage-spaces/performance-history-for-drives).

### Passive network layer

`GetIfTable2` supplies bounded hardware-interface identity, operational/media
state, and byte counters. BlackBox excludes loopback, tunnel, filter, endpoint,
and non-hardware rows. At most 128 selected interfaces are retained. A portable
cumulative transition counter advances whenever a retained interface appears,
disappears, or changes relevant state, or when aggregate connectivity changes;
normalization stores only the interval delta.

`GetNetworkConnectivityHint` supplies the machine-wide connectivity level:
internet, local-only, disconnected, constrained, or unknown. This is Windows'
aggregate hint, not proof that a particular host, DNS name, game server, or
application endpoint was reachable. `GetTcpStatisticsEx` supplies IPv4+IPv6
machine-wide cumulative TCP output, retransmission, failed-attempt, and
established-reset counters. BlackBox derives interval event counts and
`retransmitted / (out + retransmitted)` because Windows' output-segment count
excludes retransmissions. A nonzero denominator below eight segments is marked
temporarily unavailable to avoid a one-packet ratio claim. The result is TCP
retransmission evidence, not a direct packet-loss measurement.

Sources: [GetIfTable2](https://learn.microsoft.com/windows/win32/api/netioapi/nf-netioapi-getiftable2),
[MIB_IF_ROW2](https://learn.microsoft.com/windows/win32/api/netioapi/ns-netioapi-mib_if_row2),
[GetNetworkConnectivityHint](https://learn.microsoft.com/windows/win32/api/netioapi/nf-netioapi-getnetworkconnectivityhint),
and [MIB_TCPSTATS](https://learn.microsoft.com/windows/win32/api/tcpmib/ns-tcpmib-mib_tcpstats_w2k).

BlackBox deliberately does not report network RTT. Per-connection TCP EStats
requires enabling collection for each connection and setting it can require
administrator privilege; it also cannot establish an ordinary-user,
machine-wide, passive latency history with the current privacy model. No DNS,
remote address, Wi-Fi SSID, socket ownership, URL, packet, or payload is stored.

### Capture and analysis semantics

Balanced automatic capture still requires three observations for CPU, memory,
disk throughput, and network throughput. The following severe quality events
may capture from one one-second observation, subject to the same global cooldown:

- physical-disk service time at least 100 ms or queue depth at least 8;
- aggregate connectivity disconnected;
- constrained connectivity coincident with an interface transition;
- TCP retransmission fraction at least 25% after the eight-segment population gate;
- at least two failed TCP attempts or established resets in the interval.

Conservative and sensitive settings scale these thresholds. Missing values
never trigger. The detector records the exact signal used. Post-capture analysis
compares latency, queue, connectivity disruption, transitions, retransmissions,
failures, and resets with the incident-local baseline. One abnormal observation
can be moderate evidence; confidence still describes coverage, never causal
certainty. Incident plots keep physical storage/network evidence separate from
per-process disk throughput and repeat the correlation caveat.

### Qualification and remaining limits

Deterministic tests cover counter warm-up/reset, interface arrival/removal/state
change, missing capabilities, invalid values, cooldown, quiet-hour false
positives, direct schema-v1 creation/rejection, exact archive round-trip, analysis ranking,
and plot availability. Native integration tests exercise ordinary-user provider
sampling, loopback exclusion, protected-process failures, and a real unbuffered
disk workload. Release benchmarks and their host-specific results are retained
in `PERFORMANCE.md`; they are engineering evidence, not a universal latency or
accuracy guarantee.

V0.13 does not safely toggle a user's real adapter merely to manufacture churn.
The native current-state path plus injected lifecycle fixtures validate that
logic without disrupting connectivity. Real multi-adapter, sleep/resume, and
long wall-clock churn matrices remain release-candidate work in V0.17.

## Portable responsiveness and resource-pressure contract

### Purpose

BlackBox needs cross-platform evidence for periods where runnable work is delayed even when ordinary
utilization counters do not explain the delay. This document fixes the semantic boundary used by the
implemented Linux PSI evidence and by any future native source entering direct schema V1.

Pressure is not an alias for utilization, latency, responsiveness, or Windows DPC/ISR activity. A
provider must expose only the dimensions its native source measures and must leave every other
dimension explicitly unsupported.

### Source-neutral dimensions

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

### Availability and capability rules

- Every dimension has its own `MetricStatus`; partial native support is valid.
- A capability means the provider implements the exact cumulative-stall semantics above. File or API
  presence alone is not capability evidence.
- Permission denial is `inaccessible`; transient read/parse/reset conditions are
  `temporarily_unavailable`; a missing native semantic is `unsupported`.
- Zero is available evidence only after a successful native read and a valid delta. It is never a
  substitute for unavailable data.
- Collection is bounded, allocation-free after initialization, nonblocking, and performed through the
  platform telemetry provider. No pressure source may use the independent system-event thread.

### Platform mapping

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

### Privacy, persistence, and analysis

Pressure evidence contains only machine-wide durations/fractions and availability states. It contains
no PID, process name, cgroup, container, path, device, or workload identity. Per-cgroup PSI is out of
scope for V1.

The fields enter the direct schema-V1 snapshot and incident models once; there is no migration,
legacy reader, dual writer, or compatibility branch. Analysis may cite pressure as observed
context but must calibrate any causal claim against representative held-out incidents. Runtime ML does
not gain access merely because the feature exists.

### Implementation evidence

Linux uses strict bounded parsers for missing/duplicate/unknown records, numeric bounds, malformed
input, overflow, partial dimensions, and `some`-only CPU files. Normalizer tests cover exact deltas,
warm-up, independent reset/failure, and impossible fractions; provider-contract, direct-V1 archive
round-trip, dataset/truth export, UI availability copy, and the existing all-tier overhead benchmark
cover the remaining boundaries. The native parser is also part of the bounded libFuzzer graph.
The V0.22 Linux implementation has exact-revision hosted evidence. A future macOS cumulative-stall
implementation still requires a separately documented public source with the same semantics; the
implemented thermal and memory-pressure levels do not satisfy that contract.
