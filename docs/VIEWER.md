# Incident viewer

## Thread and memory boundary

The render loop never calls SQLite. `IncidentViewerService`, owned by the application composition root, executes repository list, load, search, and annotation-update jobs on its own `std::jthread`. It publishes immutable `shared_ptr<const IncidentViewerContent>` snapshots; the UI reads one pointer and emits commands. A slow query can delay another viewer/writer database operation on the serialized archive connection, but it cannot wait or stop the collector.

Archive discovery is paged at 50 incidents in the application and hard-capped at 100 rows per repository call. Search runs across labels and notes in SQLite instead of loading the archive into UI memory. Only one selected incident is loaded at a time. Incident data remains bounded by the recorder's existing 86,400 system-sample, 600,000 process-sample, and 8,192-identity limits. The process table displays at most 500 filtered identities at once.

## Time and ranges

Creation timestamps remain signed Unix epoch milliseconds in SQLite and are displayed explicitly in UTC as `YYYY-MM-DD HH:MM:SS.mmm UTC`. No implicit local-time conversion is applied. Telemetry observation times remain monotonic nanoseconds and are plotted as seconds relative to the incident marker, which is always zero.

The detail header shows requested and actual start/end ranges separately. Negative plot times are pre-incident, positive times are post-incident, and a vertical event-marker series is drawn at zero. Short uptime therefore remains visible as a shorter actual pre-window instead of appearing to contain fabricated history.

## Timeline policy

Every system and selected-process metric carries its availability counts into the view model. Available points are plotted; an empty series reports unsupported, inaccessible, and temporarily-unavailable counts rather than drawing zero values.

Series with at most 2,048 available points are retained exactly. Larger series use chronological min/max buckets while preserving first and last points. Emitting both extrema from each bucket retains a narrow spike or dip that ordinary averaging could erase. Each metric is decimated independently because its missing-value pattern can differ.

The process table aggregates by full PID/creation-token identity and exposes sample count, peak CPU, peak working set, and peak disk read/write. Name/path/PID filtering is case-insensitive. Sorting supports name, PID, and each peak metric in both directions. Selecting a row rebuilds only that process's four bounded graph series from the already-loaded incident on the viewer worker.

## Labels and notes

The pre-release schema-v1 baseline includes a label and note for every incident. Labels are limited to 128 UTF-8 bytes and notes to 4,096 UTF-8 bytes at the repository boundary; schema checks also cap stored character counts. Updates use prepared statements in a transaction and survive restart. V0.0.9 treats both fields as local diagnostic data and provides no export or synchronization.

## V0.2 anomaly ranking

After an incident load, the same viewer worker invokes the optional statistical analyzer before
publishing detail. The render loop receives only primitive rows: candidate name, bounded score,
confidence text, and strongest evidence text. Four resource rows and at most 20 process rows are
visible. Selecting a process preserves the prior analysis result rather than recomputing it.

Analysis time is reported separately from SQLite query and view-model build time. Disabled,
cold-start, ready, and error states are distinct. The viewer explicitly says that unusual behavior
is not proven causation. See `ANALYSIS.md` for formulas and candidate bounds.

## V0.3 personalized profiles

The viewer worker now derives normalized executable keys, loads their bounded prior observations,
passes portable history into analysis, and stores one idempotent observation per executable after
scoring. The render loop still receives only primitive rows. Evidence labels its incident-local or
personal-executable scope; profile cold start and observation count are explicit.

Profile database errors do not fail incident load: the worker falls back to incident-local
analysis and reports that history/update was unavailable. Profile work shares the serialized
archive connection with ordinary viewer/writer jobs, but has no path to the collector. Selecting a
process timeline reuses the already-published personalized result.

## V0.4 automatic-capture feedback

An automatic incident detail displays the retained trigger resource, observation, rolling baseline,
and detector score. Unanswered incidents ask whether the user noticed a problem; yes/no updates run
on the existing viewer worker and persist as schema-v1 incident feedback. Manual incidents do not
show the prompt. Saving a label or note preserves the current feedback value, and selecting a
process timeline reuses it from the loaded annotation state.

## V0.5 incident classification

Every manual or automatic incident exposes one primitive category selector: Unknown, System
freeze, Game stutter, Application slowdown/hang, Network, or Audio. The viewer worker saves the
selected category together with noticed/not-noticed feedback while preserving the independent
free-form label and note. Category/feedback changes append transactional schema-v1 history;
label/note-only edits do not. Selecting a process timeline preserves the loaded classification.

Classification is post-capture storage/UI state. It is not part of `core::IncidentSnapshot`, does
not enter telemetry or analysis, and cannot influence collection, detector decisions, or recorded
samples. See `CLASSIFICATION_DATASET.md` for the offline exchange boundary.

## V0.16 explicit contributor attribution

Each ranked contributor exposes an `Unsure` / `Confirmed contributor` / `Not a contributor`
selector. The viewer worker stores one replaceable direct-V1 row per incident, normalized
executable key, and resource, then reloads the immutable incident. This attribution is not the
incident-level noticed-problem answer and does not change the current rank. For future exact
matches, portable analysis may apply a bounded consensus multiplier while retaining the raw score.
The row exposes cold/conflicting/active profile state and counts. It separately labels preceding,
marker-spanning/ambiguous, and wholly post-marker possible-victim/reaction roles. Only a confirmation
recorded on genuinely preceding activity is eligible for positive future learning; neither of the
other roles can be positively promoted. Storage and analysis work remain off the render thread.
