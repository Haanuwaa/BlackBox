# BlackBox engineering roadmap

Checkboxes describe repository state, not intent. A milestone is complete only when every acceptance and validation item is satisfied on its target platform. Performance numbers are published measurements, never estimates.

## V0.0.1 — Project bootstrap

**Objective:** Create a clean native application that configures, builds, launches, and shuts down on Windows.

Features:

- [x] Native SDL3 window and event loop
- [x] Dear ImGui context and SDL3 renderer integration
- [x] ImPlot context and placeholder plot
- [x] Placeholder BlackBox status dashboard
- [x] Replaceable, thread-safe logging sink foundation

Technical work:

- [x] Establish module-oriented repository structure
- [x] Require C++23 through target compile features and presets
- [x] Add pinned vcpkg manifest for SDL3, ImGui, ImPlot, SQLite3, and Catch2
- [x] Add MSVC Debug and Release configure/build/test presets
- [x] Isolate application lifecycle, core, and UI targets
- [x] Configure warnings and clean RAII shutdown
- [x] Create architecture, telemetry, performance, and build documentation

Acceptance criteria:

- [x] Fresh Windows checkout configures with documented prerequisites
- [x] Debug and Release application targets build with MSVC
- [x] Window launches and the status panel renders correctly
- [x] Closing the window exits without errors or leaked native contexts
- [x] Core source contains no OS, SDL, ImGui, or SQLite headers
- [x] CI-friendly test discovery succeeds

Tests and benchmarks:

- [x] Add a native unit-test executable and version smoke test
- [x] Run CTest successfully in a supported Windows toolchain
- [x] Perform a manual launch/render/close smoke test
- [x] Document that recorder performance measurement begins when collection exists

Deferred work now tracked by later product gates:

- SDL3 renderer, DPI, multi-monitor, and accessibility validation is tracked in V0.12/V0.17.
- Hosted CI execution and retained release artifacts are tracked in V0.17.

## V0.0.2 — Telemetry interfaces

**Objective:** Establish a platform-independent telemetry domain and deterministic mock source.

Features:

- [x] Define raw counter, normalized system sample, process metadata, and process sample types
- [x] Define `ITelemetryProvider` and `PlatformCapabilities`
- [x] Add deterministic mock scenarios: normal, CPU spike, disk spike, network drop, process spike
- [x] Represent unsupported, inaccessible, and temporarily failed metrics explicitly

Technical work:

- [x] Specify units and CPU semantics in types/documentation
- [x] Implement cumulative counter delta/rate normalization
- [x] Add monotonic clock abstraction where deterministic tests require it
- [x] Design tier-aware sampling inputs without prematurely building the scheduler
- [x] Ensure public core/telemetry headers compile without platform headers

Acceptance criteria:

- [x] Mock raw snapshots produce stable normalized results across compilers
- [x] First observations, zero/negative elapsed time, resets, and wraps have defined behavior
- [x] Provider capability changes do not crash consumers
- [x] No allocations are required by scalar normalization hot paths

Tests and benchmarks:

- [x] Unit-test percentages, rates, reset/wrap handling, time anomalies, and unavailable values
- [x] Compile boundary test rejects accidental Windows dependencies in core targets
- [x] Establish a small normalization microbenchmark baseline

Risks:

- Counter wrap width and reset semantics vary by source; do not infer wrap without evidence.
- A single sample structure can become sparse as capabilities grow; review representation cost before freezing it.

## V0.0.3 — Windows CPU and memory telemetry

**Objective:** Collect lightweight, normalized Windows system CPU and memory data.

Features:

- [x] Implement Windows provider for total CPU and physical memory
- [x] Show live CPU, used/total RAM, and RAM percentage
- [x] Expose collection timing diagnostics

Technical work:

- [x] Validate `GetSystemTimes` semantics, topology behavior, and counter math
- [x] Validate `GlobalMemoryStatusEx` definitions
- [x] Keep all Win32 headers in `telemetry/windows`
- [x] Define recoverable provider-error behavior and logging rate limits

Acceptance criteria:

- [x] Core features run without administrator rights on supported Windows versions
- [x] CPU output agrees within documented tolerance with a trusted reference during idle and load
- [x] Memory values remain internally consistent
- [x] Provider failures degrade data availability without stopping collection

Tests and benchmarks:

- [x] Unit-test Windows counter conversion with captured fixtures
- [x] Integration-test real provider startup/sample/shutdown
- [x] Measure average, P95, P99, and maximum provider time
- [x] Publish first CPU and working-set baseline

Risks:

- Reference tools may use different averaging and CPU-capacity semantics.
- Virtualization, CPU groups, sleep/resume, and topology changes need explicit validation.

## V0.0.4 — Circular recorder

**Objective:** Continuously retain a configurable, bounded telemetry history in RAM.

Features:

- [x] Fixed-capacity circular buffer
- [x] Collector `std::jthread` with cooperative shutdown
- [x] Default one-second sampling and five-minute history
- [x] Rolling graphs and collection statistics

Technical work:

- [x] Define capacity/configuration validation and overflow policy
- [x] Implement chronological immutable snapshots
- [x] Schedule from monotonic deadlines and specify late/drop/catch-up behavior
- [x] Bound UI snapshot work and lock duration
- [x] Add collection count, failures, dropped samples, jitter, and ring utilization

Acceptance criteria:

- [x] Recorder operates headless with UI, SQLite, and analysis absent
- [x] Steady-state memory is bounded and normal recording performs no disk writes
- [x] Clean shutdown works during sampling and while readers request snapshots
- [x] Configuration supports future 500 ms and 250 ms intervals without interface redesign

Tests and benchmarks:

- [x] Test empty/partial/full/wrapped buffers and capacities 0/1/N
- [x] Test ordering, overwrite, snapshot, configuration changes, and shutdown races
- [x] Soak past multiple wrap cycles with mock telemetry
- [x] Measure append/snapshot cost, jitter, memory, and dropped samples

Risks:

- Per-process history can dominate memory; establish a capacity model before adding it.
- UI copies can cause periodic spikes if snapshots are oversized.

## V0.0.5 — Disk and network telemetry

**Objective:** Record system disk and network throughput using validated cumulative counters.

Features:

- [x] Disk read/write bytes per second
- [x] Network receive/transmit bytes per second
- [x] Live and rolling UI graphs
- [x] Explicit counter reset/interface lifecycle handling

Technical work:

- [x] Benchmark PDH versus appropriate native Windows sources for disk totals
- [x] Define physical/logical disk and virtual/loopback interface aggregation policy
- [x] Validate `GetIfTable2` or selected IP Helper source
- [x] Evaluate disk latency; include only if reliable and inexpensive

Acceptance criteria:

- [x] Throughput matches controlled file/network workloads within documented tolerance
- [x] Adapter/disk arrival, removal, reset, sleep, and resume produce no false spikes
- [x] Metrics remain usable without administrator rights where possible
- [x] Unsupported disk latency remains explicitly unavailable

Tests and benchmarks:

- [x] Fixture tests for deltas, reset/wrap, interface churn, and zero intervals
- [x] Controlled disk and loopback/LAN integration tests
- [x] Measure provider cost and collector deadline margin

Risks:

- Aggregate Windows counter sources differ in caching and device coverage.
- VPN and virtual adapters can double-count traffic without a clear policy.

## V0.0.6 — Process telemetry

**Objective:** Identify processes active around incidents without excessive enumeration or metadata cost.

Features:

- [x] Process enumeration and lifecycle tracking
- [x] Cached PID, parent PID, name, and path metadata
- [x] Per-process CPU, working set, and disk read/write rates
- [x] Active-process UI table

Technical work:

- [x] Use PID plus creation time/token as process identity
- [x] Benchmark Tool Help, per-process query APIs, PDH, and viable alternatives
- [x] Separate slow metadata resolution from normal samples
- [x] Bound metadata cache after process exit/history expiry
- [x] Handle protected, short-lived, and inaccessible processes without retry storms

Acceptance criteria:

- [x] PID reuse never joins samples from different process instances
- [x] Executable paths are not resolved on every sample
- [x] Process exit during sampling is a normal recoverable condition
- [x] Cost remains within documented budgets at representative process counts

Tests and benchmarks:

- [x] Deterministic lifecycle/PID-reuse/cache tests
- [x] Integration fixture spawning short-lived processes with CPU, memory, and I/O loads
- [x] Benchmark at approximately 50/200/500 processes
- [x] Measure cache size, per-process cost, P95/P99 collection, and failures

Risks:

- Per-process API calls may exceed the one-second budget on busy systems.
- Privilege boundaries create expected gaps that UI/analysis must preserve.

## V0.0.7 — Manual incident capture

**Objective:** Convert rolling history into manual incident snapshots with pre/post context.

Features:

- [x] Windows global hotkey, default `Ctrl+Shift+F12`
- [x] UI capture button and visible capture state
- [x] Configurable pre-window (default 120 s) and post-window (default 30 s)
- [x] Immutable incident snapshot and bounded writer work queue

Technical work:

- [x] Define platform hotkey interface outside Win32 code
- [x] Specify overlapping capture merge/queue behavior
- [x] Preserve process metadata referenced by the selected window
- [x] Keep snapshot creation bounded and collector-safe

Acceptance criteria:

- [x] Hotkey works without administrator rights and unregisters cleanly
- [x] Captured time range is correct at ring boundaries and short uptime
- [x] Collector continues throughout post-window and snapshot enqueue
- [x] Queue saturation/failure is visible and cannot exhaust memory

Tests and benchmarks:

- [x] Fake-clock tests for early, normal, overlapping, and shutdown captures
- [x] Windows hotkey integration/manual validation
- [x] Benchmark snapshot construction and collector jitter

Risks:

- The default hotkey may conflict with other software.
- Large process windows can cause transient memory duplication.

## V0.0.8 — SQLite persistence

**Objective:** Persist completed incident snapshots without writing normal rolling telemetry.

Features:

- [x] SQLite archive with schema metadata and a direct pre-release schema-v1 baseline
- [x] Incident, system sample, process metadata, and process sample storage
- [x] Asynchronous writer thread and observable failure state
- [x] Restart-safe incident discovery

Technical work:

- [x] Define normalized schema, indexes, foreign keys, and units
- [x] Use prepared statements and one transaction per incident
- [x] Define database location, durability mode, size policy, and recovery behavior
- [x] Create the complete schema transactionally and reject incompatible archives unchanged

Acceptance criteria:

- [x] Normal recording produces effectively zero telemetry database writes
- [x] Collection continues through database lock/full/path/open failures
- [x] Stored incidents round-trip without unit, ordering, or identity loss
- [x] Current schema round-trips representative fixtures and rejects non-v1 archives safely

Tests and benchmarks:

- [x] In-memory schema and repository tests
- [x] Direct-creation, incompatible-version, and unversioned-nonempty rejection tests
- [x] Failure injection for busy, full, corrupt, and unavailable database states
- [x] Measure writer latency, database size, temporary memory, and collector jitter

Risks:

- Durability settings trade write latency against crash guarantees.
- Process samples can make archives large; retention/export policy will be needed.

## V0.0.9 — Incident viewer

**Objective:** Make captured incidents inspectable and searchable.

Features:

- [x] Incident list with timestamp, duration, label, and note
- [x] Detail timeline with system graphs and incident marker
- [x] Process table/graphs with filtering and sorting
- [x] Basic label and note editing

Technical work:

- [x] Query incidents lazily and avoid loading unbounded archives into UI memory
- [x] Downsample/decimate only when required and without hiding narrow spikes
- [x] Keep database work off the render loop
- [x] Define timezone display while retaining unambiguous stored timestamps

Acceptance criteria:

- [x] Viewer accurately distinguishes pre/post windows and unavailable metrics
- [x] Large incidents remain responsive within a documented budget
- [x] Notes/labels persist across restart
- [x] Recorder continues while incidents are viewed

Tests and benchmarks:

- [x] View-model tests for filtering, sorting, ranges, and missing capabilities
- [x] UI smoke test with representative and large fixture databases
- [x] Measure query latency, frame time, and peak memory

Risks:

- Plotting all process series can overwhelm rendering and obscure useful data.
- Downsampling can erase exactly the short spikes BlackBox targets.

## V0.1 — Stable Windows Flight Recorder

**Objective:** Ship a genuinely useful, lightweight manual Windows recorder without ML.

Features:

- [x] Reliable rolling system/process telemetry
- [x] Manual hotkey and UI incident capture
- [x] SQLite archive and usable incident viewer
- [x] Recorder diagnostics and documented configuration

Technical work:

- [x] Harden startup, shutdown, sleep/resume, and error recovery
- [x] Confirm core operation does not require administrator rights
- [x] Complete user/build/architecture documentation
- [x] Establish CI, release packaging, and representative fixture data

Acceptance criteria:

- [x] All V0.0.x acceptance criteria pass on supported Windows versions
- [x] No ML or analysis dependency exists in collection
- [x] Normal telemetry remains RAM-only
- [x] Known gaps and unavailable protected-process data are clearly represented

Tests and benchmarks:

- [x] Publish idle/recording CPU and RAM
- [x] Publish average/P95/P99 collection latency, jitter, and dropped samples
- [x] Publish incident serialization latency and representative database size
- [x] Complete long soak, repeated capture, sleep/resume, and failure tests
- [x] Compare results against targets in `docs/PERFORMANCE.md`

Risks:

- Anti-cheat/security products may restrict process inspection.
- Hardware and Windows-version variance require a defined support/test matrix.

V0.1 completion evidence (2026-08-18):

- Version `0.1.0` passed 86/86 Debug tests, 86/86 Release tests, and 65/65
  UI/SQLite-disabled Release tests on Windows build 10.0.26200 with MSVC 19.51. The
  architecture test forbids analysis/app/storage/UI includes in portable telemetry.
- An ordinary-user (`elevated=0`) 65-second real-provider profile collected 66 samples across
  117 accessible processes with 0 failures, deadline misses, or dropped ticks. Collection
  average/P95/P99 was 9.720/11.264/13.546 ms; jitter was 0.664/1.310/1.514 ms.
- Headless recording used 0.130% total-machine CPU and 14.42 MiB average working set. The
  minimized packaged app used 0.042% CPU and 52.41 MiB average working set. CPU goals pass; the
  app is 2.41 MiB above the initial 50 MiB working-set target and remains a documented follow-up.
- The 2,000-sample concurrent soak crossed 400 ring wraps; 100 sequential captures drained
  through the bounded writer. Resume-gap, provider exception/recovery, transient writer recovery,
  lock/full/path/corrupt archive, viewer isolation, and shutdown drain/cancel paths are automated.
- Five-transaction 50/200/500-process serialization averages were 26.858/107.753/321.969 ms;
  representative single-incident databases were 0.85/3.21/8.22 MiB. The generated three-incident
  fixture archive is 9.28 MiB.
- CPack produced the approximately 3.0 MiB `BlackBox-0.1.0-windows-x64.zip` with `blackbox.exe`, SDL3,
  SQLite, and documentation. An isolated packaged launch recorded minimized and exited cleanly.
  Windows CI now covers Debug, Release, headless collection, fixtures, tests, and the ZIP artifact.

## V0.2 — Statistical anomaly detection

**Objective:** Rank what was unusual using explainable statistical baselines.

