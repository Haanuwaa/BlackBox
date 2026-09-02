# Incident storage

## Archive boundary

SQLite stores completed immutable incidents; it is not the rolling recorder. The collector owns sampling and the two-slot incident handoff, while `IncidentWriter` owns a separate `std::jthread` that removes completed work and calls `IIncidentArchive::store`. Storage depends only on core incident types. It cannot poll telemetry, call the collector, or make the collector wait for SQLite.

The writer retains only its current immutable incident and makes at most three attempts. `busy` and
I/O failures receive bounded 25 ms then 50 ms backoff; permanent errors such as full, corrupt,
invalid schema/data, or unopened archives fail immediately. Retry attempts, exhaustion, recovery,
and the last native error are observable. No retry can enlarge the core two-slot queue or involve
the collector. Shutdown stops capture acceptance and joins the collector before draining the writer
and closing the archive. The explicit cancel policy is available to callers that prefer a fast
shutdown.

## Location and limits

The Windows default is `%LOCALAPPDATA%\BlackBox\incidents.sqlite3`. `BLACKBOX_ARCHIVE_PATH` overrides it for controlled runs. Other platforms use `$XDG_DATA_HOME/blackbox/incidents.sqlite3`, then `$HOME/.local/share/blackbox/incidents.sqlite3`, with a local fallback only when none of those environment locations exists.

The default logical database limit is 1 GiB, implemented with SQLite `max_page_count`. A configured limit never destructively shrinks an existing archive: when an existing database already exceeds it, its current page count becomes the effective floor. WAL and shared-memory sidecars are transient and can temporarily make physical files exceed the logical limit. BlackBox never silently deletes incidents; reaching the cap fails and rolls back the new incident. V0.10 adds only explicit offline retention and privacy-purge commands, described in `USER_GUIDE.md` and `RELEASE_READINESS.md`.

## Durability and concurrency

Connections enable foreign keys, `synchronous=FULL`, WAL journaling, a 1,000-page automatic checkpoint, and a 250 ms default busy timeout. Every incident uses prepared statements inside one `BEGIN IMMEDIATE` transaction. Its incident header, system samples, extended forensic gauges, normalized system events, process identities/metadata, and process samples therefore commit together or roll back together.

WAL lets readers coexist with the single writer, while `FULL` prioritizes committed-incident durability over write latency. The archive object serializes its connection, and the application runs all viewer repository operations outside the render loop.

Offline evidence inspection and diagnostic evaluation select the explicit `read_only` open mode.
That mode requires an existing non-link regular file, opens SQLite with `SQLITE_OPEN_READONLY`, sets
connection-level `query_only`, validates the exact canonical schema-V1 layout, and skips directory creation, schema
initialization, journal changes, secure-delete/durability changes, and size-limit changes. Read APIs
remain available; every write API is rejected by SQLite. The normal desktop and maintenance paths
use the default `read_write` mode. Read-only mode is evidence protection, not a compatibility reader:
non-v1 archives remain rejected unchanged.

## Pre-release schema version 1

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

## Pre-release reset and recovery

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
