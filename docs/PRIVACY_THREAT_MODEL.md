# Privacy threat model

## Scope and security objective

BlackBox is a local-first recorder for an ordinary interactive Windows user. Its privacy objective is
to collect only the evidence enabled by that user, keep rolling history in bounded RAM, persist only
accepted incidents and explicit maintenance results, and never transmit evidence automatically.
This document covers the desktop application, SQLite archive, settings, crash evidence, exports,
support bundles, and offline evaluation tools. It does not claim protection from an administrator,
kernel-mode malware, physical compromise, or a hostile process already able to read another
process's memory or the user's files.

## Sensitive assets

- Incident system/process samples and timestamps.
- Process identities, executable names, and optional executable paths.
- Foreground process identity or a session-scoped opaque Wayland application key, durable process
  start/exit identity, and selected normalized system-event evidence when enabled; raw Wayland
  `app_id`, title, and compositor handle are discarded before the portable boundary;
  queried DNS hostnames, display driver names, adapter identities, storage LBAs/device paths/PDO
  identities, Event Log messages, and payloads are never collected.
- User labels, notes, feedback, recurrence overrides, and personalized observations.
- Archive/configuration paths and local account path components.
- Raw crash evidence: Windows minidump stack memory/module paths/incidental strings, or a POSIX
  signal record's code address.
- Dogfood ballots, session/operator pseudonyms, and hardware-profile descriptions.

Aggregate counters such as dropped samples or archive byte totals are lower sensitivity, but may
still reveal product usage and are treated as local data.

## Trust boundaries and threats

| Boundary | Principal threats | Controls |
|---|---|---|
| OS providers to collector | malformed counters, access denial, device churn, unsupported signals | capability/status values, delta validation, bounded containers, ordinary-user operation |
| RAM recorder to immutable incident | race, unbounded copying, accidental capture outside window | fixed rings, short locks, bounded snapshot windows, immutable value handoff |
| Incident writer to SQLite | partial commit, lock/full/corrupt archive, silent loss | transaction-per-incident, FULL durability, bounded retry, one visible recovery slot, no silent replacement |
| UI/maintenance to filesystem | path mistakes, overwrite, destructive action, render blocking | new-destination rules, confirmations, maintenance workers, validated restore safety copy |
| Crash handler to evidence directory | deadlock/re-entry, incomplete evidence, private memory/address disclosure | pre-opened unique file, single guarded handler, minimal platform record, `.partial` publication, no upload |
| Support bundle to recipient | accidental disclosure of incidents, paths, raw memory, or addresses | fixed allowlisted diagnostics schema, no archive/settings/log inclusion, optional raw evidence requires explicit consent |
| Offline corpus/evaluation | unconsented collection, label leakage, held-out reuse, operator bias, accidental runtime coupling | mandatory direct-v1 session consent attestation, offline-only target, independent ballots, fingerprints, exclusive one-shot attempt |

## Data minimization and user control

Normal rolling telemetry is RAM-only. Process-path collection, foreground identity, process
lifecycle identity, and event families have explicit settings; unavailable evidence stays
unavailable instead of being inferred.
Network evidence is passive and aggregate: BlackBox records no payload, packet content, endpoint,
DNS name, or browsing destination. Incident retention and purge require explicit user action;
automatic deletion remains prohibited.

The opaque Wayland application token is keyed by fresh in-memory randomness for each BlackBox run.
It supports transitions within that run and any locally captured incident, but cannot be joined across
runs or to a native PID. Offline dataset and truth-review exports omit it.

The privacy-safe support bundle is an allowlist, not a redaction pass. Free-form status/error strings,
logs, process rows, archive paths, and settings are never admitted to `diagnostics.ini`, avoiding the
fragility of trying to scrub arbitrary text. Raw platform crash evidence is the only sensitive optional
artifact and has a separate confirmation; the fixed POSIX record contains no stack or module path, but
its fault address is still treated as potentially sensitive.

Dogfood truth review is also allowlist-based. Its default ordinal-only directory contains normalized
incident values and stable process ordinals but blank PID/name fields; it excludes executable paths,
creation tokens, labels, notes, feedback, predictions, and analyzer output. The command requires an
explicit privacy-mode argument. `include-local-identities` admits only the recorded bounded PID and
process name for local disambiguation, making that directory sensitive working state that must not
be copied into the five-file corpus or shared by default. Both modes are local-only, read an existing
archive without mutation, refuse overwrite, and publish through bounded sibling staging plus rename.
Every dogfood session row also requires `consent_attested=1` before it can validate, merge, freeze,
or evaluate. The field records the collection operator's explicit confirmation that the participant
agreed to local collection and privacy-reduced campaign use; it is not represented as cryptographic
or legal proof.

## Integrity and hostile-input posture

Archives, settings, datasets, corpora, evaluation artifacts, and support bundles use strict direct-v1
formats with bounded sizes and known fields. Because the application is unreleased, non-v1 formats
are rejected unchanged; there are no migrations, downgrade readers, or legacy compatibility paths.
Publication uses transactions or sibling staging plus rename. Existing destinations are not
overwritten. Offline inputs and restored archives remain untrusted until their schema, identity,
bounds, and integrity checks pass.

## Residual risks

- `MiniDumpNormal` can contain personal data even though it is smaller than a full-memory dump; POSIX
  signal evidence can expose a code address even though it contains no memory or path.
- SQLite and dump files rely on the user's filesystem permissions and are not encrypted by BlackBox.
  Device encryption and account security are outside the application.
- Process names, optional paths, lifecycle timing/identity, timestamps, notes, and event evidence may expose activity to anyone
  who can read the archive.
- Secure deletion on SSDs, snapshots, backups, and synchronized folders cannot be guaranteed by an
  application-level purge.
- A user can intentionally export or share sensitive evidence. BlackBox can warn and minimize but
  cannot control a file after it leaves the machine.
- The current package is unsigned and not yet clean-client qualified; it must not be treated as the
  production trust boundary until the V0.17 release gates pass.

## Release review checklist

- Confirm no new network/uploader dependency or automatic transmission path exists.
- Confirm every new evidence family has a capability state, setting/privacy decision, bounds, and
  dataset behavior.
- Confirm support-bundle fields remain a fixed allowlist and tests search for source paths.
- Confirm raw crash inclusion still requires adjacent explicit consent and is size/link bounded.
- Confirm archive deletion remains explicit and recovery never silently replaces corrupt evidence.
- Re-run dependency/security analysis, fuzz/property tests, clean-client qualification, and signing
  checks on the exact release revision.