Features and technical work:

- [x] Add rolling robust baselines, percentiles, and robust z-scores
- [x] Score CPU, memory, disk, and network anomalies
- [x] Rank incident-local resource and process anomalies
- [x] Expose evidence and uncertainty through an analysis interface

Acceptance criteria and validation:

- [x] Recorder operates identically with analysis disabled
- [x] Scores are deterministic for fixed incidents and tolerate missing metrics
- [x] Synthetic spike scenarios rank the injected resource/process correctly
- [x] Benchmark analysis time/memory and false positives on normal fixtures

Risks: Non-stationary workloads and tiny baselines can create misleading confidence; cold-start output must be explicit.

V0.2 completion evidence (2026-08-18):

- Version `0.2.0` passed 94/94 Debug tests, 94/94 Release tests, 86/86 full-app
  analysis-disabled Release tests, and 65/65 UI/storage/analysis-disabled headless Release tests.
  The architecture check enforces the core-only analysis boundary and prevents collection, storage,
  platform, and UI dependencies from entering `blackbox_analysis`.
- Fixed incidents produce byte-for-byte-equivalent ordered evidence across repeated analyses. Missing
  values are excluded with explicit coverage, and short histories report cold start instead of
  fabricating a baseline. Normal 50/200/500-process fixtures had maximum anomaly score `0.000`.
- Deterministic CPU, memory, disk, network, and process-CPU spikes each ranked the injected resource
  or full PID/creation-token identity first with score above `0.99` and high confidence.
- Ten-trial 50/200/500-process Release averages were 4.523/20.731/56.637 ms; nearest-rank
  P95/P99/max were 5.125/23.102/62.965 ms. Peak temporary working-set deltas were
  0.50/1.69/4.88 MiB. The 75,000-process-row integration fixture was analyzed while a one-millisecond
  collector continued with zero failures.
- CPack produced the approximately 3.03 MiB `BlackBox-0.2.0-windows-x64.zip` with the application,
  runtime DLLs, and statistical-analysis documentation. An isolated packaged launch opened and
  exited cleanly with status zero.

## V0.3 — Personalized process baselines

**Objective:** Learn bounded, aging normal profiles for individual executables.

Features and technical work:

- [x] Persist per-executable time-window statistics
- [x] Add process-specific scoring, cold-start behavior, and baseline aging
- [x] Normalize executable identity and cap stored history/cardinality

Acceptance criteria and validation:

- [x] Compiler-like high CPU can become normal without normalizing unrelated executables
- [x] Renames, upgrades, missing paths, and hash/signature policy have defined behavior
- [x] Baseline storage remains bounded in soak/cardinality tests
- [x] Benchmark update/query cost and database growth

Risks: Executable identity is ambiguous; poisoned baselines can learn persistent faults as normal.

V0.3 completion evidence (2026-08-18):

- Version `0.3.0` passed 102/102 Debug tests, 102/102 Release tests, 89/89 full-app
  analysis-disabled Release tests, and 65/65 UI/storage/analysis-disabled headless Release tests.
  The architecture test confirms analysis and storage remain separate core-only consumers while
  the application-owned viewer worker maps portable profile context and updates.
- The complete pre-release schema-v1 baseline persists one idempotent statistic per
  executable/incident. Only prior observations are queried; reopening an incident does not learn it
  twice, and profile failures fall back to incident-local analysis without affecting collection.
- A deterministic acceptance fixture makes repeated 80%-CPU compiler behavior score `0.000` while
  the same observation for an unrelated executable scores above `0.99` and ranks first. Missing or
  older-than-30-day history reports explicit profile cold start.
- Tests define normalized-path preference, separately namespaced name fallback, rename cold start,
  same-path upgrade sharing, 2,048-byte key refusal, and the explicit absence of hash/signature
  identity. Cardinality soak exceeds 2,048 keys and 64 observations per key, then verifies the
  2,048-key, 64-per-key, and 131,072-row ceilings.
- Ten-trial 50/200/500-executable Release update averages were 2.643/5.404/8.348 ms and P95/max
  3.168/6.996/10.605 ms. Query averages were 0.677/2.456/6.221 ms and P95/max
  1.097/4.756/11.391 ms. Ten observations per executable produced logical databases of
  0.24/0.69/1.59 MiB.
- CPack produced the approximately 3.06 MiB `BlackBox-0.3.0-windows-x64.zip` with executable,
  runtime DLLs, schema/profile documentation, and the completed roadmap. An isolated packaged
  launch created a valid archive and exited cleanly with status zero. The current pre-release build
  creates the complete layout directly as schema version 1.

## V0.4 — Automatic incident detection

**Objective:** Capture obvious incidents automatically while retaining manual capture.

Features and technical work:

- [x] Add threshold/statistical triggers for severe resource events
- [x] Add cooldown, deduplication, and overlapping-capture policy
- [x] Ask users whether they noticed a problem and retain feedback
- [x] Keep detection optional and lightweight

Acceptance criteria and validation:

- [x] Deterministic scenarios trigger within a bounded detection delay
- [x] Normal fixtures stay within a documented false-positive budget
- [x] Trigger storms cannot exhaust memory or storage
- [x] Benchmark detector overhead on the collector path

Risks: Aggressive triggers create alert fatigue and archive growth; conservative triggers miss brief symptoms.

V0.4 completion evidence (2026-08-18):

- Version `0.4.0` passed 112/112 Debug tests, 112/112 Release tests, 99/99 full-app
  analysis-disabled Release tests, and 67/67 UI/storage/analysis/detection-disabled headless
  Release tests. The architecture test still enforces the core/telemetry/storage/analysis/UI and
  OS-header boundaries.
- Fixed rolling baselines and conservative hard/statistical rules cover CPU, memory, aggregate
  disk, and aggregate network. Deterministic fixtures trigger on the third qualifying observation;
  unavailable values break confirmation and never become zero.
- A global 120-second cooldown emits at most nine requests during a 1,000-second continuous severe
  fixture. Automatic/manual overlap retains both trigger counts and strongest automatic evidence.
  Every request still uses the two-slot source FIFO, one writer in-flight bound, and 1 GiB archive
  cap; saturation is rejected and observable.
- The complete pre-release schema-v1 baseline includes manual/automatic provenance plus
  automatic provenance plus unanswered/noticed/not-noticed feedback. The viewer asks only for
  automatic incidents, saves on its worker, and round-trips feedback across archive reopen.
- An 86,400-sample smooth normal fixture (one default-cadence day) produced zero triggers, meeting
  the documented deterministic false-positive budget. Five 2,000,000-sample Release benchmark
  trials measured 267.028-284.518 ns per collector observation, median 279.181 ns, with no trigger.
- CPack produced the approximately 3.07 MiB `BlackBox-0.4.0-windows-x64.zip` with the application,
  runtime DLLs, schema/detection documentation, and completed roadmap.

## V0.5 — Incident classification

**Objective:** Capture user-defined incident categories and feedback for future supervised evaluation.

Features and technical work:

- [x] Add System freeze, Game stutter, Application slowdown/hang, Network, Audio, and Unknown labels
- [x] Store label history/feedback in the direct schema-v1 baseline
- [x] Define privacy-preserving, versioned offline dataset export

Acceptance criteria and validation:

- [x] Labels survive upgrade/export/import without changing recorded telemetry
- [x] Export contains schema/version/unit metadata and no unintended private paths
- [x] Schema-creation, UI, and round-trip tests pass

Risks: User labels are sparse/noisy; executable paths and notes can contain sensitive data.

V0.5 completion evidence (2026-08-18):

- Version `0.5.0` passed 116/116 Debug tests, 116/116 Release tests, 103/103 full-app
  analysis-disabled Release tests, and 67/67 UI/storage/analysis/detection-disabled headless
  Release tests. The architecture check still enforces the portable core/telemetry/analysis,
  storage, UI, app, and OS boundaries; classification remains entirely post-capture.
- The viewer exposes the six fixed categories through primitive UI state and saves them on its
  existing worker together with noticed/not-noticed feedback. The direct schema-v1 baseline assigns
  random 128-bit export keys, records capture/user/dataset-import origin, and retains at most 64
  change-only events per incident.
- Dataset format v1 writes manifest/version/unit/status/privacy metadata, incident-relative system
  telemetry, process telemetry keyed only by incident-local ordinal, current classification, and
  bounded classification history. It excludes archive/executable paths, process names, PID/parent
  PID/creation tokens, and free-form labels/notes. Export refuses existing destinations.
- Automated privacy and round-trip tests changed category/feedback through export/import, verified
  byte-for-byte-equivalent telemetry domains before/after, rejected overwrite, found no seeded
  private path/name/note/label, and confirmed unchanged reimport creates no history event.
- The Release CLI exported the three-incident representative archive (302 system and 82,501
  process samples), then matched all three classifications with zero idempotent updates. CPack
  produced `BlackBox-0.5.0-windows-x64.zip` with the app, dataset tool, runtime DLLs, and dataset
  privacy/specification documentation.

## V0.6 — Cause candidate ranking

**Objective:** Rank plausible contributors without presenting correlation as causation.

Features and technical work:

- [x] Combine anomaly magnitude, timing, resource match, duration, and recurrence
- [x] Rank candidate processes/events with evidence and calibrated wording
- [x] Separate preceding activity, marker-spanning ambiguity, and post-marker victims/reactions

Acceptance criteria and validation:

- [x] Output says potential/likely contributor, never proven cause without causal evidence
- [x] Injected fixtures rank the intended preceding process above unrelated followers
- [x] Missing process metrics degrade confidence rather than fabricate ranks
- [x] Evaluate ranking quality on labeled fixtures and benchmark latency

Risks: Confounders and inaccurate user timestamps can invert temporal interpretation.

V0.6 completion evidence (2026-08-19):

- Version `0.6.0` passed 121/121 Debug tests, 121/121 Release tests, 103/103 full-app
  analysis-disabled Release tests, and 67/67 UI/storage/analysis/detection-disabled headless
  Release tests. Architecture enforcement preserves the core-only optional analysis boundary.
- The portable ranker combines inspectable anomaly, timing, same-direction resource, duration, and
  recurrence factors, then degrades its score and confidence with missing evidence. It scans the
  bounded incident process rows once and returns no more than 20 candidates.
- The public result has only `potential` and `likely` causal-strength states. Temporal role is
  separate: the UI labels genuinely preceding activity, marker-spanning ambiguity, and wholly
  post-marker possible victims/reactions. Only predominantly preceding activity can be promoted
  to likely.
- Labeled CPU, memory, and disk fixtures achieved 3/3 intended top ranks over equally anomalous
  unrelated followers. Missing-metric, recurrence, maximum-result, direction-match, and calibrated
  wording behavior is covered by deterministic tests.
- Twenty-trial 50/200/500-process Release ranking averages were 1.060/3.906/8.299 ms for
  7,500/30,000/75,000 process rows; P95 was 1.285/5.313/8.698 ms and maximum was
  1.286/5.383/8.823 ms. Full analysis at 500 processes averaged 53.430 ms.
- CPack produced `BlackBox-0.6.0-windows-x64.zip` with the app, dataset tool, runtime DLLs, and
  contributor-ranking documentation.

## V0.7 — Recurring incident discovery

**Objective:** Group repeated incident shapes and expose their common evidence.

Features and technical work:

- [x] Define versioned incident feature vectors and scaling
- [x] Cluster incidents locally and handle noise/outliers
- [x] Show cluster members, occurrence counts, and shared characteristics
- [x] Update clusters without unbounded recomputation

Acceptance criteria and validation:

- [x] Similar deterministic scenarios group together across capture dates
- [x] Dissimilar resource patterns remain separable at documented thresholds
- [x] Users can inspect and override misleading groupings
- [x] Benchmark clustering time, memory, and stability as archive size grows

Risks: Feature scaling dominates cluster meaning; algorithm/version changes can destabilize user-visible groups.

V0.7 completion evidence (2026-08-19):

- Version `0.7.0` passed 129/129 Debug tests, 129/129 Release tests, 106/106 full-app
  analysis-disabled Release tests, and 67/67 UI/storage/analysis/detection-disabled headless
  Release tests. Architecture checks preserve the portable optional-analysis and collector
  boundaries.
- Feature version 1 defines twelve explicitly scaled CPU, memory, disk, network, temporal, duration,
  and resource-concentration dimensions with independent availability. Distance penalizes unequal
  missingness and refuses weak overlap instead of converting missing signals to zero.
- Deterministic complete-link threshold grouping considers at most the newest 512 valid incidents,
  requires two automatic members, exposes unmatched incidents as noise, and publishes occurrence
  counts, member links, shared medians/support, and maximum pair distance.
- Labeled CPU and disk families across different capture dates formed the intended separate
  three-member groups while a balanced shape remained noise. Reversing 512 inputs produced an
  identical result in deterministic stability tests.
- The direct schema-v1 baseline caches generic versioned feature rows and retains 64-byte manual group
  overrides. Viewer-worker tests recomputed two uncached incidents once, reused both cache entries,
  forced statistically dissimilar incidents into a visible user group, and restored automatic
  grouping after the override was cleared.
- Twenty-trial 32/128/512-incident Release averages were 0.078/0.526/7.240 ms; P95 was
  0.100/0.589/8.007 ms and maximum was 0.124/0.785/11.538 ms. All trials were stable; input object
  payload was 5,376/21,504/86,016 bytes and the largest observed temporary working-set delta was
  143,360 bytes.
- CPack produced `BlackBox-0.7.0-windows-x64.zip` with the application, dataset tool, runtime DLLs,
  and recurring-discovery specification.

## V0.8 — Context recognition

**Objective:** Recognize broad workloads so anomaly scoring reflects what the machine is doing.

Features and technical work:

- [x] Classify Idle, Gaming, Development, Compilation, Video playback/call, Heavy download, Desktop, and Unknown
- [x] Start with explainable deterministic/statistical signals
- [x] Feed context probabilities, not hard assumptions, into anomaly scoring

Acceptance criteria and validation:

- [x] Context is optional and recorder-independent
- [x] Ambiguous workloads retain Unknown/uncertainty
- [x] Context improves held-out anomaly ranking without unacceptable regressions
- [x] Benchmark recognition overhead and confusion matrix on labeled fixtures

Risks: Context can expose user activity and can bias diagnosis when wrong.

V0.8 completion evidence (2026-08-19):

- Version `0.8.0` passed 136/136 Debug tests, 136/136 Release tests, 106/106 full-app
  analysis-disabled Release tests, and 67/67 UI/storage/analysis/detection-disabled headless
  Release tests. The architecture checks and separate graphs preserve the portable optional-analysis
  and recorder boundaries.
- `blackbox_analysis` emits a deterministic probability distribution over Unknown plus seven broad
  workloads from normalized incident-wide resource averages and at most 512 already-recorded process
  metadata rows. Missing or closely competing evidence selects Unknown; derived context is not
  persisted, so the direct schema-v1 storage layout needs no additional field.
- Raw statistical/personalized evidence is retained separately. The complete probability distribution
  applies a resource-expectation multiplier with a 20% default reduction and a validated 25% absolute
  cap; the UI shows probabilities, uncertainty, bounded signals, raw score, and applied multiplier.
