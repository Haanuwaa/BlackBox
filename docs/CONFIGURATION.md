# Recorder configuration

V0.14 exposes three validated recorder profiles in the Settings view. Applying a profile
restarts only the collector, cancels any incomplete post-window, resets rate baselines, and clears
the rolling RAM epoch; it does not stop the viewer/writer or delete saved incidents. Settings use a
versioned bounded text file at `%LOCALAPPDATA%\BlackBox\settings.ini` (override with
`BLACKBOX_SETTINGS_PATH`). Saving happens only after an explicit settings action, never during
normal telemetry recording. Missing settings select the conservative default; malformed, unknown,
oversized, or out-of-bound input is reported and falls back safely.

Product controls use a separate versioned, bounded file at
`%LOCALAPPDATA%\BlackBox\product-settings.ini` (override with
`BLACKBOX_PRODUCT_SETTINGS_PATH`). The UI validates an F1-F12 hotkey with at least one modifier,
capture windows, detector enablement/sensitivity/resource selection, a 0-24 hour cooldown,
notifications, an absolute archive path, a 64 MiB-64 GiB archive capacity, and executable-path
collection. Hotkey, detector, notification, capture-window, and privacy changes apply immediately;
archive path/capacity changes are clearly marked for the next launch. A conflicting hotkey is
rejected and the previous registration is restored.

| Profile | Sample interval | Rolling history | System frames |
|---|---:|---:|---:|
| Conservative (default) | 1 s | 5 min | 300 |
| Balanced | 500 ms | 5 min | 600 |
| Detailed | 250 ms | 2 min | 480 |

| Setting | V0.14 default | Validation / effect |
|---|---:|---|
| Sample interval | 1 s | Positive; scheduler never catch-up polls |
| Rolling history | 5 min | Positive; 300 default frames |
| Late tolerance | 50 ms | Nonnegative; increments late-start diagnostics |
| Process metadata interval | 30 s | Positive; avoids repeated path resolution |
| Incident pre-window | 120 s | Nonnegative |
| Incident post-window | 30 s | Nonnegative |
| Resume-gap threshold | 5 s | Positive; resets rate baselines and cadence |
| Incident work capacity | 2 | Fixed bound, including queued/in-progress work |
| Archive maximum | 1 GiB | SQLite logical page cap |
| SQLite busy timeout | 250 ms | Writer fails one attempt, then can recover on later work |
| Automatic confirmation | 3 samples | Consecutive severe observations before capture |
| Disk stall / queue | 100 ms / 8 requests | One-observation physical-layer quality capture |
| TCP retransmission | 25% with at least 8 segments | One-observation passive transport capture |
| TCP failure / reset | 2 interval events | One-observation passive transport capture |
| Automatic cooldown | 120 s | Global deduplication after an emitted trigger |
| Executable paths | On | Can be disabled for future samples; apply clears the RAM metadata cache |

History capacity is `ceil(history / interval)` and is capped at 86,400 system frames. Process
history is independently capped at 600,000 rows per configuration epoch and metadata at 8,192
identities. Invalid configurations are rejected before collector construction.

The 500 ms and 250 ms profiles retain the same 600,000-row process-history and 86,400-system-frame
hard caps. Conservative remains the release qualification profile; faster profiles are explicit
user choices and diagnostics expose their timing, jitter, misses, and drops.

## V0.14 evidence privacy controls

Fresh product settings keep all new privacy-sensitive sources off. Applying Settings updates the
foreground sampling request and restarts only the independent event provider when its gates change:

| Control | Evidence enabled |
|---|---|
| Record foreground application | Durable foreground `(PID, creation token)` and matching GPU engine usage where process identity exists; an opaque session application key without PID/GPU correlation on advertising wlroots compositors; never title/content/raw `app_id` |
| Record process start and exit identity | Durable `(PID, creation token)` lifecycle context from the existing process enumeration; initial inventory and recovery-gap observations are suppressed |
| Record power and device events | Suspend/resume and device enumerate/start/remove notifications |
| Record audio device events | Endpoint add/remove/state/default transitions without endpoint ID |
| Record selected Windows events | Service Control Manager, Defender, Windows Update, Application Hang, DNS Client timeout, Display timeout recovery, and `disk` I/O retry normalized IDs/kinds without messages, queried hostnames, driver names, storage LBAs/device paths/PDO identities, or payloads |

GPU aggregate/memory, DPC/ISR, CPU frequency/thermal limit, battery/power status, and uptime remain
ordinary capability-gated system gauges. Process lifecycle is context only: it cannot request
automatic capture or prove causation. The local archive retains durable identity, while dataset and
truth-review exports omit foreground identities and never export PID, creation token, or opaque
application token. The event
ring defaults to 4,096 records and is hard-capped
at 65,536; there is no unbounded event configuration. Previously saved incidents are immutable and
require explicit retention/purge to remove.

