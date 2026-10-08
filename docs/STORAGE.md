# Capture, archive and viewer reference

[Documentation index](README.md) · [Current candidate](CURRENT_CANDIDATE.md)

- [Incident storage](#incident-storage)
- [Incident capture](#incident-capture)
- [Incident viewer](#incident-viewer)
- [Incident classification and offline dataset](#incident-classification-and-offline-dataset)
- [Logging](#logging)

## Incident storage

### Archive boundary

SQLite stores completed immutable incidents; it is not the rolling recorder. The collector owns sampling and the two-slot incident handoff, while `IncidentWriter` owns a separate `std::jthread` that removes completed work and calls `IIncidentArchive::store`. Storage depends only on core incident types. It cannot poll telemetry, call the collector, or make the collector wait for SQLite.

The writer retains only its current immutable incident and makes at most three attempts. `busy` and
I/O failures receive bounded 25 ms then 50 ms backoff; permanent errors such as full, corrupt,
invalid schema/data, or unopened archives fail immediately. Retry attempts, exhaustion, recovery,
and the last native error are observable. No retry can enlarge the core two-slot queue or involve
the collector. Shutdown stops capture acceptance and joins the collector before draining the writer
and closing the archive. The explicit cancel policy is available to callers that prefer a fast
shutdown.

### Location and limits

The Windows default is `%LOCALAPPDATA%\BlackBox\incidents.sqlite3`. `BLACKBOX_ARCHIVE_PATH` overrides it for controlled runs. Other platforms use `$XDG_DATA_HOME/blackbox/incidents.sqlite3`, then `$HOME/.local/share/blackbox/incidents.sqlite3`, with a local fallback only when none of those environment locations exists.

The default logical database limit is 1 GiB, implemented with SQLite `max_page_count`. A configured limit never destructively shrinks an existing archive: when an existing database already exceeds it, its current page count becomes the effective floor. WAL and shared-memory sidecars are transient and can temporarily make physical files exceed the logical limit. BlackBox never silently deletes incidents; reaching the cap fails and rolls back the new incident. V0.10 adds only explicit offline retention and privacy-purge commands, described in `USER_GUIDE.md` and `RELEASE_READINESS.md`.

### Durability and concurrency

Connections enable foreign keys, `synchronous=FULL`, WAL journaling, a 1,000-page automatic checkpoint, and a 250 ms default busy timeout. Every incident uses prepared statements inside one `BEGIN IMMEDIATE` transaction. Its incident header, system samples, extended forensic gauges, normalized system events, process identities/metadata, and process samples therefore commit together or roll back together.

WAL lets readers coexist with the single writer, while `FULL` prioritizes committed-incident durability over write latency. The archive object serializes its connection, and the application runs all viewer repository operations outside the render loop.

Offline evidence inspection and diagnostic evaluation select the explicit `read_only` open mode.
That mode requires an existing non-link regular file, opens SQLite with `SQLITE_OPEN_READONLY`, sets
connection-level `query_only`, validates the exact canonical schema-V1 layout, and skips directory creation, schema
initialization, journal changes, secure-delete/durability changes, and size-limit changes. Read APIs
remain available; every write API is rejected by SQLite. The normal desktop and maintenance paths
use the default `read_write` mode. Read-only mode is evidence protection, not a compatibility reader:
non-v1 archives remain rejected unchanged.

### Pre-release schema version 1

Both `PRAGMA user_version` and the singleton `schema_metadata` row record schema version 1. The
application ID is `1111644209`. Acceptance also compares the ordered definitions of every
non-internal table and index against a fresh isolated in-memory database created from the one
compiled schema. This detects missing or extra objects and changes to columns, keys, constraints,
or indexes even when a development database still claims version 1. Tables are normalized as
follows:

| Table | Purpose and key |
|---|---|
| `incidents` | One capture header per row; UTC creation time, monotonic window, manual/automatic trigger provenance and evidence, recorder epochs, row counts, private label/note, category, noticed-problem feedback, recurring-group override, and random 128-bit export key |
| `incident_classification_history` | Bounded category/feedback changes, UTC change time, and capture/user/dataset-import origin keyed to an incident |
| `system_samples` | Chronological system metrics keyed by `(incident_id, sample_index)` |
| `system_quality_samples` | Capability/status-preserving physical-disk latency, queue/service-concurrency, and passive-network quality evidence keyed one-to-one with a system sample |
| `system_extended_samples` | Capability/status-preserving GPU, process or opaque application foreground identity, DPC/ISR, CPU frequency/thermal, power, battery, and uptime evidence keyed one-to-one with a system sample |
| `system_pressure_samples` | Independent Linux PSI interval fractions, coarse thermal/memory-pressure states, VM activity rates, scheduler wake delay, and processor topology, each with explicit availability and keyed one-to-one with a system sample |
| `system_events` | Privacy-normalized power/device/audio/Windows activity plus opt-in process lifecycle context keyed chronologically within an incident; only lifecycle rows may reference a durable incident-local process identity, and no native message/payload is stored |
| `process_identities` | Full `(PID, creation token)` identities and optional static metadata, keyed within an incident |
| `process_samples` | Chronological process metrics with a foreign key to the full incident-local identity |
| `executable_profiles` | Bounded normalized executable keys, display names, and last-seen UTC time |
| `executable_profile_observations` | One idempotent evaluation-window statistic per executable/incident for CPU, working set, and disk rates |
| `feedback_profile_state` | Singleton revision, active UTC cutoff, previous cutoff, and one-step rollback flag for local feedback influence; annotations remain on incidents |
| `incident_contributor_feedback` | One explicit confirmed/not-a-contributor attribution per incident, normalized executable key, resource, and source temporal role; repeated edits replace the vote and `Unsure` removes it |
| `incident_feature_cache` | Derived versioned feature dimensions and availability bits keyed by incident/index for recurring discovery |
| `schema_metadata` | Singleton schema version and creation timestamp |

Indexes support newest-first and label-ordered incident discovery, chronological system/process reads, and identity-oriented process reads. Viewer queries are paginated, can search labels/notes, and can order by creation time, duration, or label. Foreign keys cascade incident deletion to child rows.

Metric units are unchanged from the core domain: CPU and memory utilization are fractions,
memory/working-set/compressed-memory values are bytes, I/O, VM activity, and network rates are bytes
per second, disk latency/service time and scheduler wake delay are seconds, disk queue is requests,
disk service concurrency is the average number of requests being serviced, TCP retransmission is a
fraction, network events and processors are counts, and observation/window times are monotonic
nanoseconds. Every metric stores its explicit availability status. Schema checks require a value
when that status is `available`.

SQLite signed integers cannot exactly represent every core `uint64_t`. Capture sequences, recorder
epochs, process creation tokens (including lifecycle-event references), opaque foreground session and
application tokens, and unsigned byte counts are therefore stored as fixed eight-byte big-endian
blobs. This preserves values through `UINT64_MAX` and keeps PID reuse protection exact. Offline
dataset export excludes both kinds of foreground identity.

### Pre-release reset and recovery

BlackBox has not been publicly released, so pre-release schemas are not an installed compatibility
contract. The current complete layout is version 1 and is created directly in one transaction.
There are no historical compatibility branches, older settings readers, or older dataset import
paths. A version other than 1, an unversioned non-empty database, or a development database missing
any required direct-V1 object, exact definition, application identity, metadata row, or singleton
feedback control state is refused without renaming, replacing, deleting, or upgrading it.
There is no migration path: incompatible pre-release development archives must be explicitly
recreated. Schema evolution after the first public release will require a separate, explicitly
designed compatibility contract.

Restore opens its source read-only and runs integrity plus the same full canonical-layout check
before creating the required safety backup or touching the active archive. A rejected source and
the active archive remain unchanged; restore never serves as a schema conversion mechanism.

Profile observations are not continuous telemetry. They are written only after an incident is
opened and analyzed on the viewer worker. `(executable_key, incident_id)` makes repeated views
idempotent. History is limited to 64 observations per key over 30 days and 2,048 keys total, with
deterministic least-recently-seen eviction. Profile transactions use the same error handling and
logical archive cap as incidents; a profile failure never changes the stored incident or recorder.

Feedback calibration reads at most 256 prior answered automatic incidents on the viewer worker.
The query is strictly earlier than the incident being analyzed and applies the singleton reset
cutoff before analysis receives value-only observations. Reset and one-step rollback are atomic
updates of that singleton direct-v1 row; they never rewrite feedback, classification history, or
sample evidence. Privacy purge returns the singleton to its initial revision-zero state. There is
no migration table or compatibility reader.

Contributor attribution is stored in `incident_contributor_feedback`, not inferred from the
incident-level noticed-problem field. The row retains whether the source process activity was
preceding, marker-spanning/ambiguous, or wholly post-marker. The primary key
`(incident_id, executable_key, resource)` makes repeated edits replace one vote rather than amplify
it; choosing `Unsure` deletes that vote.
History queries are newest-first, strictly prior to the incident, and capped at 256 rows. The
portable analyzer independently reapplies exact identity/resource, feedback-entry time, 90-day,
duplicate, reset-cutoff, and source-temporal-provenance rules. Only confirmations originating from
preceding activity can teach positive uplift. Foreign-key cascade removes attribution when its incident is
explicitly retained/purged; normal recording never reads or writes this table.

Recurring discovery reads the existing category and noticed-problem fields with each bounded
incident summary. The application maps only same-cluster values into portable analysis observations;
storage does not score, classify, or learn from them. Similar-incident evaluation independently
reapplies strict prior-time, 90-day, 32-row, duplicate, and feedback-reset bounds before publishing
historical context.

Recurring feature rows are derived post-capture on the viewer worker. A changed feature version
replaces an incident's cache transactionally; at most 512 incidents and 32 dimensions per vector
are accepted by the storage contract; rows outside the newest 512 incidents are pruned. Cache failure leaves the source incident untouched and
causes later recomputation. Manual recurring-group overrides persist independently of the cache.

Busy/locked, full, corrupt/not-a-database, unavailable/read-only path, I/O, invalid schema/data, and general SQL failures have distinct error categories. BlackBox never deletes, renames, recreates, or overwrites a corrupt or future-version archive automatically. Recovery is an explicit future/user operation so a startup failure cannot destroy diagnostic history.

## Incident capture

### Capture behavior

A manual trigger records a monotonic event time `T`. The default requested range is `T - 120 s` through `T + 30 s`; both durations are part of `RecorderConfiguration`, are independently configurable, and must be nonnegative. The collector continues its normal schedule during the post-window. The first observation at or after the requested end completes the capture, so the immutable snapshot's actual end can be up to one sample interval later than the requested end.

Short uptime and ring boundaries never fabricate history. Snapshot construction selects only samples whose observation times are at or after the requested start and at or before the completion observation. `actual_start` and `actual_end` report the first and last system sample that were really retained. Recorder epochs are copied into the incident header so a future storage layer cannot silently join samples across a reconfiguration.

The incident contains:

- normalized system samples with explicit units and availability states, including either a durable
  foreground process identity or a non-correlatable opaque application key when opted in and native;
- flattened process samples carrying their frame observation time and full PID/creation-token identity;
- only process metadata referenced by those selected samples;
- the original event, requested range, actual range, manual/automatic trigger counts, strongest
  automatic resource evidence, and recorder epochs.

The core snapshot exposes const spans only. Completed work is shared as `std::shared_ptr<const IncidentSnapshot>`, allowing the writer to consume the same immutable object without another full telemetry copy. The dedicated writer persists one complete incident transaction and reports its success, latency, or failure through dashboard diagnostics.

### Overlap and queue policy

BlackBox permits one collecting post-window at a time. A manual or automatic trigger received while
that window is active merges into it: the earliest requested start is retained, the requested end
extends to the latest trigger plus its post-window, and source-specific trigger counts increment.
The strongest automatic score/evidence is retained. This prevents repeated hotkeys or detector
observations from multiplying large overlapping snapshots.

### Automatic capture and feedback

V0.4 can detect sustained hard-threshold or statistically extreme CPU, memory, disk, and network
events on the normalized collector stream. Confirmation takes at most three available samples (three
seconds at the default cadence). The detector uses fixed storage, emits at most one resource per
sample, and applies a global 120-second cooldown. It requests the same pre/post window and bounded
coordinator used by the hotkey; it has no storage, UI, OS, or post-capture analysis dependency.

Selecting an automatic incident shows the retained resource, observation, baseline, and detector
score and asks “Did you notice a problem at this time?” Yes/no feedback is stored with the incident
in the pre-release schema-v1 baseline and survives restart. The answer is evidence for later evaluation, not a cause label.
Manual capture remains available regardless of detector build configuration.

Completed or in-progress snapshots reserve slots in a fixed two-item FIFO. If both slots are reserved and no capture is already available to merge, a new trigger is rejected immediately. The queue never grows, and the dashboard exposes capacity, queued count, rejections, construction failures, merges, completions, and cancellations. The writer removes queued work asynchronously; its one in-flight immutable incident is outside the two source slots, so at most three completed immutable snapshots can be retained across source and writer at once.

Stopping collection first stops acceptance and cancels an incomplete post-window. The application unregisters and joins the hotkey service before stopping the collector, so a native callback cannot race destruction. It then drains queued writer work, joins the writer, and closes SQLite before tearing down the UI. A drain failure is observable but never restarts collection during shutdown.

The incident viewer has a separate fixed-capacity durability queue for accepted annotations,
contributor feedback, recurrence overrides, and feedback-profile controls. Refresh/detail work may be
coalesced or cancelled, but accepted mutations are never evicted and are drained in submission order
at shutdown. A persistence failure increments `failed_mutations`, does not increment
`completed_mutations`, and does not terminate the worker: later accepted mutations still run. Queue
depth, coalescing, cancellation, rejection, successful completion, and failed completion are exposed
as bounded primitive diagnostics; they never enter the collector path.

### Windows hotkey

The default chord is `Ctrl+Shift+F12`. `IGlobalHotkeyManager` contains no Win32 types. `WindowsGlobalHotkeyManager` registers the chord with `MOD_NOREPEAT` on a dedicated ordinary-user message thread, receives `WM_HOTKEY`, and emits the capture callback. Clean shutdown posts `WM_QUIT`; the same thread calls `UnregisterHotKey` before exiting.

Windows may reject a chord already owned by other software. Registration status is visible in the dashboard and the capture button remains usable. Microsoft also documents F12 as debugger-reserved, so a failure is treated as an explicit conflict/unavailable state rather than silently changing the requested default. The V0.0.7 validation host successfully registered and unregistered `Ctrl+Shift+F12` without elevation. Sources: [RegisterHotKey](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-registerhotkey), [WM_HOTKEY](https://learn.microsoft.com/windows/win32/inputdev/wm-hotkey), and [UnregisterHotKey](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-unregisterhotkey).

## Incident viewer

### Thread and memory boundary

The render loop never calls SQLite. `IncidentViewerService`, owned by the application composition root, executes repository list, load, search, and annotation-update jobs on its own `std::jthread`. It publishes immutable `shared_ptr<const IncidentViewerContent>` snapshots; the UI reads one pointer and emits commands. A slow query can delay another viewer/writer database operation on the serialized archive connection, but it cannot wait or stop the collector.

Archive discovery is paged at 50 incidents in the application and hard-capped at 100 rows per repository call. Search runs across labels and notes in SQLite instead of loading the archive into UI memory. Only one selected incident is loaded at a time. Incident data remains bounded by the recorder's existing 86,400 system-sample, 600,000 process-sample, and 8,192-identity limits. The process table displays at most 500 filtered identities at once.

### Time and ranges

Creation timestamps remain signed Unix epoch milliseconds in SQLite and are displayed explicitly in UTC as `YYYY-MM-DD HH:MM:SS.mmm UTC`. No implicit local-time conversion is applied. Telemetry observation times remain monotonic nanoseconds and are plotted as seconds relative to the incident marker, which is always zero.

The detail header shows requested and actual start/end ranges separately. Negative plot times are pre-incident, positive times are post-incident, and a vertical event-marker series is drawn at zero. Short uptime therefore remains visible as a shorter actual pre-window instead of appearing to contain fabricated history.

### Timeline policy

Every system and selected-process metric carries its availability counts into the view model. Available points are plotted; an empty series reports unsupported, inaccessible, and temporarily-unavailable counts rather than drawing zero values.

Series with at most 2,048 available points are retained exactly. Larger series use chronological min/max buckets while preserving first and last points. Emitting both extrema from each bucket retains a narrow spike or dip that ordinary averaging could erase. Each metric is decimated independently because its missing-value pattern can differ.

The process table aggregates by full PID/creation-token identity and exposes sample count, peak CPU, peak working set, and peak disk read/write. Name/path/PID filtering is case-insensitive. Sorting supports name, PID, and each peak metric in both directions. Selecting a row rebuilds only that process's four bounded graph series from the already-loaded incident on the viewer worker.

Foreground transitions backed by a durable process identity resolve to local process metadata and may
show matching GPU evidence. A compositor-specific Wayland transition is labeled only with a short
private application token; the viewer never guesses a process name/PID or attaches GPU evidence to it.

### Labels and notes

The pre-release schema-v1 baseline includes a label and note for every incident. Labels are limited to 128 UTF-8 bytes and notes to 4,096 UTF-8 bytes at the repository boundary; schema checks also cap stored character counts. Updates use prepared statements in a transaction and survive restart. V0.0.9 treats both fields as local diagnostic data and provides no export or synchronization.

### V0.2 anomaly ranking

After an incident load, the same viewer worker invokes the optional statistical analyzer before
publishing detail. The render loop receives only primitive rows: candidate name, bounded score,
confidence text, and strongest evidence text. Four resource rows and at most 20 process rows are
visible. Selecting a process preserves the prior analysis result rather than recomputing it.

Analysis time is reported separately from SQLite query and view-model build time. Disabled,
cold-start, ready, and error states are distinct. The viewer explicitly says that unusual behavior
is not proven causation. See `ANALYSIS.md` for formulas and candidate bounds.

### V0.3 personalized profiles

The viewer worker now derives normalized executable keys, loads their bounded prior observations,
passes portable history into analysis, and stores one idempotent observation per executable after
scoring. The render loop still receives only primitive rows. Evidence labels its incident-local or
personal-executable scope; profile cold start and observation count are explicit.

Profile database errors do not fail incident load: the worker falls back to incident-local
analysis and reports that history/update was unavailable. Profile work shares the serialized
archive connection with ordinary viewer/writer jobs, but has no path to the collector. Selecting a
process timeline reuses the already-published personalized result.

### V0.4 automatic-capture feedback

An automatic incident detail displays the retained trigger resource, observation, rolling baseline,
and detector score. Unanswered incidents ask whether the user noticed a problem; yes/no updates run
on the existing viewer worker and persist as schema-v1 incident feedback. Manual incidents do not
show the prompt. Saving a label or note preserves the current feedback value, and selecting a
process timeline reuses it from the loaded annotation state.

### V0.5 incident classification

Every manual or automatic incident exposes one primitive category selector: Unknown, System
freeze, Game stutter, Application slowdown/hang, Network, or Audio. The viewer worker saves the
selected category together with noticed/not-noticed feedback while preserving the independent
free-form label and note. Category/feedback changes append transactional schema-v1 history;
label/note-only edits do not. Selecting a process timeline preserves the loaded classification.

Classification is post-capture storage/UI state. It is not part of `core::IncidentSnapshot`, does
not enter telemetry or analysis, and cannot influence collection, detector decisions, or recorded
samples. See `STORAGE.md` for the offline exchange boundary.

### V0.16 explicit contributor attribution

Each ranked contributor exposes an `Unsure` / `Confirmed contributor` / `Not a contributor`
selector. The viewer worker stores one replaceable direct-V1 row per incident, normalized
executable key, and resource, then reloads the immutable incident. This attribution is not the
incident-level noticed-problem answer and does not change the current rank. For future exact
matches, portable analysis may apply a bounded consensus multiplier while retaining the raw score.
The row exposes cold/conflicting/active profile state and counts. It separately labels preceding,
marker-spanning/ambiguous, and wholly post-marker possible-victim/reaction roles. Only a confirmation
recorded on genuinely preceding activity is eligible for positive future learning; neither of the
other roles can be positively promoted. Storage and analysis work remain off the render thread.

## Incident classification and offline dataset

### Categories and history

The pre-release schema-v1 baseline defines six stable categories: `unknown`, `system_freeze`, `game_stutter`,
`application_slowdown_or_hang`, `network`, and `audio`. Category and noticed/not-noticed feedback
are independent of the private free-form label and note. Newly captured incidents begin at
Unknown. A transaction appends history only when category or
feedback actually changes. The newest 64 events per incident are retained, with `capture`,
`user`, or `dataset_import` origin.

Each incident receives a random 128-bit export key. It has no path, account, device, executable,
or process meaning and is used only to match an exported classification back to the same local
archive. Export keys do not allow an export to create incidents or import telemetry into another
archive.

### Pre-release dataset version 1

`blackbox_dataset_tool export <archive> <new-directory>` creates exactly these UTF-8 files:

- `manifest.json`: format/application versions, time bases, recorded-value status codes, units,
  and the privacy exclusion list.
- `incidents.tsv`: pseudonymous key, current category/feedback, capture provenance/evidence, and
  sample counts.
- `system_samples.tsv`: incident-relative timestamps plus explicit status/value pairs for CPU,
  memory, throughput, physical-disk/network quality, GPU/memory, foreground-GPU usage, DPC/ISR,
  frequency/thermal limit, power/battery, and uptime.
- `system_events.tsv`: privacy-normalized source/kind/level/native event ID/numeric detail,
  incident-relative timestamp, and an optional incident-local process ordinal for lifecycle context.
  It contains no rendered Event Log message, PID, creation token, or native identifier payload.
- `process_samples.tsv`: incident-relative timestamps plus metric status/value pairs. A process is
  represented only by an incident-local ordinal assigned during export.
- `classification_history.tsv`: bounded category/feedback history and change origin.

The exporter loads one bounded incident at a time and pages archive discovery. It refuses an
existing destination so it cannot overwrite user data. If creation fails, it removes only the new
incomplete destination it created.

### Privacy boundary

Version 1 intentionally excludes the archive path, executable paths, process names, PIDs, process
creation tokens, parent PIDs, foreground process identity, Event Log messages, device/audio
endpoint identifiers, window titles, free-form labels/notes, recurring-group overrides, and
derived feature-cache rows. It retains raw resource and
per-process metric values, timestamps relative to each incident marker, UTC incident/change times,
capture provenance, pseudonymous lifecycle ordinals, and random export keys. Those remaining values can still reveal workload
patterns or when a problem occurred; inspect the files and apply your sharing policy before
distributing a dataset.

Because the application is unreleased, the importer accepts only the current complete version-1
manifest/header. Compatibility branches begin only after a public format is established. Import
still cannot modify telemetry.

No hidden path or metadata column is inferred on import. `import-classifications` reads only the
current category and feedback from `incidents.tsv`, matches an existing random export key, and
updates those two fields. Unknown keys are ignored. Telemetry snapshots, trigger evidence,
free-form labels/notes, and process metadata are never written by import. Reimporting unchanged
data is idempotent and creates no duplicate history event.

## Logging

BlackBox uses the small replaceable logger in `core`; runtime modules do not depend on a logging
framework. The default sink writes one record per physical line:

```text
[blackbox +125ms] [info] [telemetry] Provider sampling recovered
```

The elapsed timestamp is relative to process start and is diagnostic ordering evidence, not wall-clock
time. Components are bounded to 32 bytes and messages to 2,048 bytes. Leading/trailing whitespace is
removed, internal whitespace is collapsed, control bytes are replaced, and truncated fields end in
`...`. Warning and error records flush immediately.

Custom sinks receive `(level, component, message)` after normalization. The logger copies the active
sink while holding its configuration lock and invokes it after releasing the lock, so adapters may
reenter or replace the sink without deadlocking. Sinks must not retain either `string_view`; both views
refer to bounded call-local storage. Logging remains transition-oriented on collection paths so a
persistent provider failure does not produce one record per sample.

## Bounded recovery and incremental maintenance

The production coordinator reserves at most two queued/constructing snapshots; the writer
can own one additional snapshot. Exhausted failures retain all three accepted snapshots in
a fixed-capacity recovery array and close new capture admission. Recovery is oldest-first;
new failures never evict earlier recovery data. Recovery status remains visible independently
of the last write result. This capacity assumes the production coordinator bound; custom work
sources must respect it. Explicit purge clears queued work and all retained recovery snapshots.

For file-backed archives, backup opens a separate read-only SQLite connection and pins a WAL
snapshot before releasing the active archive mutex. Copying advances in 128-page batches,
then reopens and verifies the staging database before exclusive publication. A progress callback
may observe page counts; an operation stop token and default two-minute deadline interrupt
copying, lock retries and verification. Memory-only archives and restore remain exclusive.
The deadline is cooperative and cannot bound a blocked kernel filesystem call.

A long read transaction can retain WAL pages while writers continue. The deadline limits
normal exposure, but operators should leave free disk space. Cancellation during restore rolls
back unfinished SQLite backup work; a successfully committed restore is reported as success even
if cancellation arrives too late. Maintenance reports accepted, queued, completed, failed and
cancelled work, and drains accepted jobs on orderly shutdown.