- Two labeled variants per class produced a 16/16 confusion matrix, including 2/2 Unknown fixtures.
  Six held-out synthetic ranking scenarios improved from 0/6 to 6/6 top-one, while six protected
  already-correct scenarios recorded zero regressions. These fixtures validate mechanics and
  guardrails rather than claim real-world calibration.
- On the development host, 4,000 Visual Studio 2026 x64 Release recognitions of 150-sample fixtures
  measured 11.178 microseconds average, 13.600 microseconds P95, 20.300 microseconds P99, and
  42.400 microseconds maximum. The benchmark prints the full confusion and regression matrices.
- `docs/CONTEXT_RECOGNITION.md` documents signals, probability/Unknown behavior, adjustment weights,
  bounds, privacy/bias risks, validation, and reproduction. CPack produced
  `BlackBox-0.8.0-windows-x64.zip`.

## V0.9 — Intelligent analysis pipeline

**Objective:** Combine anomalies, personalization, context, recurrence, and contributor ranking into coherent local diagnoses.

Features and technical work:

- [x] Define a versioned `IIncidentAnalyzer` pipeline and evidence model
- [x] Produce incident type, ranked contributor, evidence, and calibrated confidence
- [x] Add optional native ML inference only if evaluation proves material improvement
- [x] Keep model training/tools outside the shipped runtime

Acceptance criteria and validation:

- [x] Every conclusion links to inspectable recorded evidence
- [x] Analysis versions are reproducible for a fixed incident/configuration
- [x] Disabling ML preserves statistical diagnosis and collection
- [x] Compare quality, latency, memory, binary size, and failure modes before adopting ONNX Runtime

Risks: Combining correlated scores can inflate confidence; model dependencies can violate footprint goals.

V0.9 completion evidence (2026-08-19):

- Version `0.9.0` passed 145/145 Debug tests, 145/145 Release tests, 106/106 full-app
  analysis-disabled Release tests, and 67/67 UI/storage/analysis/detection-disabled headless
  Release tests. Architecture checks keep the pipeline post-capture and portable; collection,
  recording, persistence, and the direct schema-v1 storage layout are unchanged.
- Pipeline/evidence model v1 deterministically composes the established bounded components. A stable
  field-wise configuration fingerprint identifies the exact configuration, while typed links
  reference resource, process, contributor, workload, recurrence, and automatic-trigger evidence.
  Link validation rejects dangling, non-finite, unavailable, and manually fabricated recurrence
  references. Repeated fixed incident/context/configuration runs produced identical result values.
- Diagnoses distinguish CPU, memory, storage, network, and multi-resource pressure patterns or retain
  Unknown when support is insufficient. Confidence incorporates evidence coverage, weights contributor
  confidence, caps contextual inputs, and subtracts an explicit overlap penalty; process evidence is
  linked without double counting. The UI describes contributors as correlations, not proven causes.
- Controlled Release fixtures diagnosed 4/4 labeled resource incidents with 0/1 quiet false positives.
  At 50/200/500 processes, component versus composed-pipeline averages were 4.928/4.897,
  21.298/21.171, and 57.404/57.300 ms; pipeline P95 was 5.284/21.991/58.630 ms and peak temporary
  memory was 552,960/1,798,144/4,485,120 bytes. The small negative measured overheads are timer noise.
- Native ML and ONNX Runtime were not adopted: there is no representative held-out labeled dataset
  demonstrating a material quality improvement, so the dependency, model loading, compatibility,
  memory, binary, and failure paths are not justified. The manifest and shipped runtime contain no
  inference dependency, model, training tool, or Python runtime; provenance reports `not_adopted`.
- The Release executable is 10,066,944 bytes versus 9,923,072 bytes in the V0.8 package, an
  incremental 143,872-byte (140.5 KiB) cost. The analysis-disabled executable is 1,071,616 bytes.
  `docs/INTELLIGENT_ANALYSIS.md` records calibration, failure behavior, exact benchmarks, and the
  future ML adoption gate. CPack produced `BlackBox-0.9.0-windows-x64.zip`.

## V0.10 — Roadmap reset and incident-persistence resilience

**Objective:** Correct the premature V1.0 designation and harden accepted incident persistence
without weakening the bounded recorder or claiming product validation from controlled fixtures.

Features and engineering work:

- [x] Return the application/package version to an explicit pre-1.0 engineering milestone
- [x] Separate implemented mechanisms from clean-client, wall-clock, usability, and signing gates
- [x] Retry transient busy/I/O incident writes with bounded attempts and bounded backoff
- [x] Keep permanent archive failures immediate and preserve the fixed incident memory bound
- [x] Expose retry, exhaustion, recovery, and last-error diagnostics to the application/UI
- [x] Document the retry/error contract and retain explicit user-only archive deletion

Acceptance criteria and validation:

- [x] A transient first write failure persists the same immutable incident on retry
- [x] Persistent transient failure exhausts exactly the configured bound
- [x] Full/corrupt/schema/configuration failures cannot retry-storm
- [x] Collection remains independent while storage retries or fails
- [x] Debug, Release, analysis-disabled, and fully headless test graphs pass
- [x] Package and documentation consistently identify version 0.10.0 as pre-1.0

Historical correction:

- The code previously labeled `1.0.0` established valuable engineering mechanisms: validated
  recorder profiles, explicit retention/purge, provider conformance, architecture-disabled build
  graphs, release scripts, an accelerated seven-day simulation, controlled benchmarks, and broad
  deterministic tests. Those results remain evidence for their narrow claims.
- They did not prove a polished tray/background product, real-world diagnosis quality, a wall-clock
  multi-day soak, clean Windows 10/11 client qualification, accessibility/DPI behavior, hosted CI
  execution, or a signed official package. V1.0 is reserved until those gates are actually met.

V0.10 completion evidence (2026-08-20):

- Version `0.10.0` passed 153/153 Debug tests, 153/153 Release tests, 113/113 full-app
  analysis-disabled Release tests, and 69/69 UI/storage/analysis/detection-disabled headless
  Release tests. The architecture boundary checker passed in every applicable graph.
- The writer retains only its current immutable incident and uses a validated default maximum of
  three store attempts with 25 ms then 50 ms backoff. Only `busy` and I/O failures retry. A
  deterministic transient fixture failed once then stored the same incident; persistent busy
  failure exhausted exactly three attempts; full storage made exactly one attempt; invalid attempt
  and delay bounds were rejected.
- Retry attempts, exhaustion, recovery, current activity, and the last native error are observable
  through writer diagnostics and primitive dashboard state. Collector-independence and zero normal
  recording database work tests remained green.
- README, build, release-qualification, configuration, storage, performance, architecture, package,
  and version-test surfaces identify the code as pre-1.0. CPack produced
  `BlackBox-0.10.0-windows-x64.zip`; package contents and its generated SHA-256 sidecar verified.
  The developer binary remains deliberately unsigned and therefore is not an official V1.0
  candidate.

## V0.11 — Native background recorder shell

**Objective:** Make BlackBox behave like the quiet Windows flight recorder described by the vision.

- [x] Add a native tray icon with recording, capture, retry, and error states
- [x] Add show/hide, capture, pause/resume, and exit tray commands
- [x] Make close-to-tray behavior explicit and preserve clean full shutdown from Exit
- [x] Add optional, user-controlled launch at login without elevation
- [x] Add capture-started/completed/failed notifications with quiet controls
- [x] Prove hidden/minimized CPU, memory, and wakeup overhead remains within published bounds
- [x] Test Explorer restart, duplicate launch, hotkey conflict, suspend/resume, and shutdown races

V0.11 completion evidence (2026-08-20):

- `IBackgroundShell` is a portable command/status/diagnostics boundary. Only
  `platform/windows` owns the notification-area icon, hidden message window, Explorer recovery,
  current-user Run value, named instance mutex, and Windows end-session handling. The shell has no
  telemetry, recorder, storage, analysis, SDL, or UI dependency; callbacks set a bounded command
  bit for the application composition root.
- The tray exposes show/hide, capture, pause/resume, start-with-Windows, notification quieting, and
  Exit. Close-to-tray is used only after tray creation succeeds; otherwise close exits instead of
  stranding an invisible process. Resume preserves ring history but resets delta/detector baselines.
  Capture start/extension, archive success, and rejected/failed storage notifications contain no
  telemetry/process names and use the no-sound notification mode.
- Deterministic Windows tests cover command dispatch, Explorer `TaskbarCreated`, removable HKCU
  startup state, duplicate activation, hotkey ownership conflict, pause/resume baseline recovery,
  queued-command shutdown, and Windows end-session exit. An assembled-app smoke used isolated app
  data: the first hidden instance exited cleanly, while a second launch activated it and exited with
  code zero in 17 ms.
- The single-slot notification mailbox posts at most one native message while a payload is pending.
  A newer burst payload replaces and counts the displaced one without queuing a second empty balloon;
  a deterministic blocked-shell-thread test proves three calls become exactly one delivery attempt
  plus two coalesced drops.
- Repeated application status refreshes post no tray message unless the finite status actually
  transitions. A failed native post restores the prior value only if no concurrent newer state won,
  retaining retry behavior. A deterministic test proves pre-start state retention, 128 unchanged
  refreshes with zero posts, and exactly one post for each subsequent transition.
- On the documented 12-logical-processor Release host, 30-second post-warm-up hidden/minimized
  profiles used 0.133784%/0.133812% total-machine CPU, 49.590/56.426 MiB maximum working set, and
  57.770/63.059 MiB maximum private bytes. Both pass the published V0.11 0.5% CPU, 60 MiB working-
  set, and 65 MiB private-byte bounds. Hidden/minimized code schedules at most four UI timeout
  opportunities plus one default collector observation per second and performs no UI snapshot or
  render work. The warm minimized UI still misses the older aspirational 50 MiB target and remains
  an explicit optimization opportunity.
- Version `0.11.0` passed 160/160 Debug tests, 160/160 Release tests, 120/120 full-app
  analysis-disabled Release tests, and 76/76 UI/storage/analysis/detection-disabled headless
  Release tests. The platform boundary checker passed in every graph.
- CPack produced `BlackBox-0.11.0-windows-x64.zip`; package contents and the generated SHA-256
  sidecar verified. The developer executable remains deliberately unsigned and is not an official
  V1.0 candidate.

## V0.12 — Product UI, settings, and guided recovery

**Objective:** Replace the engineering dashboard with a usable evidence-first diagnostic workflow.

- [x] Add real Live, Incidents, Detail, Patterns, Settings, and Diagnostics navigation
- [x] Lead incident detail with symptom, likely contributor, uncertainty, and plain-language evidence
- [x] Add synchronized/zoomable timelines and an unambiguous incident marker
- [x] Expose validated hotkey, capture-window, detector, cooldown, notification, archive, and privacy settings
- [x] Show archive health/capacity and guided retry, backup, restore, retention, export, and purge actions
- [x] Add first-run onboarding and explanations for unavailable/cold-start/correlation states
- [x] Make first run and Live glanceable while retaining technical status behind progressive disclosure
- [x] Add explicit loading, empty, no-match, and archive-unavailable incident presentations
- [x] Add automated interaction, screenshot, keyboard, DPI, multi-monitor, and high-contrast tests

V0.12 completion evidence (2026-08-20):

- The native shell now presents six explicit product pages with keyboard navigation. Incident detail
  leads with symptom, likely contributor, uncertainty/confidence, coverage, and plain-language linked
  evidence; it retains an explicit correlation-is-not-proof caveat. System, throughput, and process
  timelines share zoom state, retain one bounded marker-relative hover cursor across every plot, and
  mark the incident at zero with a separate labeled vertical line. The cursor is UI-only evidence
  navigation and cannot affect stored telemetry or analysis.
- A strict, versioned, atomically replaced product-settings file controls the hotkey, automatic
  detector sensitivity/resources/cooldown, notifications, archive location/capacity, onboarding, and
  executable-path privacy. Recorder capture windows remain separately validated and versioned. Live
  reconfiguration stops and restarts only the collector epoch, rolls back hotkey conflicts, and keeps
  archive path/capacity changes explicit as next-launch settings.
- The incident writer retains exactly one oldest failed immutable incident in a bounded recovery slot.
  Guided maintenance runs on its own bounded worker and exposes health, retry, online backup, validated
  restore with a pre-restore safety copy, retention, dataset/failed-incident export, and confirmed purge.
  Normal rolling collection never opens SQLite or invokes maintenance, and deletion remains explicit.
- First-run onboarding and unavailable, warming/cold-start, and probabilistic-correlation explanations
  are user-facing. Windows high-contrast state and per-monitor display scale feed primitive UI state;
  deterministic keyboard, DPI, multi-monitor, palette, settings, recovery, and interaction tests cover
  the workflow. The screenshot smoke renders all six real pages in normal and scaled high-contrast
  modes and fingerprints the resulting ImGui draw data.
- The preview simplification pass turns onboarding into a three-action recorder/capture/review path,
  leads Live with recorder readiness and one plain-language capture action, and moves platform,
  capture internals, forensic counters, rolling graphs, active-process tables, and detailed incident
  evidence behind named disclosure controls. Detail retains its symptom/contributor/uncertainty
  headline and previews at most three standout contributors before raw factors and timelines.
  Incidents now distinguishes loading, a genuinely empty archive, an empty search result, and an
  unavailable archive with direct navigation to the appropriate next action. Pure presentation
  classification plus rendered onboarding/empty-state and full two-scale raster tests protect the
  flow without moving archive, analysis, or collection work into rendering.
- Version `0.12.0` passed 175/175 Debug tests, 175/175 Release tests, 135/135 full-app
  analysis-disabled Release tests, and 76/76 UI/storage/analysis/detection-disabled headless Release
  tests. Architecture boundary checks passed in every graph, and the assembled Release application
  completed an isolated hidden startup/shutdown smoke with exit code zero.
- README, configuration, user, build/release, and readiness documentation describe the finished
  workflow and recovery contract. CPack produced `BlackBox-0.12.0-windows-x64.zip`; the package and
  generated SHA-256 sidecar were verified. The developer binary remains unsigned and pre-V1.0.

## V0.13 — Storage and network forensic signals

**Objective:** Diagnose stalls and connectivity failures rather than only throughput saturation.

- [x] Select and validate ordinary-user Windows disk latency, queue, and service-time sources
- [x] Add capability-gated disk latency/queue domain types, normalization, incident storage, and plots
- [x] Add network latency, loss/timeout, interface transition, and connectivity evidence where reliable
- [x] Distinguish physical storage/network layers from process I/O and application payload claims
- [x] Detect short stalls/drops without requiring three one-second high-throughput observations
- [x] Validate accuracy, privilege behavior, device churn, overhead, and false positives on real workloads

V0.13 completion evidence (2026-08-20):

- A separate persistent ordinary-user PDH query now samples non-`_Total` physical-disk read/write
  latency, service time, and current queue depth without coupling failure to the existing throughput
  source. Portable capability/status types, normalization, immutable incident capture, direct
  schema-v1 one-to-one quality rows, direct dataset-v1 export, analysis evidence, version-2
  recurrence features, Live
  status, and synchronized incident plots preserve the full path without adding a ring or thread.
