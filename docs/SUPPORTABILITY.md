# Supportability and crash evidence

BlackBox provides a local, bounded support path without adding an uploader, network client, or
dependency to the recorder. Support tooling is downstream of collection and cannot be called by a
telemetry provider or the rolling recorder.

## Crash diagnostics

On Windows, the application creates the local crash directory before normal initialization and
pre-opens one uniquely named `.dmp.partial` file for the current process. The top-level exception
filter writes `MiniDumpNormal` evidence through that already-open handle, flushes it, and renames it
to `.dmp`. A clean shutdown closes and removes the unused partial file. Existing completed dumps
are never deleted automatically.

The handler is deliberately small: it does not open SQLite, allocate an incident, render UI, acquire
application locks, upload data, or attempt recovery. Windows implementation details remain under
`platform/windows`; the application sees only `ICrashDiagnostics` and a bounded snapshot containing
availability, armed state, completed-dump count, and the newest local dump.

The default directory is `crashes` beside `product-settings.ini` (normally
`%LOCALAPPDATA%\BlackBox\crashes`). A completed minidump can contain thread stacks, module names,
module paths, and fragments of process memory. Treat every `.dmp` as potentially personal data.

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

The newest `crash.dmp` is included only when all of these are true:

1. completed crash evidence exists;
2. the user enables **Include latest crash dump**;
3. the adjacent raw-dump consent is checked; and
4. the source is an absolute, non-link, nonempty regular file no larger than 64 MiB.

The source filename and source path are not written into the bundle. The copied file is always named
`crash.dmp`. Consent creates a local copy only; sharing remains a separate user action.

## Failure and retention semantics

- Failed support publication leaves only the visibly suffixed `.partial` directory for inspection.
  It cannot masquerade as a completed bundle.
- Completed bundles and completed crash dumps have no automatic retention or upload policy.
- A crash during support-bundle creation does not affect the archive or collector. The partial bundle
  can be removed explicitly after inspection.
- Support bundles use one direct v1 contract. There is no pre-release migration or legacy reader.
- Crash evidence is diagnostic context, not incident truth and not proof of causation.

Operational recovery steps are in [RECOVERY_RUNBOOKS.md](RECOVERY_RUNBOOKS.md), and the security and
privacy boundaries are in [PRIVACY_THREAT_MODEL.md](PRIVACY_THREAT_MODEL.md).