Both recorder and product settings use their current complete pre-release format version 1. Older
pre-release layouts are intentionally unsupported; the application has no compatibility readers.

## Background shell settings

The tray menu owns two user controls that are independent of recorder sampling:

- **Start with Windows** persists as the current-user
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` value `BlackBox`. Its quoted command is the
  current executable plus `--background`; it is never a service, scheduled task, or
  machine-wide/elevated setting.
- **Notifications** enables or quiets session-local, no-sound capture lifecycle notifications.
  Quiet mode does not change capture, persistence, or diagnostics. The value is durable and
  synchronized with the Settings page.

The internal `--background-diagnostic-seconds=N` switch (1-3600 seconds) starts hidden and performs
a clean automatic Exit after the requested interval. It exists for controlled performance and
lifecycle qualification; it does not alter sampling or archive behavior.

## V0.2 analysis profile

Analysis is optional at build time (`BLACKBOX_ENABLE_ANALYSIS`, default `ON`) and never changes the
recorder profile. The validated statistical defaults are a 60-second baseline ending 30 seconds
before the event, a 256-value rolling baseline capacity, eight required baseline samples, 512
preselected process identities, and 100 returned process rankings. The viewer displays 20. Invalid
zero/unbounded relationships are rejected at analyzer construction. See `ANALYSIS.md` for scoring.

## V0.3 personalization profile

Personalization keeps observations from the preceding 30 days, at most 64 per normalized
executable identity, and requires eight observations before replacing an incident-local metric
score. SQLite retains at most 2,048 executable identities and accepts at most 512 keys per incident
query/update. Reopening an incident is idempotent. These settings affect only post-capture viewer
analysis and never the recorder cadence, history, or normal-telemetry write policy. See
`PERSONALIZATION.md` for the identity and aging contract.

## V0.4 automatic detection profile

Automatic detection is optional at build time (`BLACKBOX_ENABLE_AUTOMATIC_DETECTION`, default
`ON`). When disabled, the implementation and detector tests/benchmark are absent and the collector
receives a null detector. Manual capture is unchanged.

The default detector keeps four fixed 30-sample rolling baselines inside 60-value-capacity arrays.
It confirms an event after three consecutive qualifying observations. Hard thresholds are 98% CPU,
97% physical memory, 1 GiB/s aggregate disk read+write, and 512 MiB/s aggregate network
receive+transmit. A statistical trigger requires the current value to exceed both a resource floor
(85% CPU, 90% memory, 128 MiB/s disk, or 64 MiB/s network) and eight baseline standard deviations.
The standard-deviation floors are 2 CPU percentage points, 1 memory percentage point, 8 MiB/s disk,
and 4 MiB/s network. Missing values break confirmation and remain missing rather than becoming zero.

After a trigger, the global two-minute cooldown suppresses all further resources. Resume gaps,
provider loss, and recorder reconfiguration reset live baselines. V0.13 exposes
conservative/balanced/sensitive profiles, per-resource enablement, a master switch, and cooldown
through validated application-owned commands. Disabled metrics remain missing rather than becoming
zero.

The quality-event thresholds are independent of the throughput baseline and can trigger from one
observation. Conservative uses 200 ms disk service, queue 16, 50% retransmission, and four TCP
events; Sensitive uses 50 ms, queue 4, 10%, and one event. Connectivity disconnected always
qualifies; constrained connectivity requires an observed interface transition. All still share the
global cooldown and respect disk/network enablement.

## Archive maintenance and recovery

Normal recording never invokes retention or deletion. The Settings page publishes current archive
path, schema, incident count, logical size, and configured capacity. User-initiated jobs run on a
dedicated application maintenance worker serialized with archive access:

- **Retry failed incident** retries the single bounded recovery slot retained after terminal writer
  failure. The same immutable snapshot can first be exported as a standalone SQLite archive.
- **Backup** uses SQLite online backup and refuses an existing destination.
- **Restore** verifies SQLite integrity, BlackBox application identity, and the current schema before
  replacement; it first creates a new pre-restore safety backup.
- **Retention** keeps a confirmed positive count of newest incidents, cascades owned rows, removes
  orphaned profiles, checkpoints, and compacts only after explicit confirmation.
- **Dataset export** produces the inspectable offline evidence format without changing telemetry.
- **Privacy purge** requires explicit confirmation and removes incident/profile evidence. No
  automatic age, capacity, or background deletion policy exists.