- Passive network evidence combines the bounded `GetIfTable2` hardware-interface lifecycle,
  `GetNetworkConnectivityHint`, and IPv4+IPv6 `GetTcpStatisticsEx` deltas. It records aggregate
  connectivity, active interfaces, transitions, TCP retransmission fraction, failed attempts, and
  established resets. Independent source failures preserve healthy fields. RTT, DNS, endpoints,
  packets, payloads, and per-application network claims remain deliberately unsupported because no
  ordinary-user, machine-wide passive source met the semantic/privacy gate.
- Balanced automatic capture can react to one 100 ms physical service-time observation, queue depth
  8, disconnection, constrained connectivity plus a transition, 25% TCP retransmission over at
  least eight segments, or two failed/reset connections. The exact trigger signal is persisted;
  the global cooldown, per-resource controls, availability rules, and three-observation
  utilization/throughput detector remain intact. A two-million-observation quiet detector soak
  emitted zero captures and averaged 309.778 ns per collector sample.
- Deterministic tests cover warm-up/reset, missing/invalid/independently failing sources, interface
  arrival/removal/state changes, cooldown, quiet-hour false positives, direct schema-v1 creation/rejection,
  archive/dataset round-trip, quality-aware analysis, recurrence invalidation, and plots. Native
  ordinary-user tests cover current interface/TCP state, loopback exclusion, provider recovery, and
  a 32 MiB unbuffered/write-through disk workload with finite nonnegative quality gauges. Automated
  real adapter disable/enable was rejected as disruptive; long multi-adapter/sleep churn remains an
  explicit V0.17 wall-clock matrix rather than an unsafe milestone test.
- The final version `0.13.0` passed 182/182 Debug tests, 182/182 Release tests, 142/142 full-app
  analysis-disabled Release tests, and 79/79 fully headless Release tests. The boundary checker
  passed in every graph, and the assembled Release app completed isolated hidden startup/shutdown.
  A 30.016-second ordinary-user headless run used 0.134475% total-machine CPU, 14,983,168 bytes
  maximum working set, 13.754 ms collection P99, and had zero failed samples, deadline misses, or
  drops. The unsigned developer ZIP and SHA-256 sidecar were generated and verified; this remains
  pre-V1.0.

## V0.14 — GPU, responsiveness, audio, power, and Windows events

**Objective:** Cover the game, UI-stutter, audio, thermal, and background-Windows cases central to
the original vision.

- [x] Add capability-gated GPU engine/memory and foreground-application evidence
- [x] Research bounded frame-presentation and application-hang signals
- [x] Add audio glitch/device-change and DPC/ISR evidence only where semantics are defensible
- [x] Add CPU frequency, power/thermal/throttling, sleep/resume, and device transition events
- [x] Add selected service, Defender, Windows Update, and Event Log evidence with privacy controls
- [x] Keep high-rate/event streams in separate bounded rings and join them only into immutable incidents
- [x] Measure every provider independently and prove the base recorder works with each disabled

V0.14 completion evidence (2026-08-20):

- Version `0.14.0` adds capability/status-preserving GPU engine and dedicated-memory gauges,
  foreground PID/creation-token correlation behind an opt-in identity gate, DPC/ISR activity,
  current/maximum CPU frequency and thermal-limit context, power/battery/uptime gauges, and bounded
  power, device, audio, service, Defender, Windows Update, and application-hang events. The live and
  incident UI, SQLite archive, and privacy-normalized dataset expose the same evidence without
  converting unavailable values to zero or presenting temporal correlation as causation.
- Native callbacks feed a fixed 1,024-event queue; a separate event collector drains into a
  configurable fixed ring (4,096 default, 65,536 maximum) in batches of at most 256. Incident
  construction copies only the event-time window into the immutable snapshot. When every event
  source is disabled the application does not start the event thread. Foreground identity and each
  event family default off and are independently reconfigurable.
- Official API research and explicit non-adoptions are recorded in `docs/WINDOWS_EVENT_EVIDENCE.md`.
  Persistent PDH counters are sampled at the existing one-second cadence. Direct frame-presentation,
  high-rate audio glitch, and generic hung-window polling were not adopted because their semantics,
  rate, privilege, or documented intended use do not meet the base-recorder contract.
- A release-truth audit corrected the primary telemetry matrix's stale pre-V0.14 GPU-research row
  and now lists the implemented capability-gated GPU/memory, foreground correlation, DPC/ISR,
  frequency/thermal, and power gauges. A cross-graph documentation contract binds those claims to
  the Windows provider while requiring the real corpus, held-out, physical-client, unsigned-package,
  and clean-client gates to remain explicitly open until their evidence exists.
- Because the application remains unreleased, archive, recorder/product settings, and classification
  dataset formats were reset to one complete version-1 contract. They are created/read directly;
  incompatible or unversioned non-empty archives are rejected unchanged, and no legacy reader or
  pre-release compatibility path remains.
- The final source passed 183/183 Debug tests, 183/183 Release tests, 143/143 full-app
  analysis-disabled Release tests, and 86/86 fully headless Release tests. Architecture boundaries
  passed in every graph. The isolated hidden application startup/shutdown returned status zero.
- A 30.020-second ordinary-user native run collected 31/31 samples with 0.065061% total-machine CPU,
  17,076,224 bytes maximum working set, 15.473 ms collection P99, 1.364 ms scheduling-jitter P99,
  and zero failures, deadline misses, drops, or resume events. Independent two-second runs of the
  disabled baseline and each of the seven enabled event families completed 40 polls each
  with zero native drops; the maximum observed source CPU was 0.064595% total-machine capacity.
- The unsigned developer `BlackBox-0.14.0-windows-x64.zip` and SHA-256 sidecar were generated and
  content-verified. Official signing and clean-client qualification remain V0.17 gates.
- A 2026-08-21 privacy/diagnostic augmentation adds future-only DNS Client event 1014 as the eighth
  bounded event family. A one-second isolated run completed 20 polls with zero events/drops and
  0.000000% observed total-machine CPU; the nine-case provider benchmark's maximum was 0.128730%
  on the audio case. This does not rewrite the original V0.14 seven-family measurement.
- A second 2026-08-21 augmentation adds future-only, privacy-normalized Display recovery event 4101
  as the ninth bounded family. It retains no message, driver name, adapter identity, or payload and
  can assert only the exact Windows timeout-recovery symptom, never its root cause. Its isolated
  one-second benchmark completed 20 polls with zero events/drops and 0.000000% observed CPU; the
  ten-case benchmark maximum was 0.128958% on the Windows Update case.
- A third 2026-08-21 augmentation adds future-only provider `disk` event 153 as the tenth bounded
  family. Only source/kind/level/ID/time enter core; LBA, device/PDO identity, message, and payload
  are absent. The exact Windows-reported I/O-retry symptom may request disk-scoped capture through
  the existing coordinator, but never identifies overload, cable, controller, driver, media,
  firmware, application, hardware failure, or another root cause. Its isolated one-second benchmark
  completed 20 polls with zero events/drops and 0.000000% observed CPU; the eleven-case benchmark
  maximum was 0.128763% on the Windows Update case.
- A fourth 2026-08-21 augmentation adds opt-in, future-only process start/exit context without a
  second native process poll. The existing full-identity enumeration emits only durable PID plus
  creation-token identities after a successful warm-up; initial inventory, resume/reconfiguration,
  incomplete enumeration, access races, and uncertain resynchronization cannot fabricate events.
  Events enter the separate bounded event recorder, never request capture or assert causality, and
  join immutable incidents only when they fall inside the incident window. The direct V1 archive
  retains local identity references, while dataset and truth-review output expose only stable
  incident-local process ordinals. Collection defaults off and adds no migration, legacy reader, or
  compatibility branch. A real child-process workload verifies both start and exit identity, and
  strict validation covers storage round-trip, privacy-safe export, rendering, settings, diagnostics,
  and malformed input. Final source passed 294/294 Release, 294/294 Debug, 122/122 fully headless,
  186/186 analysis-disabled, and 294/294 Windows AddressSanitizer tests; native MSVC static analysis
  also completed cleanly.
- A fifth 2026-08-21 augmentation makes that lifecycle evidence useful in contributor explanations
  without turning chronology into causality. Contributor output now distinguishes the first
  anomalous activity sample from an exact recorded process start and exit. Lifecycle context attaches
  only when the event source and kind are exact, PID plus creation token match, the event lies inside
  the immutable incident window, and start/activity/exit ordering is consistent. It never changes
  contributor score, rank, strength, confidence, diagnosis, or feedback calibration. Adversarial tests
  cover PID reuse, late starts, out-of-window events, wrong sources, and unrelated contributors. A
  low-rate DWM presentation-statistics candidate was also implemented and measured, then fully removed:
  19 normalized native intervals reported zero displayed, late, dropped, and missed frames, and the
  API's queued-present model is retired. No misleading field, compatibility path, or migration was
  retained; any future continuous frame-quality signal must use current semantics and earn adoption
  with real calibration. Final source passed 295/295 Release, 295/295 Debug, 122/122 fully headless,
  186/186 analysis-disabled, and 295/295 Windows AddressSanitizer tests; the complete native MSVC static
  analysis graph also completed cleanly.

## V0.15 — Real-world diagnostic corpus and calibration

**Objective:** Establish whether BlackBox diagnoses actual user-visible incidents rather than only
passing synthetic mechanics tests.

- [x] Define a versioned local dogfood protocol and truth/uncertainty annotation format
- [x] Collect representative CPU, disk, network, hang, game, audio, quiet, and ambiguous incidents
- [x] Predeclare top-1/top-3, Unknown, miss, false-capture/hour, calibration, and usefulness metrics
- [x] Evaluate detector, context, contributor, recurrence, and diagnosis components on held-out captures
- [x] Calibrate confidence and thresholds without weakening missing-data or correlation language
- [x] Publish dataset limitations, hardware distribution, disagreements, and reproducible evaluation tools

V0.15 evidence:

- Version `0.15.0` adds an offline-only `BlackBox::Evaluation` boundary, strict direct-format-v1
  corpus parser/freezer, isotonic calibration with a predeclared 80%-precision assertion gate,
  privacy-safe evaluation output, and a one-shot held-out lock. The desktop does not link the
  evaluation library and collection does not read evaluation state.
- A real Windows provider/normalizer/detector/recorder/archive harness and bounded controlled
  CPU, disk, loopback-reset, hang, frame-stutter, and audio-playback-gap workloads produced a
  frozen 32-session/32-incident corpus with zero collector failures or drops. The corpus fingerprint
  is `4801556224897119752`; pipeline/configuration are `3`/`15102167315426489669`.
- One-shot held-out results were 2/6 supported diagnoses after calibration, 4/4 Unknown-truth
  abstentions, 4/7 top-1 and top-3 contributors, 6/9 automatic detector recall, 10/10 desktop
  context, and 0/65-second quiet automatic captures. The assertion gate emitted only 2/10 held-out
  diagnoses. These are evidence of excessive abstention and insufficient V1 quality, not product
  accuracy claims.
- Development findings produced low-confidence bounded cold-start contributor evidence, practical
  process effect-size floors, and automatic trigger-resource disambiguation without adding causal
  language. The frozen results, single-host hardware distribution, controlled-surrogate limits,
  absent natural/usefulness evidence, and reproduction commands are published in
  `docs/V015_DOGFOOD_RESULTS.md`.
- Debug, Release, headless, and analysis-disabled graphs build. Release CTest passes all 195 tests;
  Debug passes 194/195 in the sandbox and the current-user registry test passes with required access.
  Repeating held-out evaluation is refused without creating output.

## V0.15.1 — Diagnostic reliability recovery

**Objective:** Recover useful diagnostic coverage from the V0.15 negative result before feedback
learning or symptom classification can amplify weak evidence.

- [x] Add practical system-resource effect floors and evidence-quality abstention so quiet and
  unsupported symptom truth does not depend on a corpus-wide confidence cutoff
- [x] Separate observed resource pressure from inferred symptom explanation throughout diagnosis,
  UI wording, exports, and tests; retain `Unknown` when no aligned symptom evidence exists
- [x] Improve automatic hang/frame/audio capture only with bounded measurable signals, and keep
  unsupported probes visibly unavailable rather than inferred from incidental pressure
- [x] Extend the direct-format-v1 corpus/freezer contract with session-operator pseudonyms, distinct
  non-operator annotation ballots, per-split nine-class truth coverage, the same three or more
  hardware profiles contributing natural/quiet/scorable evidence to both frozen splits, at least
  ten quiet exposure hours, and zero reuse of V0.15 held-out incident keys; expose exact readiness
  counts and unmet requirements through the offline CLI, and evaluate an explicit profile-to-archive
  map with duplicate-key and session/profile provenance checks
- [x] Require an explicit participant-consent attestation on every direct-v1 session row; reject
  missing or false consent before validation, packet merge, freeze, fingerprinting, or evaluation
  without accepting an older header or compatibility representation
- [x] Add blinded read-only archive listing/inspection and immutable one-session acquisition packets;
  require exact five-file direct-v1 contents, archive-key and automatic-capture evidence, identical
  coarse hardware identity on reuse, reload verification, and atomic publication of a new full corpus
- [x] Add an atomic prediction-free truth-review artifact with raw normalized plots/events, stable
  process ordinals, a blank independent ballot, explicit identity privacy mode, strict bounds, and
  no analyzer invocation so human annotation does not depend on console-only evidence
- [x] Add an atomic prediction-free campaign-status artifact with exact schema-v1 manifest/TSV/HTML
  output so operators can review profile, split, quiet-hour, truth, and symptom gaps without
  invoking diagnosis or treating coordination output as collected evidence
- [x] Add an atomic prediction-free helper for completed consented quiet exposures with zero
  automatic captures, requiring explicit consent/exposure/no-capture attestations and leaving every
  incident-bearing session on the full independently annotated archive-backed path
- [x] Add an atomic one-incident natural-session packet helper that requires explicit consent,
  completed-session, and fixed-consensus attestations; validates two independent non-operator
  ballots; proves incident/automatic-capture provenance against the read-only schema-v1 archive;
  and leaves the base corpus, archive, and analyzer boundary unchanged
- [x] Make offline CLI analyzer construction lazy so blinded truth review and campaign coordination
  commands cannot even instantiate prediction-bearing analysis state
- [x] Add canonical standalone completed-ballot validation that binds the expected incident and
  rejects the session operator while emitting no diagnosis-bearing fields; reuse the exact corpus
  parser so annotation handoff cannot drift from protocol V1
- [x] Add prediction-free two-ballot comparison that requires distinct annotators and emits only
  binding metadata plus the mechanical disagreement bit without exposing payloads or generating
  consensus truth
- [x] Make invalid campaign packets actionable with privacy-safe row-specific diagnostics for
  session/ballot counts, disagreement, duplicates, and operator conflicts without exposing ballot
  payloads, process identity, archive paths, or analyzer output
- [x] Add future-only, privacy-normalized DNS Client timeout evidence and a precise event-aligned
  symptom without retaining queried hostnames/messages, inferring root cause, or automatically
  capturing a common resolver warning
- [x] Add future-only, privacy-normalized Display timeout-recovery evidence, route the OS-confirmed
  symptom through the bounded automatic-capture coordinator, and retain no driver/message/adapter/
  payload or unsupported root-cause attribution
