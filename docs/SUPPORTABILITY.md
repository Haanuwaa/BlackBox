# Supportability and crash evidence

BlackBox provides a local, bounded support path without adding an uploader, network client, or
dependency to the recorder. Support tooling is downstream of collection and cannot be called by a
telemetry provider or the rolling recorder.

## Crash diagnostics

On Windows, the application creates the local crash directory before normal initialization and
pre-opens one uniquely named `.dmp.partial` file for the current process. The top-level exception
filter copies only the exception pointer and crashing thread ID into preallocated state, signals a
dedicated dump thread, and waits at most 15 seconds. The dedicated thread writes `MiniDumpNormal`
evidence through the already-open handle, flushes it, and renames it to `.dmp`; this avoids asking the
damaged thread and stack to run DbgHelp. A failed dump write resets the same bounded staging file and
retries at most five times with 100-millisecond delays. Neither path allocates nor opens another file
after the crash. Final publication tolerates only bounded transient access/sharing/lock contention
for at most 4.75 seconds. Permanent write or publication failure leaves the `.partial` artifact
visibly incomplete rather than presenting it as completed evidence. A clean shutdown stops the
worker, closes the handle, and removes the unused partial file. Existing completed dumps are never
deleted automatically.

The handler is deliberately small: it does not open SQLite, allocate an incident, render UI, acquire
application locks, upload data, or attempt recovery. Windows implementation details remain under
`platform/windows`; the application sees only `ICrashDiagnostics` and a bounded snapshot containing
availability, armed state, completed-dump count, and the newest local dump.

The default directory is `crashes` beside `product-settings.ini` (normally
`%LOCALAPPDATA%\BlackBox\crashes`). A completed minidump can contain thread stacks, module names,
module paths, and fragments of process memory. Treat every `.dmp` as potentially personal data.

On Linux and macOS, the same composition boundary pre-opens one mode-0600 `.crash.partial` record and
installs bounded handlers for fatal POSIX signals. The handler appends exactly one canonical 40-byte
event to its prewritten direct-v1 header using async-signal-safe calls, flushes, closes, and renames the
64-byte record. It stores only signal/code, process ID, UTC seconds/nanoseconds, and a fault address;
it does not allocate, walk a stack, resolve a module, call SQLite/UI, or open another file. The signal
is then re-raised with its default disposition so native crash reporting remains possible. A real
child-process probe validates publication on both hosted platforms. This small signal record is useful
crash evidence, not a Windows minidump or a substitute for physical crash-reporter qualification.

## Local support bundle

The Diagnostics page creates a new directory selected by the user. Bundle creation runs on a
dedicated one-request worker so filesystem work does not block collection or rendering. The direct
format-v1 bundle is assembled in a sibling `<destination>.partial` directory, every expected regular
file is checked as nonempty and bounded, and the directory is atomically renamed into place. An
existing final or partial destination is refused; neither is overwritten or silently deleted.

A default bundle contains exactly:

- `manifest.ini`: format, creation time, exact file count, inclusion flag, and content fingerprints;
- `diagnostics.ini`: bounded recorder, capture, event, writer, archive-health, and privacy-switch
  counters; and
- `README.txt`: the sharing/privacy contract.

It excludes incident rows, telemetry samples, process lists, executable names or paths,
annotations, feedback, settings values, the hotkey, usernames, archive/configuration locations, and
all other absolute paths. BlackBox has no automatic upload path.

The newest raw crash-evidence file is included only when all of these are true:

1. completed crash evidence exists;
2. the user enables **Include latest raw crash evidence**;
3. the adjacent raw-evidence consent is checked; and
4. the source is an absolute, non-link, nonempty regular file no larger than 64 MiB.

The source filename and source path are not written into the bundle. The copied file is always named
`crash-evidence.bin`. The manifest uses the neutral direct-v1
`includes_crash_evidence` and `crash_evidence_fingerprint` fields. Consent creates a local copy only;
sharing remains a separate user action.

## Failure and retention semantics

- Failed support publication leaves only the visibly suffixed `.partial` directory for inspection.
  It cannot masquerade as a completed bundle.
- Completed bundles and completed crash evidence have no automatic retention or upload policy.
- A crash during support-bundle creation does not affect the archive or collector. The partial bundle
  can be removed explicitly after inspection.
- Support bundles use one direct v1 contract. There is no pre-release migration or legacy reader.
- Crash evidence is diagnostic context, not incident truth and not proof of causation.

Operational recovery steps are in [RECOVERY_RUNBOOKS.md](RECOVERY_RUNBOOKS.md), and the security and
privacy boundaries are in [PRIVACY_THREAT_MODEL.md](PRIVACY_THREAT_MODEL.md).
