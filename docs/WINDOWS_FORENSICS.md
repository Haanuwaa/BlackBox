# V0.13 Windows storage and network forensics

V0.13 adds ordinary-user, passive evidence for short physical-storage stalls and
network disruptions. These signals extend the existing fixed one-second system
sample; they do not create a high-rate stream, perform active probes, inspect
application payloads, or move Windows APIs outside `telemetry/windows`.

## Physical-storage layer

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

## Passive network layer

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

## Capture and analysis semantics

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

## Qualification and remaining limits

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