- [x] Add future-only, privacy-normalized provider `disk` event 153 evidence, route the exact
  Windows-reported I/O-retry symptom through disk-scoped bounded automatic capture, and retain no
  LBA/device/PDO/message/payload or unsupported storage root-cause attribution
- [x] Add future-only, privacy-normalized Application Error event 1000 evidence, route the exact
  Windows-reported crash symptom through bounded automatic capture, and retain no application/
  module/exception/path/message/payload or unsupported root-cause attribution
- [x] Promote `application_crash` to the ninth canonical direct-v1 truth symptom so corpus parsing,
  per-split freeze coverage, campaign status, evaluation reports, independent ballots, and the
  archive-proven session helper cannot conflate crashes with hangs or ambiguous incidents; size the
  bounded report structures from one compile-time class count and add no migration or legacy reader
- [ ] Collect the qualifying consented, independently annotated, multi-hardware natural and quiet
  sessions required by that contract
- [x] Predeclare and mechanically enforce the new held-out gate: at least 80% assertion precision,
  60% supported diagnosis recall, 90% Unknown-truth abstention, 70% contributor top-3, nonzero and
  published denominators; a failed one-shot run remains written and locked
- [x] Acquire the one-shot held-out attempt exclusively before analysis, bind calibration/report
  artifact fingerprints, preserve crash state, expose attempt status, and publish calibration and
  held-out directories atomically so partial output cannot masquerade as a result
- [ ] Meet the predeclared gate on the new one-shot held-out split
- [ ] Re-run a new fingerprinted calibration/one-shot held-out corpus; do not reuse or relabel the
  V0.15 held-out set

Implementation status (2026-08-22): pipeline v13 / evidence model v12 / configuration fingerprint
`6701770989141957614`. Practical pressure and raw statistical deviation are separate values. A
symptom explanation now requires automatic-capture, direct quality, strong preceding-contributor,
or system-event alignment. Windows Application Error event 1000 and Application Hang event 1002
can request bounded automatic capture. Display recovery event 4101 uses the same bounded coordinator and supports only the exact
Windows-reported timeout-recovery symptom; it retains no driver/message/adapter/payload and makes no
root-cause attribution. DNS Client event 1014 remains manual-incident evidence only: it stores no queried hostname
or Event Log message and can assert only that Windows reported a nearby resolution timeout, never
its cause; an independently aligned resource explanation outranks a coincidental timeout.
Provider `disk` event 153 uses disk-scoped bounded automatic capture and can assert only the exact
Windows-reported I/O-retry symptom; LBA/device/PDO/message/payload are absent and no overload,
controller, driver, media, firmware, application, hardware-failure, or other cause is inferred.
OS-wide
frame-pacing and audio-glitch probes remain explicitly unsupported. Release and
Debug each pass 290/290 tests, headless Release passes 120/120, analysis-disabled Release passes
182/182, and Windows ASan passes 289/289 applicable tests. The full warnings-as-errors MSVC
static-analysis graph is clean; large capture, provider,
PDH, and UI benchmark buffers use explicit heap/static-duration ownership instead of constrained
thread stacks. The regenerated unsigned portable ZIP is content/checksum verified at SHA-256
`c5e81e1d86a9888759502a1f0e8e2260e19f646ac973dd36433f08a32d833663`; its standard packaged
smoke qualification artifact is
`out/client-qualification-smoke-v1-20260821-storage-retry-final/`. The isolated 15-second
run completed 16 collections and two archived incidents with zero partial/failed/dropped/late/
deadline-missed samples, snapshot failures, writer failures, or capture rejections; schema v1 was
healthy and maximum working set was 55,619,584 bytes. The refreshed 30-case deterministic UI raster
bundle is `out/ui-qualification-v1-20260821-storage-retry-reviewed/`; its manifest SHA-256 is
`637e990fe7530b46b44a63115c0d2566eaa2708a94028a81f3fc4534b5c6f2fb` and normal/high-contrast
representative event timelines were visually checked. This remains an unsigned one-host smoke, not
the clean-client, physical, or signing gate.

The strengthened direct-v1 campaign contract does not accept annotator counts without matching
ballots or machines represented in only one evidence role. Acquisition now separates blinded truth
inspection from prediction-bearing development inspection, opens retained evidence through an
existing-file/query-only schema-v1 archive connection, and imports one structurally complete session
at a time without mutating its packet, archive, or prior corpus. A real representative schema-v1
archive integration proved key/capture matching and byte preservation, and the registered CLI
contract repeats quiet and incident-bearing packet publication end to end. That fixture proves the
tooling only. An ordinal-only truth-review directory now turns retained raw values into a bounded
self-contained visual/TSV annotation surface without invoking or exposing predictions; local
PID/name inclusion is a mandatory explicit privacy choice and never changes the corpus. Automated
contracts verify generation, atomicity, exact files, privacy exclusions, and the real read-only CLI
path. A separate exact six-file campaign-status directory provides a self-contained readiness page
and machine-readable gap tables; its manifest declares prediction-free and evidence-neutral, and
API/CLI contracts reject malformed provenance, occupied outputs, staging residue, and prediction
leakage. A campaign browser visual check remains procedural. V0.15.1 remains incomplete until new
consented human evidence is collected, frozen, calibrated, and passes the one-shot gate; no
synthetic substitute is accepted.

Completed zero-capture quiet exposures now have a narrow prediction-free PowerShell acquisition
helper. It requires exact post-fact consent, completed-exposure, and no-capture attestations; accepts
only calibration/held-out durations of at least one hour; uses the native direct-V1 `init-session`
and `validate` path; validates both sides of an atomic sibling-directory publication; and leaves the
base corpus byte-identical. Its end-to-end contract rejects false tokens and occupied destinations.
The helper neither observes the exposure nor creates evidence, and every incident-bearing session
still requires archive proof and independent annotation.

The common one-incident natural-session path now has a matching atomic helper rather than requiring
five coordinated TSV edits. It consumes two already completed independent ballots plus separately
fixed consensus truth, rejects either ballot when authored by the session operator, derives the
disagreement bit, and invokes only prediction-free native commands. A disposable merge proves the
incident key and automatic-capture count against the supplied read-only schema-v1 archive; hashes
prove the archive and base corpus remain unchanged, and proof/staging trees cannot masquerade as the
published five-file packet. This is acquisition correctness tooling only and does not count as a
natural session, consent proof, independent annotation, or diagnostic-quality evidence.

Consent is now enforceable rather than procedural-only. The sole direct-v1 `sessions.tsv` header
requires a canonical `consent_attested` column, and every row must contain `1` only after the
participant agrees to local collection and privacy-reduced campaign use. Missing, false, malformed,
and pre-consent nine-column session layouts are rejected before validation, packet merge, campaign
status, freeze, fingerprint publication, or evaluation; no compatibility reader or converter was
added. The attestation is included in the corpus fingerprint and described honestly as an operator
record rather than cryptographic/legal proof. API and real CLI contracts cover false consent,
obsolete headers, fingerprint binding, and exact error reporting. The direct-v1 build contract now
also rejects documentation that reintroduces non-V1 persisted-version claims. Final source
passes 296/296 Release, 296/296 Debug, 122/122 fully headless, 186/186 analysis-disabled, and
296/296 Windows AddressSanitizer tests; the complete warnings-as-errors MSVC static-analysis graph
is clean. The accelerated collector concurrency test was also corrected to assert bounded recorder
and diagnostic accounting rather than an arbitrary host-scheduler dropped-tick threshold.

The exact V1 held-out scorer now closes a sparse-submission loophole: missing predictions remain
failures in every eligible truth-based supported-recall, Unknown-abstention, contributor, context,
and detector denominator instead of disappearing from the rate. Supported precision and confidence
calibration remain assertion-based. The sole V1 metric name is `supported_diagnosis_recall`; no
`diagnosis_accuracy` alias or old-manifest reader exists. Evaluation JSON now contains every rate's
  numerator/denominator, all nine symptom counts, coarse represented-hardware buckets, quiet capture
count and exposure hours, the explicit miss rate, all ten calibration bins, and recurrence pair
counts. Regression tests prove three omitted predictions cannot qualify, and the direct-V1 contract
pins the report vocabulary and rejects the obsolete alias.

Evaluation publication is now independently recomputable rather than merely nonempty and
fingerprinted. One canonical offline-only V1 serializer/verifier strictly parses bounded prediction
rows, recomputes the complete report from frozen truth without an analyzer or archive, and requires
exact bytes before publication and again after the atomic rename. The final two-file fingerprint is
also rechecked before the one-shot attempt can complete. `verify-evaluation` later repeats the same
check and binds a parsed supplied calibration's fingerprint, assertion state, and threshold.
Malformed/oversized/excess rows, duplicate contributor ordinals, sparse prediction removal, metric
tampering, and wrong corpus provenance are covered. The direct V1 diagnosis table includes all
eleven pipeline outputs: application crash, DNS timeout, display recovery, and storage I/O retry no longer
fall outside the corpus serializer. No seven-value reader, conversion, or alias remains.

Application crash is now also a first-class ninth symptom in the direct V1 truth taxonomy, rather
than only an expected diagnosis. The single canonical class count drives both split-coverage arrays
and published evaluation counts; the campaign surface and natural-session helper accept the same
vocabulary. Complete-corpus fixtures prove crash coverage in both frozen splits, the canonical JSON
publishes its count, and the acquisition contract exercises a crash ballot/packet end to end. Because
the product is prerelease, this tightens V1 in place and intentionally adds no migration, alias, or
legacy reader.
The focused evaluation/acquisition/direct-V1 set passes 14/14 tests. The complete Debug graph passes
308/309 inside the restricted process environment, with only the expected HKCU launch-at-login
denial; that exact registry integration passes 1/1 with current-user access, completing 309/309.

The confidence calibration file now has one bounded canonical direct-V1 implementation in the
offline evaluation library instead of a permissive app-local parser/writer. It fixes exact-precision
serialization for header probabilities and thresholds, accepts only ordered LF-terminated canonical
bytes from a non-link regular file, enforces 64-KiB file/4-KiB line/32-knot bounds, validates
monotonic knots, represented sample totals, and assertion-state consistency, refuses occupied final
or sibling-partial paths, publishes by same-directory rename, and reloads the result before success.
CRLF, blank/oversized lines, equivalent noncanonical number spellings, reordered fields, malformed
counts, directories, and overwrite attempts are covered. There is no old calibration reader or
conversion branch. The CLI also reloads and exactly compares the calibration after the outer
evaluation-directory rename. Current Release and Debug pass 290/290 tests, the ASan preset passes 289/289
applicable tests (290 registered before its intentional crash-probe exclusion), headless Release
passes 120/120, analysis-disabled Release passes 182/182, and MSVC native analysis is clean.

## V0.16 — Feedback learning and symptom classification

**Objective:** Make confirmed user feedback improve future local diagnoses safely.

- [x] Add bounded feedback-aware detector/ranking calibration and false-positive suppression
- [ ] Add symptom classification only after held-out evaluation proves useful performance
- [x] Reuse confirmed similar-incident evidence without presenting recurrence as causal proof
- [x] Add poisoning/noise safeguards, minimum evidence, rollback, reset, and profile inspection
- [x] Keep training/tools outside the runtime and collection independent of learned state
- [x] Preserve source timing in contributor attribution and separate preceding, marker-spanning,
  and post-marker victim/reaction roles
- [ ] Reconsider native ML only if it materially beats the statistical baseline within footprint gates

Implementation status (2026-08-21): three conservative slices are complete without claiming the
  milestone. Pipeline v13 consumes at most 256 prior answered automatic incidents, only when their
resource/signal signature exactly matches the current trigger. It excludes the current/future,
duplicate, mismatched, manual, and older-than-90-day evidence; requires four matches; uses a
smoothed 75% false-positive activation threshold; and caps ordinary confidence reduction at 55%.
Only an existing automatic-trigger diagnosis can be reduced or changed to `Unknown`; detector
collection, resource/process scores, contributor order, immutable telemetry, and annotations are
unchanged. The Detail UI exposes counts, multiplier, revision, a confirmed reset, and one-step
rollback. Reset advances a singleton direct-schema-v1 cutoff rather than deleting history, privacy
purge clears its state, and no migration or legacy path exists. Automatic recurrence now also
summarizes at most 32 distinct prior members from 90 days as historical context only. Manual groups,
current/future/stale/reset/duplicate rows, fewer than two matching confirmations, under-75%
noticed-problem agreement, and under-75% category consensus cannot teach it. The UI shows
ready/cold/conflicting/manual-excluded state; reset and rollback apply; diagnosis, confidence,
evidence links, rankings, grouping, and telemetry remain identical. Explicit contributor
attribution is now a separate direct-V1 signal: one replaceable vote per incident/executable/resource,
never inferred from noticed-problem feedback. Future exact matches require four distinct prior
votes and 75% consensus; strict prior/update-time, 90-day, reset, duplicate, key, and resource
guards apply. Existing candidates retain their incident-local score and may move only within
`0.70x..1.15x`. The ranker now counts anomalous samples on both sides of the marker and separates
predominantly preceding activity from marker-spanning ambiguity and wholly post-marker possible
victims/reactions. Only preceding rows can be likely contributors or align a symptom. Attribution
stores this source role directly in schema V1, and confirmations from either non-preceding role are
ineligible for positive future learning. The UI exposes current attribution, raw/adjusted scores,
multiplier, counts, phase balance, and cold/conflicting/active state. Controlled pipeline, storage,
worker, and poisoning tests validate the mechanics without claiming representative causal accuracy.
Current provenance is pipeline v13 / evidence model v12 / fingerprint `6701770989141957614`.
Remaining V0.16 work includes any useful symptom classifier after the V0.15.1 held-out gate and the
native-ML adoption comparison. Final Release and Debug pass 290/290 tests, headless Release passes
120/120, analysis-disabled Release passes 182/182, and Windows ASan passes 289/289 applicable tests;
warnings-as-errors MSVC native analysis is
clean. The corrected provenance-aware
analysis benchmark passes 4/4 controlled labeled diagnoses and 0/1 quiet false positives. The
30-case final raster bundle at
`out/ui-qualification-v1-20260821-storage-retry-reviewed` passed automated validation
and manual normal/high-contrast review; its manifest SHA-256 is
`637e990fe7530b46b44a63115c0d2566eaa2708a94028a81f3fc4534b5c6f2fb`. The refreshed unsigned
portable package SHA-256 is
`c5e81e1d86a9888759502a1f0e8e2260e19f646ac973dd36433f08a32d833663`; its isolated 15-second
local smoke passed with 16 collections, two archived incidents, schema v1, zero partial/failed/
dropped/late/deadline-missed samples, zero writer failures or capture rejections, and a
55,619,584-byte maximum working set. Evidence is at
`out/client-qualification-smoke-v1-20260821-storage-retry-final`; its manifest SHA-256 is
`2479833a23a426cb5fe8266c3945637270cd4ffbc8437a9865bb7be589a729cb`. This local smoke does not
satisfy signing, clean-client, or physical-client gates.

## V0.17 — Release candidate, supportability, and trust

**Objective:** Prove the completed product on its supported matrix before naming it V1.0.

