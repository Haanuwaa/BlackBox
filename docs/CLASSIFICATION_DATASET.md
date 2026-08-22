# Incident classification and offline dataset

## Categories and history

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

## Pre-release dataset version 1

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

## Privacy boundary

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