- [x] Add crash diagnostics, support bundle, privacy threat model, and recovery runbooks
- [x] Add sanitizer, static-analysis, dependency/security, fuzz/property, and coverage jobs
- [x] Enforce the exact canonical direct-V1 SQLite layout on open and restore without conversion
- [x] Pin and publish the named long-mode capture/checkpoint cadences and minimum scheduled-capture
  count; require retained runner/verifier hashes to match the current release-source scripts; and
  make standalone verification recheck every runner acceptance gate, including app-report cadence/
  collection accounting and independently recomputed process-journal timing, sampling gaps, CPU,
  maximum-resource, and first/last steady-state metrics
- [ ] Complete overnight and 72-hour wall-clock soaks including sleep, lock, device churn, and archive faults
- [ ] Run and record clean Windows 10 22H2 and supported Windows 11 package matrices
- [ ] Complete accessibility, DPI, multi-monitor, low-end hardware, battery, and power-mode validation
- [ ] Execute hosted CI on the release revision and retain artifacts/results
- [ ] Produce and verify an officially signed/timestamped release candidate and checksum

V0.17 supportability evidence (2026-08-21):

- Archive acceptance no longer trusts `user_version=1` plus a partial table check. Storage builds
  the one compiled V1 layout in an isolated in-memory SQLite connection and compares every ordered
  non-internal table/index definition, the application ID, metadata row, and singleton feedback
  state before accepting an existing archive. Restore opens the source read-only and applies the
  same canonical comparison before creating a safety copy or touching the active archive. Tests
  alter a same-named table, remove an index, add an object, change application identity, remove each
  required control row, and present an incompatible restore source; every case is rejected without
  changing the evidence or active archive. No older-layout reader or conversion path exists.
Release and Debug pass 290/290 tests, Windows ASan passes 289/289 applicable tests,
analysis-disabled Release passes 182/182, headless Release passes 120/120, and warnings-as-errors
  MSVC native analysis is clean. The
  repackaged unsigned ZIP is checksum-verified at SHA-256
  `c5e81e1d86a9888759502a1f0e8e2260e19f646ac973dd36433f08a32d833663`; its fresh 15-second
  direct-V1 smoke completed 16 collections and two archived incidents with every sample/capture/
  writer failure counter at zero and a 55,619,584-byte maximum working set. Evidence is at
  `out/client-qualification-smoke-v1-20260821-storage-retry-final/`.

- A Windows-only platform service pre-opens one unique minimal-dump staging handle before normal app
  initialization. The guarded top-level exception path enters no recorder, SQLite, UI, or analysis
  code, flushes and renames completed `.dmp` evidence, and clean shutdown removes the unused
  `.partial` file. Completed dumps are local and never automatically deleted or uploaded.
- Diagnostics creates a fixed-allowlist direct-format-v1 support bundle on a dedicated single-request
  worker. Incident/process/settings/path/free-form evidence is excluded. Raw dump inclusion is
  bounded to a non-link regular file at most 64 MiB and requires a mechanically propagated explicit
  disclosure confirmation. Exact nonempty staging contents publish through one sibling directory
  rename; existing final/partial destinations are refused.
- The privacy threat model identifies sensitive assets, trust boundaries, controls, residual risks,
  and the release review checklist. Evidence-preserving runbooks cover failed capture, archive
  lock/full/corruption, invalid settings/startup, crash, partial support output, and verified restore.
  All formats remain direct v1 with no migration or legacy path.
- A cross-platform `pre_release_direct_v1_contract` now pins the schema and all nine persisted
  format constants to version 1, requires one version-1 schema publication, and rejects production
  migration/compatibility tokens. Product and recorder settings tests also rewrite otherwise valid
  files to format 2 and prove strict rejection without conversion. This is a prerelease regression
  guard, not a compatibility layer.
- A real child-process unhandled-exception probe produced a bounded file with the `MDMP` signature;
  an isolated hidden assembled-app run exited zero and left no crash staging or completed dump.
  Release and Debug pass 216/216 tests, headless Release passes 101/101, analysis-disabled Release
  passes 152/152, and architecture boundaries pass in every graph.
- Isolated quality jobs now cover immutable dependency/action policy, CycloneDX SBOM generation,
  moderate-or-higher dependency review, extended CodeQL, warnings-as-errors MSVC native analysis,
  Windows ASan with an instrumented dependency triplet, Linux UBSan, bounded native libFuzzer,
  deterministic strict-v1/archive corruption properties, and 60% line/45% branch coverage floors.
  Local policy contracts and SBOM read-back passed; MSVC analysis completed cleanly across product
  and tests; Windows ASan passed 215/215 applicable tests. The intentional unhandled-exception probe
  is excluded only from ASan and passed in both 216-test ordinary graphs.
- The analyzer findings were fixed rather than suppressed: application and bounded scratch state
  moved off constrained stacks, Windows allocation failure degrades explicitly, and integration
  workload handles/buffers have analyzable RAII lifetimes. The assembled Release app completed an
  isolated hidden startup/shutdown smoke. All formats remain direct v1 with no migration or legacy
  path. Hosted Linux sanitizer/fuzz/coverage, CodeQL, dependency-review, and artifact execution is
  still evidence required by the separate hosted-CI checkbox; soaks, client/accessibility matrices,
  and signing also remain open.
- Wall-clock qualification now has a bounded hidden diagnostic CLI (up to seven days), periodic
  ordinary capture scheduling, and a privacy-safe direct-v1 report written only after collector/event
  shutdown and writer drain. The campaign runner uses fresh settings and a fresh schema-v1 archive,
  records atomic checkpoints and hash-bound evidence, refuses shortened long modes, retains failed
  campaigns as `.partial`, and uses a non-shipping isolated SQLite lock probe for the 72-hour archive
  fault/recovery exercise. A 15-second assembled Release smoke passed with 16 collections, two
  completed/written/archived incidents, zero failures/drops/deadline misses, a healthy schema-v1
  archive, 54,616,064-byte maximum working set, and 0.0161% average total-machine CPU. A separate
  12-second smoke passed with a spaced evidence path, validating CLI quoting. These smokes validate
  the harness only; the overnight and actual 72-hour runs (including operator sleep, lock, and device
  churn) have not elapsed, so the soak checkbox deliberately remains open.
- The isolated archive-fault smoke then held the private SQLite writer lock beyond all bounded busy
  waits: seven snapshots completed, three writes exhausted retries and remained recoverable, four
  incidents committed, and a post-fault commit independently incremented writer recovery. The
  resulting archive remained healthy at schema v1 with no collector drops, deadline misses, snapshot
  failures, or cancellations.
- Wall-clock evidence publication is independently bound rather than trusted only because a runner
  returned zero. Its exact direct-v1 manifest binds the SQLite archive, settings, journals,
  completed checkpoint, app report, and summary; the verifier rejects partial/tree/link/size/hash/
  header/accounting changes before atomic publication and on later review. Named long modes now pin
  and publish their capture/checkpoint cadences plus minimum process, collection, and scheduled-
  capture coverage. Later review requires the retained runner/verifier identities to match the
  current release-source scripts, rechecks app-report duration/cadence/coverage, and recomputes
  elapsed/CPU monotonicity, sampling gaps, CPU, maxima, and first/last resource growth directly from
  `process-samples.tsv` rather than accepting self-consistent derived values. Checkpoint waits now
  wake on process exit, CPU uses the exact published journal values, long runs compare first/last ten-sample
  steady-state windows with 16 MiB working/private-memory and 32-handle growth ceilings, concurrent
  journal writes have bounded retry, and only the runner can attest archive-fault events. A dedicated
  Ctrl+Shift+Alt+F10 qualification hotkey avoids interfering with product-default hotkey tests.
  Campaign provenance binds and rechecks the application, runner, verifier, optional fault probe,
  and supplied source revision before publication; `local-uncommitted` is explicitly non-release.
  The final 12-second real smoke published six checkpoints with zero sampling gaps; the 40-second lock
  smoke published 20 checkpoints, three bounded write failures, four commits, one recovery, and a
healthy bound schema-v1 archive. The exact graphs pass 290/290 Release and Debug tests, 182/182
analysis-disabled tests, 120/120 headless tests, 289/289 ASan-applicable tests, and clean MSVC
  analysis. A first provenance-bound eight-hour local prequalification was retained as failed after
  a second BlackBox launch activated its supposedly hidden process and contaminated the resource
  baseline. The client runner now refuses any existing BlackBox process before hardware probes or
  staging. Its replacement started at `2026-08-21T15:56:46Z` under
  `out/soaks/overnight-v1-20260821-clean-final.partial`; it remains non-evidence until the full
  elapsed-time run verifies and atomically publishes, and its explicit `local-uncommitted` identity
  cannot substitute for the eventual release-revision run.
  That process was externally interrupted at 20,285 elapsed seconds (338 checkpoints, zero detected
  sampling gaps) and correctly remained `.partial`; it receives no release credit. A clean replacement
  started at `2026-08-22T06:33:59Z` under
  `out/soaks/overnight-v1-20260822-restart1.partial`. Its first 60-second checkpoint was healthy with
  one process sample and zero sampling gaps. It was also externally interrupted at 16,084 elapsed
  seconds after 268 checkpoints, zero detected gaps, and 17 readable scheduled incidents. It remains
  `.partial` and receives no release credit. The hardened contract now has hash-consistent rejection
  cases for stale runner/verifier identities, false maxima/CPU/growth/gaps, nonmonotonic journals,
  false coverage minimums, insufficient collections/captures, app-report cadence disagreement, and
  long-mode cadence overrides. It also accepts an exact-floor synthetic overnight campaign with 455
  process checkpoints, 27,360 collections, and 31 scheduled captures, then rejects the same bundle
  with an overridden cadence or one missing process row. A real 15-second assembled-app smoke at
  `out/soaks/hardened-contract-smoke-v1-20260822-2/` independently re-verified seven process samples,
  16 collections, two required scheduled incidents, zero failures/drops/deadline misses, and exact
  journal-derived metrics before atomic publication. This proves the hardened harness, not either
  required release-revision long soak.
- The shell now registers its hidden native window for current-session lock/unlock notifications,
  and the event collector retains exact aggregate totals for power, device, audio, service, Defender,
  Windows Update, and application sources. The 72-hour verifier requires WTS availability plus both
  lock transitions, requires device/audio activity to corroborate the operator journal, and rejects
  any report whose source totals do not account for every event. A real assembled-app smoke confirmed
  WTS registration in the interactive session with a healthy schema-v1 archive and zero failures,
  drops, or deadline misses. After these additions, ordinary Release and Debug pass 219/219 tests,
  headless Release passes 104/104, analysis-disabled Release passes 155/155, Windows ASan passes
  219/219, architecture boundaries pass in every applicable graph, and MSVC native analysis is clean.
- UI regression qualification now renders actual SDL3 software pixels rather than hashing display
  parameters. Representative and large incident fixtures cover all six product pages at 1100x700
  100% dark mode and 1100x700 logical/1650x1050 physical 150% high contrast. Opt-in evidence writes
  an exact 30-BMP direct-v1 bundle only from the test executable; the runner validates names, BMP
  headers/dimensions, executable/image SHA-256 identities, exact contents, and atomic publication.
  Full image review exposed an initially unused high-DPI canvas, unsupported em-dash placeholders in
  the bundled Basic Latin font, and an unwrapped Settings privacy notice. The harness now uses the
  renderer scale; production UI punctuation is glyph-safe, the privacy notice wraps, and a source
  regression test rejects UI bytes outside the shipped font coverage. The obsolete parameter-only
  “screenshot signature” helper/test was removed. Four additional scrolled incident-detail cases now
  prove the sticky synchronized cursor in both fixtures and display modes. Their review found a PID/
  large-value table collision; wider identity space and bounded compact numeric rendering fixed it.
  The corrected 30-image set adds two explicit feedback-control renders while retaining the four
  synchronized-cursor cases. That review exposed a collapsed statistical-evidence column; the
  final stretch-column sizing keeps evidence legible at both display modes. The bundle at
  `out/ui-qualification-v1-20260821-feedback-controls-30-layout-final/` was regenerated and
  reviewed; its local manifest hash is
  `ceb137ac14ed88d5e13efe4ee37c0b2c30f1af532d74204980c544f3d60eb92f`. Its generator summary still
  explicitly says manual review is required and the physical matrix is unsatisfied so generation
alone can never self-qualify. Current Release and Debug pass 290/290 tests, headless Release passes
120/120, analysis-disabled Release passes 182/182, Windows ASan passes 289/289 applicable tests,
  architecture/direct-v1 contracts pass in every graph, and MSVC native analysis is clean.
- Production accessibility state is no longer startup-only. The visible app polls the portable
  Windows preference boundary at most once per second, applies and reverses one UI-owned complete
  high-contrast palette, reports the system-animation preference, and catches up on the first refresh
  after reopening from the tray. The renderer test now consumes that production palette, its complete
  high-contrast six-page set was reviewed, and an interaction test drives real ImGui Ctrl+1/Ctrl+6
  input through `render_dashboard` rather than testing only a helper. A 15-second isolated assembled-
  app smoke completed 16 collections and two stored incidents with zero failed/dropped/deadline-
  missed samples, 55,111,680-byte maximum working set, and 0.040169% average total-machine CPU; its
  direct-v1 manifest hash is
  `f53a5ace21f4ba5928f90c4e3b7ffa1ec74c679e32619d1ad1b438d6ed8cc153`.
  At that point Release and Debug passed 221/221 tests, headless Release passed 104/104,
  analysis-disabled Release passed 157/157, Windows ASan passed 221/221, architecture boundaries
  passed, and MSVC native analysis was clean. The accessibility/DPI/multi-monitor/low-end/battery/power
  checkbox remains open until the documented clean-client physical matrix actually runs.
- Clean-client qualification now operates on the portable ZIP rather than trusting a build-tree
  executable. The runner verifies the exact copied ZIP/sidecar before extraction, refuses an occupied
  BlackBox single-instance host before hardware probes or staging, and binds/rechecks hashes for the
  launched application, runner, and independent evidence verifier. The operator helper now hashes
  the running executable instead of trusting a live PID. The strict single-bundle verifier enforces
  the exact file/directory tree (including no unexpected empty directory), links and size bounds,
  manifest hashes, invariant process rows, direct-v1 artifacts and event/writer/archive accounting;
  it streams bounded archive inspection, re-verifies the embedded package, and matches all shipped
  executable hashes/signature facts back to that package. Partial evidence is rejected by default.
  The aggregate verifier invokes that verifier for every bundle, then rejects bundle reuse, binds one
  package/source identity, and cannot pass without standard Windows 10 22H2 and Windows 11 plus
  multi-monitor, low-end, and battery profiles. Contracts cover interactive and empty-case smoke
  bundles plus checksum/traversal, duplicate/out-of-profile case, partial, provenance mismatch,
  unexpected-directory, matrix-completeness, and tamper rejection. The final independently rechecked
  unsigned packaged Windows 11 smoke at
  `out/client-qualification-smoke-v1-20260821-strict-evidence-final3/` completed 16 collections with
  zero failed/dropped/deadline-missed samples, a 55,402,496-byte maximum working set, package SHA-256
  `c5e81e1d86a9888759502a1f0e8e2260e19f646ac973dd36433f08a32d833663`, and manifest SHA-256
  `ae98650bae5e500ccd4118c9b95789e6cdf2b8d93e2ab93c5215775c66f97fa5`; its own summary still records
  every clean-client/physical/signing gate as unsatisfied. Current registered totals are 290 Release
  and Debug tests, 120 headless Release tests, 182 analysis-disabled Release tests, and 290 Windows
  ASan tests (289 applicable after the intentional crash probe exclusion). The two clean-client
  package matrices and physical matrix checkbox
  remain open until real independent hosts produce the documented interactive bundles; signing is
  also open.
- V0.17 evidence composition is now fail-closed instead of a prose-only checklist. Both hosted
  workflows have aggregate jobs that become eligible only after their required jobs succeed on a
  push or manual run; the exact direct-v1 artifacts bind GitHub SHA, repository, workflow, run ID/
  attempt, ref, and writer hash. Pull requests and local processes cannot produce release-hosted
  evidence. UI raster publication now binds `summary.ini`, source/test/runner/verifier identities,
  exact V5 BMP structure and all pixels, and invokes an independent verifier before rename. A
  separate explicit review artifact binds one fully reviewed raster manifest so generation cannot
  attest its own human review. The new local-uncommitted raster at
  `out/ui-qualification-v1-20260821-provenance-final2/` independently verifies with manifest SHA-256
  `50125c525f5a66658492102e66826bce930b6c5a536a923a0df84356b5fa7e3d`; all 30 image hashes exactly
  match the prior fully reviewed `storage-retry-reviewed` set, and its separately verified review
  manifest is `0b5e088031059cabbdd47c4b6b9d8f9d8b89b160ab38cda0e6b122e4b0991584`. Client matrix review now
  regenerates the three-file result from every retained source bundle and requires byte-for-byte
  agreement. The aggregate V0.17 runner re-invokes the timestamped package, overnight/72-hour, UI/
  review, physical client matrix, and both hosted verifiers for one lowercase 40-character revision,
  rejects evidence-role reuse, and atomically publishes only their hash ledger. It deliberately
  cannot accept `local-uncommitted`, unsigned rehearsal, smoke, partial, or authored-but-unrun CI
  evidence and does not claim the separate V0.15.1 diagnostic gate. Hosted, UI/review, matrix, and
  aggregate negative contracts pass in Release, Debug, analysis-disabled, headless, and ASan graphs;
  dependency/action and quality policy verification remains clean. The hosted execution, physical
  matrix, signed package, and final same-revision aggregate remain open because their real external
  evidence does not yet exist.
- A local Git repository now exists on branch `main` with the owner-supplied local author identity
  and an audited initial source commit. Generated build/output trees, CPack staging, local ZIP/
  checksum artifacts, and ImGui state are explicitly ignored and were verified through Git's ignore
  matcher. No remote, push, or hosted run exists because no repository URL has been created; hosted
  CI remains an external gate and is not inferred from the authored workflows.
- Repository attributes now pin LF for every source/workflow/script/document/direct-text format and
  explicitly protect packages, PE/DLL/library files, databases, dumps, fonts, and rasters as binary.
  A platform-independent CTest pins both those rules and the generated-artifact ignore set, preventing
  Windows/Linux checkout settings from changing source-script hashes or admitting local build output.

## V1.0 — BlackBox Computer Flight Recorder

**Objective:** Ship the original local-first computer flight recorder vision as a trustworthy Windows
product.

- [x] Add fail-closed V1 evidence composition that independently re-verifies V0.17, binds the exact
  signed packaged evaluator, recomputes held-out output, and matches the passing one-shot fingerprints
- [x] Pin the multi-hardware acquisition guide to all nine direct-v1 symptoms and the final client,
  V0.17, and V1 evidence instructions to the exact `BlackBox-1.0.0-windows-x64.zip`; enforce both
  through the release-claims documentation contract while retaining explicit `0.15.0` rehearsal examples
- [x] Bind every Windows product/qualification executable to its configure-time Git revision in the
  signed PE identity; require an exact clean HEAD before signing and reject binary/package/V0.17/V1
  evidence whose embedded revision differs from the declared release revision
- [x] Make client and UI qualification source identity non-self-asserted: reject a package whose
  embedded revision differs before physical execution, and reject a raster request that differs from
  the generator's compiled revision before writing the first image
- [ ] Set every final product/package/evidence semantic-version surface to exactly `1.0.0` only
  after the other release gates pass; no prerelease or release-candidate build may carry it
- [ ] All V0.10-V0.17 acceptance criteria are proven by current evidence
- [ ] The app quietly records in the tray and captures manual and automatic short-lived incidents
- [ ] Supported symptom classes have sufficient signals and measured real-world diagnostic quality
- [ ] Incident explanations distinguish probable contributors, victims/reactions, and unknown causes
- [ ] Feedback and recurrence improve the local experience within explicit safety/privacy bounds
- [ ] Release package is signed, clean-client qualified, recoverable, documented, and supportable
- [ ] Published overhead, false-positive, accuracy, reliability, and compatibility claims match evidence

V1 evidence composition is implemented but cannot currently publish a passing ledger. The V0.17
hosted, signed-package, physical-client, and wall-clock sources do not yet all exist for one release
revision, and V0.15.1 still lacks its consented multi-hardware corpus and passing one-shot result.
The final runner/verifier now also require the exact `BlackBox-1.0.0-windows-x64.zip` package,
`application_version=1.0.0` in both wall-clock reports, and `product_version=1.0.0` in the final
ledger. The current engineering build deliberately remains `0.15.0` until those gates are real.
All five Windows product/qualification executables now derive their complete PE `VERSIONINFO` from
that same CMake project version. A Windows runtime contract rejects blank or inconsistent string/
numeric versions, descriptions, product identity, internal names, and original filenames, so the
final version change cannot leave an old tool or file-property surface behind. The final V1 verifier
also extracts the signed ZIP and requires the same exact `1.0.0` identity on all three shipped
executables before it accepts the package.
The complete Debug graph verifies all five compiled resources and passes 309/309 tests. The
analysis-disabled Release graph verifies its four enabled executables, the full Release graph
verifies all four non-running tools, and the headless architecture/format/release contracts pass
3/3. A synthetic `BlackBox-0.15.0-windows-x64.zip` passed the new package identity check while an
expected-`1.0.0` mismatch was rejected. The active soak holds the older full-Release `blackbox.exe`
open, so that one artifact will be relinked after natural campaign exit rather than invalidating the
run; the separately linked Debug and analysis-disabled Release main executables already prove the
same generated resource. No runtime recording path changed.
Official signing now runs an independently testable three-binary identity preflight before it looks
up `signtool` or touches a certificate, and defaults fail-closed to expected version `1.0.0`.
Missing/link/stale/renamed/inconsistently versioned binaries are rejected before signing; an explicit
version remains available only for deliberate prerelease rehearsal. The contract passed against
the compiled `0.15.0` binaries, rejected those binaries when presented as final `1.0.0`, rejected a
renamed main executable substituted for the dataset tool, and is included in the 309/309 Debug run.
The PowerShell V1 evidence-contract test and the release-claims documentation contract pass. The
current Release suite passed 295 other tests in the restricted environment; its only failure was
the expected HKCU denial in the launch-at-login test, which passed 1/1 when rerun with registry
access. No compiled runtime source changed for this version-policy addition.
The final runner/verifier refuses `local-uncommitted`, partial evidence, reused evidence directories,
a non-packaged evaluator, independently failing V0.17 inputs, incomplete/failed diagnostic attempts,
fingerprint disagreement, and tampered source ledgers. No acceptance checkbox above is closed by
tooling alone.
The qualification instructions now agree with those executable gates: application crash is present
in the complete nine-class acquisition matrix, official interactive/final composition examples use
the exact `1.0.0` package, and only explicitly labeled current-build smoke instructions retain the
`0.15.0` filename. The documentation contract requires the corrected vocabulary and final package
name and rejects a prerelease package from either final composition guide.
The focused final-evidence/documentation contracts pass 5/5. The complete Debug graph passes 308/309
inside the restricted process environment, and the sole HKCU launch-at-login integration passes
1/1 with current-user access, completing the 309-test graph.
Release provenance is now inside the signed executable boundary. `BLACKBOX_SOURCE_REVISION=auto`
embeds an exact 40-character HEAD only for a clean Git worktree and otherwise embeds
`local-uncommitted`; all five Windows resource tests enforce it. The signing preflight independently
requires clean exact-HEAD source before binary inspection or certificate access. Binary/package and
both final composition verifiers require the embedded revision to match their evidence SHA.
Contracts exercise clean source plus wrong-HEAD, tracked-dirty, untracked, renamed-binary, direct
binary-revision, and ZIP-revision rejection. The owner-supplied author identity and initial commits
now exist locally, but no remote or hosted evidence does. The focused provenance/resource/
documentation set passes 9/9; the complete Debug graph passes 308/309 inside the
restricted environment and its sole HKCU launch-at-login integration passes 1/1 with registry access,
completing the effective 309/309 regression graph.

Qualification evidence producers now fail at the source boundary instead of relying only on final
composition to discover a mismatch. The client runner passes its declared revision into strict ZIP/
PE verification before extraction. The UI test executable compares its configure-time revision with
the runner request before raster output; the contract invokes the real executable with a different
revision and proves that no BMP is written. A matching local-uncommitted Debug run published and
independently verified 30 direct-v1 rasters at
`out/ui-qualification-v1-20260822-compiled-provenance-live-width/`. Visual inspection found that the
Live status table had collapsed every value to one character; an explicit stretch column fixed the
complete values at 100% dark and 150% high contrast. All 30 corrected cases were visually checked,
the raster manifest is `c1412fbafb8b1297712288076495916a72fa3a551beddc84fe7d9357ebec6b4b`,
and the separate local review manifest is
`7c560ba0574a0785e387e34c9656953cefda0afcf3fc1675bf40f6a972eb5789`. These are rehearsal artifacts,
not release-revision or physical-client evidence. The focused UI/provenance/architecture/documentation
set passes 6/6; the complete Debug graph again passes 308/309 in the restricted environment and its
sole HKCU integration passes 1/1 with registry access, completing the effective 309/309 graph.
The subsequent tray-notification concurrency audit found that each replacement of the bounded
single pending payload still posted another native message, allowing a burst to display empty
balloons and double-count drops. The mailbox now posts once, coalesces later payloads to the newest
lifecycle state, counts each displaced payload once, and ignores stale native messages. A
deterministic test blocks the shell thread while three notifications arrive and proves exactly one
delivery attempt and two replacements. The focused test passes 9/9 assertions; the complete Debug
graph passes 309/310 inside the restricted environment and its sole HKCU integration passes 1/1
with registry access, completing the effective 310/310 graph. The active wall-clock campaign uses
the previously bound Release executable and was not changed or restarted by this source-only fix.
The adjacent background-efficiency audit then found that the composition root's 250 ms status
refresh posted four unchanged native tray messages every second. `set_status` now returns without a
post when its finite value is unchanged, retains state set before shell startup, and rolls back only
its own failed transition so the next refresh can retry without overwriting a concurrent newer state.
The deterministic transition test passes 6/6 assertions and the notification regression remains
9/9. The complete Debug graph passes 310/311 inside the restricted environment and its sole HKCU
integration passes 1/1 with registry access, completing the effective 311/311 graph. Architecture,
performance, and roadmap documentation now state the transition-only wakeup contract. The running
wall-clock campaign remains bound to its prior Release binary and was not changed or restarted.
The isolated Windows AddressSanitizer preset was then rebuilt completely so the application and all
four companion qualification executables matched the current direct-V1 protocol and version
resources. A deliberately test-target-only first build caused the release/acquisition contracts to
reject those stale companions; after the complete preset build every such contract passed. The
ASan preset passed 309/310 applicable tests in the restricted environment and its sole HKCU startup
integration passed 1/1 with registry access, completing the effective 310/310 applicable graph. The
intentional unhandled-exception probe remains the one policy-excluded registered test because its
required access violation is the test input. Both new shell regressions pass all 15 assertions under
ASan, and no sanitizer finding occurred.
The isolated MSVC native-analysis preset subsequently rebuilt the production shell, assembled
application, portable/runtime libraries, and unified test translation units with `/analyze` and
warnings as errors. It completed with no analyzer warning; the resulting Release-instrumented test
binary also passed the two shell regressions' 15 assertions. This build graph is separate from the
active full-Release soak binary and did not relink or restart the campaign.
The isolated full-app analysis-disabled Release graph was also rebuilt from current source, including
the assembled desktop, Windows platform/telemetry, storage, UI, offline tools, and all applicable
contracts. It passed 195/196 tests inside the restricted environment and its sole HKCU startup
integration passed 1/1 with registry access, completing the effective 196/196 graph. The two shell
regressions therefore remain independent of `blackbox_analysis`; collection and the native desktop
do not acquire an ML/statistical-analysis dependency. This isolated graph likewise did not touch the
running full-Release soak executable.
The reduced Windows headless Release graph was rebuilt with the assembled app, UI/storage dependency
graph, automatic detection, and analysis disabled while retaining portable recorder plus native
provider/platform contract coverage. It passed 126/127 tests in the restricted environment and its
sole HKCU startup integration passed 1/1 with registry access, completing the effective 127/127
graph. Both new shell regressions pass there, and the architecture/direct-V1 contracts continue to
reject dependency leakage. This build remained isolated from the running full-Release campaign.
The current V0.15.1 qualification-status document now reports those rebuilt graph totals rather
than presenting the older 309/182/120/289 counts as current. After the interrupted soak released its
executable, the complete current full-Release preset relinked every production/qualification target
and passed 311/311 tests with scoped Windows process/registry access. The rebuilt application is
SHA-256 `57ac61b36dd10e8d7e5741a006135c50e8df5d5fd8c0b3b5b8959d63d1d79d69`.
A subsequent 15-second hardened smoke at
`out/soaks/hardened-current-release-smoke-v1-20260822/` independently re-verified seven checkpoints,
zero gaps, 16 collections, two required captures/writes/archive rows, and zero failed/dropped/
deadline-missed samples; its manifest SHA-256 is
`c9bcc6cd982637079d783087f8fd0dff820149d994aa3c7d23f235ae12e7e72a`.
The first clean-revision build audit exposed that CMake's regex dialect did not implement the
`{40}` repetition used by the source-identity checks: a real commit SHA was rejected and automatic
resolution could never promote a clean tree from `local-uncommitted`. Source validation now uses an
explicit 40-character length plus lowercase-hex check shared by configured input and detected HEAD,
with a platform-independent malformed-value contract. Clean revision
`f5b93840564ac9cb5ed7f7d2c113557ae365a56a` then rebuilt all Release targets, passed the independent
three-binary `0.15.0`/revision preflight, and passed 312/312 tests in 39.21 seconds. The application
SHA-256 for that revision is
`d21df9baa991114021323f45fd218ba845a36967ef5fab1a3423acbb3de0d0a9`.
The first smoke launched from that clean-revision workflow also failed closed before publication:
the standalone verifier used a newer `.NET` relative-path API absent from inbox Windows PowerShell
5.1, which is the shell the campaign invokes. That attempt remains `.partial` and receives no
release credit. Wall-clock and clean-client verifiers now normalize only full paths proven beneath
their evidence root using a cross-shell bounded prefix routine. Their complete contract suites pass
under PowerShell 7, and each valid-bundle path is additionally invoked through Windows PowerShell
5.1 when available. Replacement evidence is accepted only from a subsequent clean revision and
rebuild after the full test graph passes; the overnight, 72-hour, physical-client, hosted, signing,
and diagnostic-quality gates remain open.

The first public hosted-CI execution on revision `6e288e6131048d3d076c2bd0231f36998d158bc8`
failed closed and receives no release credit. It exposed two portability defects: CMake's
`FindSQLite3` imported target is named `SQLite::SQLite3` on the hosted CMake generation but
`SQLite3::SQLite3` locally, and GCC correctly rejected two Windows-only `getenv_s` size variables
as unused. The workflows also bootstrapped vcpkg inside the checkout, which would have embedded
`local-uncommitted` even after a successful build. The replacement resolves either supported
SQLite target behind `BlackBox::StorageDependencies`, scopes the variables to Windows, and moves
all hosted vcpkg work to an ephemeral checkout sibling; a contract now rejects regression to an in-checkout
dependency bootstrap. Hosted evidence remains open until replacement Windows and quality runs
complete successfully on the same clean revision and their attestations verify independently.
The first replacement push also failed closed before scheduling jobs because GitHub does not expose
the `runner` expression context to job-level environment declarations. That invalid configuration
receives no credit; the corrected checkout-sibling path uses the job-valid `github.workspace`
context while remaining outside the Git worktree.

The first Linux coverage execution also found that SQLite may interpret a tiny malformed file as an
empty database on Linux and initialize it, violating the rejection-without-mutation contract even
though Windows rejected the same deterministic corpus. Archive open now preflights every existing
nonempty file's exact SQLite header read-only before passing its path to SQLite; the deterministic
128-input corruption property therefore has platform-independent semantics.
The same hosted compiler pass identified two uninstantiated numeric TSV helper overloads; one
arithmetic template now performs integral promotion directly, eliminating the GCC/Clang unused-code
warnings without suppressing them. A contributor fixture likewise binds its initializer-list pairs
by const reference instead of copying each identity/name pair.

The first Windows Server 2025 matrix execution confirmed that its current runner image has no
Visual Studio 2022 instance. That leg now uses the repository's existing Visual Studio 2026 Release
preset, while Windows Server 2022 retains the VS2022 Debug/Release legs; the mapping is contract
tested instead of inferred from runner labels.

The same diagnostic run showed that hosted Clang 18 paired with its default libstdc++ does not
expose the C++23 `std::expected` API. UBSan now runs under the image's GCC 14 toolchain, while the
Clang 18 libFuzzer leg remains on libstdc++ and supplies its required C++20 concepts feature-test
value. The rejected libc++ 18 pairing was also diagnosed: it lacks `jthread` and `stop_token`, so it
cannot compile the recorder. The contract test locks the supported compiler/library pairing.
Checkout, artifact upload, and CodeQL actions were also advanced to their immutable Node
24-compatible release SHAs, preserving the full-SHA supply-chain policy while removing the runner's
Node 20 deprecation path.

The first corrected Linux coverage execution passed all 231 applicable tests, including the
cross-platform corrupt-archive property, then found that the report step assumed its output
directory already existed. The workflow now creates `out/quality` explicitly before invoking
`gcovr`, and the hosted workflow contract locks that prerequisite.

GCC 14's optimized UBSan build also diagnosed an `array-bounds` false positive inside libstdc++'s
initializer-list vector assignment for a one-element invalid-ordinal test fixture. The fixture now
uses `push_back`, preserving the exact invalid input while keeping warnings fatal and avoiding any
compiler-warning suppression.

The first complete hosted Windows ASan execution then failed closed and receives no release credit.
It exposed three Windows-host assumptions: the ASan workflow built only the app and unit-test target
even though registered release/privacy/version tests require all companion executables; a viewer
concurrency assertion treated a requested 1 ms cadence as a guaranteed Windows timer resolution; and
the accelerated collector soak left a reader that ignored `jthread` stop requests, so assertion
unwinding could wait until CTest's 25-minute timeout. The ASan leg now builds the complete graph, the
viewer test requires observable collection progress rather than a host-specific sample count, and
the soak performs 25 complete ring wraps with a stop-aware adversarial reader. Hosted contracts reject
regression to the target-limited ASan build. The complete local Release graph passes 312/312; the
complete local ASan graph passes 310/311 inside the restricted environment and its sole HKCU
launch-at-login integration passes 1/1 with registry access, completing the effective 311/311
sanitizer graph. Replacement same-revision hosted evidence remains open.

The next hosted attempt was also diagnostic and receives no release credit. Its quality graph
completed dependency review, dependency policy/SBOM, native fuzzing, Linux coverage, UBSan, the
complete Windows ASan suite, and MSVC static analysis successfully before the superseded run was
cancelled during CodeQL. The Windows headless graph exposed one remaining sub-timer-resolution test
assumption: a 1 ms normal cadence and 5 ms metadata cadence caused every wake on a coarse Windows
clock to cross the next metadata deadline, correctly producing 17 slow requests from 17 samples.
The cadence test now uses 20 ms normal and 250 ms metadata intervals and waits for two real metadata
cycles, directly asserting that normal requests occur between slow requests. It passes 50 consecutive
Release repetitions and the complete 128/128 fully headless Windows graph. Both superseded hosted run
records were then permanently deleted to release capacity and keep Actions history limited to useful
evidence. Final same-revision hosted evidence remains open.

A subsequent hosted diagnostic remained fail-closed and receives no release credit. Windows
headless exposed two single-observation integration assumptions: one valid interval could report a
zero process-write delta while the CPU/memory workload remained observable, and one crash-probe
launch could exit abnormally without a completed dump despite prior hosted success. The process
workload test now observes up to 25 bounded 100 ms intervals and requires positive CPU, memory, and
write evidence within that real workload; the crash test permits at most three isolated launches but
still requires exactly one bounded `MDMP` artifact. Both hardened integrations pass 10 consecutive
fully headless Windows repetitions. The same diagnostic also reproduced GCC bug 68080 as a negative
branch hit in otherwise successful 231/231 coverage tests. The pinned gcovr invocation now applies
only its documented `negative_hits.warn_once_per_file` recovery, preserving warnings and coverage
floors while treating the impossible negative count conservatively instead of aborting report
generation. A workflow contract locks that narrow exception. The two superseded run records were
permanently deleted; final same-revision hosted evidence remains open.

The next hosted Windows diagnostic also failed closed and receives no release credit. Both Release
matrix legs passed 311 of 312 tests but exposed the same final compiler-speed assumption: the viewer
concurrency test expected a large real statistical analysis to remain busy for at least ten requested
1 ms collection intervals, while the hosted VS 2026 and VS 2022 runners completed it after only two
and one additional samples. The test now pauses the real statistical analyzer at an explicit bounded
handoff, proves ten collector samples arrive while the viewer worker is occupied, resumes analysis,
and still verifies the completed statistical result. This preserves the production threading path
without treating workload size or timer cadence as elapsed-time synchronization. The repair passes
100 consecutive Release repetitions, 50 consecutive Windows AddressSanitizer repetitions, the
complete 312/312 local Release graph, and the complete 312/312 local AddressSanitizer graph. The
parallel quality diagnostic independently passed Windows ASan, MSVC static analysis, dependency
review, dependency policy/SBOM, Linux coverage, UBSan, and native fuzzing before the superseded run
was cancelled during CodeQL. Both obsolete run histories were then permanently deleted; final
same-revision hosted evidence remains open.

The first attempted overnight campaign on clean revision
`b8930033412e1b6c5ef76491bf2ba5c03b1e0764` was stopped after 6,242 elapsed seconds and 104
checkpoints, with zero sampling gaps, because its generated product settings omitted the required
direct-v1 `record_process_lifecycle` field. The production parser correctly rejected the entire
file, startup correctly fell back to defaults, and automatic Windows network evidence consequently
created 16 unintended captures in addition to six scheduled captures. Average total-machine CPU was
1.57%, also above the 1% long-mode gate. The retained `.partial` directory is diagnostic evidence
only; the queued 72-hour handoff was cancelled and no process remains active.

The harness now validates both generated settings files through the assembled application's
production parsers before launching timed work. Its app report binds the compiled source revision and
publishes effective automatic-detection, detector-trigger, automatic-capture, and event-request
counters; the runner and standalone verifier require all four to be zero and reject a revision
mismatch. A new 20-second end-to-end smoke at
`out/soaks/settings-provenance-smoke-v1-20260822/` passed and independently re-verified ten process
checkpoints, two scheduled captures, matching `local-uncommitted` provenance, and zero automatic
detection/trigger/capture/event requests. A fresh clean-revision overnight is still required; this development smoke receives no
release credit.

The replacement hosted Windows run on revision
`fb9165b53950e59ff721dbc797d71134787776b1` then exposed a real crash-evidence publication race:
all three bounded probe launches exited abnormally on the Debug Windows 2022 leg, but a transient
file-sharing/access conflict prevented the flushed `.dmp.partial` artifact from being renamed to a
completed `.dmp`. Production publication now retries only access, sharing, and lock conflicts for a
bounded 500 milliseconds without allocating on the crash path; permanent failure remains visibly
incomplete as `.partial`. A deterministic handle-contention integration test proves delayed release
publishes exactly one completed dump, while the real crash probe now reports retained partial counts
and bytes on failure. The real probe passed 50 consecutive Release and 50 consecutive Debug
repetitions after the repair. The complete local Release and Debug graphs both pass 313/313 tests;
the first Debug pass's sole launch-at-login failure was reproduced as the restricted test sandbox
denying creation of a missing HKCU Run key, and the same test passed 20 consecutive runs plus the
complete graph with normal current-user registry access. The failed Windows run receives no release
credit and its parallel quality run was cancelled as revision-obsolete. Replacement revision
`949e919b014beff5062c3cc1caa54e1bd45ef26d` then passed the complete Windows validation and quality/
security workflows. Their downloaded direct-v1 attestations independently verify against the local
writer at `out/hosted-ci/949e919b014beff5062c3cc1caa54e1bd45ef26d/{windows,quality}`.

A fresh exact-revision overnight campaign started from that frozen Release executable at
`out/soaks/overnight-v1-949e919-20260822.partial`. It was deliberately stopped as revision-obsolete
without publication after 19,206 elapsed seconds (about 5 hours 20 minutes), 320 process checkpoints,
and zero sampling gaps. No process remains active and the stale `state=running` checkpoint cannot be
treated as completed evidence. The retained `.partial` directory remains diagnostic-only; a fresh
exact-revision campaign is required after integration.

The resource-aware project audit is published in `docs/PROJECT_AUDIT_2026-08-22.md`. Its actionable
one-host engineering tracks are now implemented:

- [x] Decompose application lifecycle/reporting, product/archive settings rendering, and SQLite
  backup/restore into responsibility-specific translation units after an 11-test characterization
  baseline; the public graph, shutdown order, renderer surface, and direct-v1 archive remain unchanged.
- [x] Add a Linux-only CPU/memory `ITelemetryProvider` with bounded strict `/proc` parsing, portable
  malformed/overflow tests, a native provider-contract test in hosted Linux graphs, and Linux app
  composition without claiming product support.
- [x] Add a build-only offline model harness that exports sibling-staged label-free versioned feature
  matrices from read-only archives and compares independently verified candidate/baseline artifacts
  from the same frozen corpus under predeclared non-inferiority tolerances.
- [x] Add one semantic native UI visual system with explicit standard/high-contrast roles, production
  system-font fallback, selected navigation, glanceable Live cards, and grouped archive/detail/settings
  surfaces without changing the primitive view-model/command boundary.
- [x] Broaden the Linux-only provider through existing cumulative-counter normalization with bounded
  physical-device disk, non-loopback network, and stable-identity process CPU/RSS/I/O evidence; share
  the fixed-capacity entity lifecycle tracker with Windows and strictly test malformed/overflow cases.
- [x] Preserve the hosted Linux headless graph while additionally building the complete native Linux
  desktop target and running a bounded diagnostic-report smoke under a virtual display. This is
  engineering evidence, not a product-support or packaging claim.
- [x] Reuse Linux `/proc` and `/sys` scratch capacity, add a real-host all-tier provider benchmark with
  bounded P95/maximum/RSS evidence, and require CI to extract and launch an explicitly unsupported TGZ
  engineering preview before the Linux boundary passes.
- [x] Make the replaceable core logger produce bounded single-line component records, keep the default
  elapsed-time output readable, and invoke custom sinks outside logger locks so reentrant adapters cannot
  deadlock or inject multiline/unbounded records.
- [x] Add a separate Ubuntu 24.04, Debian 13, and Fedora 43 desktop/package compatibility matrix that
  builds/tests the native graph, measures 64 real provider samples, smokes the build-tree and extracted
  TGZ, and publishes comparable direct-v1 package-size/provider-overhead evidence.
- [x] Add the native Linux background boundary behind `IBackgroundShell`: SDL tray commands, a
  nonblocking per-user instance lock, exact atomic XDG autostart ownership, explicit unavailable
  notifications, no-tray fallback, and Linux-only lifecycle/autostart tests.
- [x] Replace fixed auto-sized onboarding with a bounded responsive, scrollable, keyboard-guided
  recorder/capture/review flow and live readiness summary; add compact 100% and 200%-high-contrast
  raster cases with strict 32-image publication/review contracts.
- [x] Strengthen clean-client physical qualification so the standard profile starts at first run and
  requires separate keyboard-only onboarding plus focus-visibility/increased-text observations before
  its existing 100/125/150/200%, high-contrast, tray, capture, and navigation cases can pass.
- [ ] Execute and retain the new three-distribution hosted matrix on the exact integration revision.
- [ ] Execute the strengthened physical Windows accessibility/DPI matrix on qualifying real clients;
  authored cases and deterministic rasters do not satisfy this gate.

The resulting Windows Release graph builds completely and passes 330/330 tests. A representative
schema-v1 archive also produced a three-row direct-v1 feature matrix end to end. These tracks do not
weaken the Windows-first architecture, claim Linux/macOS support, adopt a runtime model without
held-out value, or satisfy any external V1 evidence gate.

## V2.0 — Optional advanced intelligence and additional platforms

- [ ] Consider native ML only behind the V0.16 adoption gate
- [ ] Consider Linux/macOS providers only after Windows quality remains intact
- [ ] Consider opt-in anonymous feature-level collective intelligence as a separate privacy-reviewed product

## Exact next milestone

Proceed to **hosted validation and isolated integration**: run the new Ubuntu/Debian/Fedora matrix on
the exact feature revision, harden any genuine distro/package/shell failure, and retain its comparison
artifact. Review the new 32-image onboarding/page raster set. Do not change or replace the currently
frozen overnight executable; after that campaign publishes or retains diagnostic evidence, integrate
this branch, rebuild one clean revision, and start the next exact-revision campaign. macOS remains
behind the proven Linux desktop boundary, and runtime ML remains behind representative held-out value.
The signed Windows package, operator-assisted 72-hour actions, independently reviewed physical UI/
client matrix, consented multi-hardware corpus, and one-shot held-out result remain V1 evidence-
execution gates; local rehearsals or additional schema machinery cannot satisfy them.
