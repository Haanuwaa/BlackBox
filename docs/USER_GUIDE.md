# Using BlackBox

[Documentation index](README.md) · [Current candidate](CURRENT_CANDIDATE.md)

- [BlackBox user guide](#blackbox-user-guide)
- [Supportability and crash evidence](#supportability-and-crash-evidence)
- [Recovery runbooks](#recovery-runbooks)

## BlackBox user guide

BlackBox records a bounded telemetry history in RAM. It writes an incident to SQLite
only after a manual or automatic capture; ordinary recording does not stream telemetry to disk.

On **Explain**, expand **Export readable incident summary**, choose a new text file and export.
The summary includes requested/recorded coverage, observed metric peaks, missing-value counts,
confidence and uncertainty. Paths, process identities, raw events and raw samples are omitted;
saved labels/notes are included only when selected. Review exported text before sharing it.
Writes run in the viewer worker and refuse existing destinations.

Settings offers native file pickers and a visible unsaved-changes message. Incident drafts survive
page navigation and background refresh. Save or discard them before opening another incident.

Windows is the only production target in this pre-release. Linux DEB/RPM/TGZ and macOS TGZ outputs
are explicitly unsupported engineering previews used to exercise platform boundaries. Linux can
collect native telemetry, publish bounded notifications through the XDG portal with a freedesktop
service fallback, publish coalesced background status, and request its configured global shortcut
through the XDG Desktop Portal. Shortcut sessions recover after a portal restart, while a shortcut
removed by the desktop remains visibly unavailable until reapplied. Desktop policy may require user
approval or leave any service unavailable. Generic Wayland foreground process identity remains
unsupported because no standardized permission-bounded portal exposes an active-window PID. On
wlroots compositors that advertise the optional protocol, BlackBox may instead show a private
session-only application key; it never exposes a title, raw application ID, or inferred PID, and it
cannot correlate that key with process or GPU evidence. Both
previews supply native CPU/memory/disk/network/power/uptime/process
telemetry and passive local-link/TCP-quality evidence, but no qualified product experience. macOS's
unsigned engineering `.app` can expose menu-bar controls, request launch at
login through macOS ServiceManagement, deliver permission-gated local notifications, and follow the
system's increased-contrast/reduced-motion preferences. Its passive Input Monitoring-gated global
shortcut is implemented; signing/notarization and physical-client qualification remain open.

### Start and capture

1. Start `blackbox.exe` as an ordinary desktop user. Administrator rights are not required.
   First-run onboarding follows three steps: keep a short bounded history in RAM, capture after a
   slowdown, then review the saved local evidence. It also makes clear that correlation is not proof.
2. Leave BlackBox running while reproducing or waiting for a short performance problem.
3. At the problem, press the configured global hotkey (**Ctrl+Shift+F12** by default), press
   **Ctrl+Enter** while the window is open, or select **Capture what just happened** in Live.
4. Keep BlackBox open during the 30-second post-window. Live shows capture and writer state.
5. Open Incidents, select the saved item, and inspect system graphs and process peaks. Labels and
   notes are editable and survive restart. Choose Unknown, System freeze, Game stutter,
   Application slowdown/hang, Network, or Audio and save to classify the incident.

### Background and tray controls

BlackBox keeps recording when its main window is minimized or closed. Closing the window hides it
to the notification area only when the tray icon is available; if Windows cannot create the tray
icon, closing exits so the application can never become an unreachable background process.

Left-click the tray icon to show the window. Right-click it to show or hide BlackBox, capture an
incident, pause/resume recording, enable or disable **Start with Windows**, quiet/unquiet capture
notifications, or **Exit BlackBox**. Exit is the deliberate full-shutdown path: it stops new
captures, joins the collector, drains accepted incident writes, closes the archive, and removes the
tray icon. Pausing preserves the bounded RAM history but records no new telemetry; resuming resets
cumulative rate baselines so the pause gap cannot become a false spike.

Start with Windows uses the current user's Run setting and requires no administrator rights. It
launches `blackbox.exe --background`, so the recorder starts hidden. Moving or deleting the
executable later requires disabling/re-enabling the setting from its new location. A second launch
does not create another recorder; it asks the existing instance to show its window and exits.

Notifications cover capture start/extension, successful archive completion, and failed/rejected
capture or storage. They are quiet (no sound), contain no captured telemetry or process names, and
can be disabled durably from the tray menu or Settings. The tray tooltip/icon distinguishes
recording, capture, pause, storage retry, and attention-required states. Rapid notification bursts
coalesce to the newest lifecycle state instead of stacking stale or empty balloons.

On Windows, hover over the blue recorder icon to see **BlackBox** and its current state.
If the icon is hidden, open the notification area's overflow arrow. The icon's badge shows
recording (green), capture (blue), pause (amber pause bars), or storage trouble (an amber/red
exclamation); the tooltip supplies the full meaning. Keyboard users can focus the notification
area with **Win+B**, select BlackBox, and press **Shift+F10** for the same menu.

### Product pages

- **Live** leads with recorder readiness, the configured shortcut, one **Capture what just happened**
  action, local saved-incident count, and tray/background guidance. Current processor, memory,
  storage, and network activity remains visible. Recent graphs use seconds from now, allow hover
  inspection, and explain missing values; platform/capture internals and detailed signals remain
  behind named disclosure controls.
- **Incidents** searches, sorts, and pages immutable saved captures. Loading, a genuinely empty
  archive, no search matches, and archive failure are separate states with direct next actions.
- **Explain** leads with symptom, possible contributor, confidence, plain-language evidence, and the
  correlation caveat plus at most three standout contributors before deeper evidence is expanded.
  GPU and responsiveness/power plots, foreground
  transitions, and privacy-normalized Windows events share the incident-relative timeline. Pan or
  wheel-zoom one plot to update every plot. Hover any plot to place one sticky cyan inspection
  cursor at the same marker-relative second across all timelines; clear it separately from the
  orange incident marker. These are correlations/context, not proof of cause.
- **Patterns** shows bounded recurring-evidence groups and explicit noise/unique captures.
- **Settings** owns validated recorder/product controls and guided archive recovery.
- **Diagnostics** shows provider, ring, timing, writer, accessibility, DPI, unavailable-state,
  crash-evidence, and local support-bundle detail.

### Keyboard workflows

Press **F1** from the main window for the complete shortcut guide. Use **Tab** and **Shift+Tab** to
move focus, **Enter** or **Space** to activate the focused control, **Ctrl+1** through **Ctrl+6** to
switch pages, **Ctrl+Enter** to capture what just happened, and **Ctrl+R** to refresh Saved incidents.
Press **Escape** to close the shortcut guide. The configured global shortcut works when the window is
hidden; in-window shortcuts apply only while BlackBox has keyboard focus.

### Automatic capture

BlackBox watches normalized CPU, memory, disk, and network samples for sustained severe events.
The balanced default requires three qualifying observations for utilization/throughput. A severe
physical-disk stall/queue or passive network drop/failure can capture from one observation, and a
two-minute cooldown deduplicates a sustained storm. The default quality gates are 100 ms disk
service time, queue depth 8, disconnected connectivity, a constrained interface transition, a 25%
TCP retransmission fraction over at least eight segments, or two failed/reset TCP connections.
Settings can disable detection, select conservative/balanced/sensitive behavior,
enable individual resources, and change cooldown. Automatic and manual requests share the default
120-second pre-window, 30-second post-window,
overlap merge policy, two-slot FIFO, and 1 GiB logical archive cap.

When selected Windows Event Log evidence and automatic detection are both enabled, future-only
Application Error event 1000, Application Hang event 1002, and Display timeout-recovery event 4101
can also request capture through
that same bounded coordinator. A display capture says only that Windows reported a timeout recovery;
it does not identify a faulty driver, application, GPU, or other root cause. DNS Client timeout 1014
remains context for a manual or independently triggered incident and never triggers by itself.
Provider `disk` event 153 may likewise request a disk-scoped capture, but says only that Windows
reported retrying a timed-out storage I/O request. It does not identify overload, a device, cable,
controller, driver, media, firmware, application, hardware failure, or another root cause.

An automatic incident explains which resource triggered it and shows the observed value, baseline,
and detector score. When you select it, answer whether you noticed a problem. That yes/no answer is
stored with the incident and survives restart. It does not replace the free-form label/note and is
not treated as proof that the detected resource caused the symptom.

After at least four prior answered automatic incidents with the exact same resource/signal trigger,
repeated **Did not notice a problem** answers can conservatively lower only the diagnosis confidence
for a future matching incident. Conflicting **Noticed a problem** answers protect against
suppression. The Detail page shows the matching counts, profile state, and multiplier; raw graphs,
observed pressure, process evidence, contributor ranks, and automatic collection are unchanged.
If the adjusted assertion becomes too weak, BlackBox displays `Unknown` rather than a weakened
claim.

Use **Reset feedback influence** in that Detail section after checking its confirmation when you
want older answers to stop influencing future diagnoses or confirmed-similar history. The reset preserves every incident,
annotation, classification, and sample. **Undo last reset** restores the immediately previous
cutoff; a second rollback is unavailable. Full privacy purge also clears the feedback profile.

### Statistical ranking

Selecting an incident in V0.2 runs analysis on the viewer worker. The ranking compares the event
window with an earlier incident-local baseline and shows a 0-100% anomaly score, confidence, and
the strongest median/P95/robust-z evidence for each resource and process candidate. “High” means
the baseline has good coverage; it does not mean BlackBox proved causation.

“Cold start” means too little usable pre-incident history exists. Unavailable protected-process
metrics remain missing and never become zeros. A normal result with zero scores means no evaluated
value exceeded the conservative 3.5 robust-z threshold; it is not a guarantee that nothing caused
the symptom. See `ANALYSIS.md` for exact windows, formulas, bounds, and limitations.

In V0.3, process rows say whether their evidence is incident-local, a personalized executable
baseline, or a profile cold start. Opening an incident learns one bounded observation for each
analyzed executable; reopening it does not count it again. Eight earlier observations within 30
days are needed before personalization replaces a metric score. A rename/path move starts a new
profile, an in-place upgrade shares the path profile, and an inaccessible path falls back to a
separate name-based profile. See `ANALYSIS.md` before interpreting these identities as
cryptographic binary identity—they are not hashes or signatures.

V0.6 adds a **Potential contributors** table. “Likely contributor (correlation only)” means the
recorded process activity was large, preceded the marker, matched a system resource anomaly, had
adequate duration, and had adequate evidence coverage. “Potential contributor” is weaker or
incomplete preceding correlation. **Ambiguous correlate across marker** began before the marker but
has more anomalous samples after it. **Possible victim/reaction** began only after the marker and is
not a causal rank. Expand the evidence rather than treating any label as proof: the row exposes all
five score factors, pre/post sample counts, coverage, missing metrics, onset, and duration. See
`ANALYSIS.md` for
the formula and limitations.

If opt-in lifecycle evidence exists, contributor timing distinguishes **anomalous activity began**
from **recorded process start/exit**. Those lifecycle timestamps require the full durable identity
and consistent ordering; they are context only and never change the rank. No lifecycle line means
the event was not recorded—it does not prove the process was already running.

Each contributor row also asks for an explicit attribution: **Unsure**, **Confirmed contributor**,
or **Not a contributor**. This is a causal judgment and is deliberately separate from whether you
noticed a problem. It never changes the incident being viewed. Four consistent prior answers for
the exact normalized executable and resource are required before a future rank can move; the row
shows the original score, bounded multiplier, adjusted score, and confirmed/rejected counts.
Confirmed history cannot promote marker-spanning or post-marker activity. A confirmation also
cannot teach positive uplift unless that source row genuinely preceded its own marker; its temporal
role is retained with the direct-V1 vote. Use **Unsure** to remove the stored attribution.
Reset/undo controls apply to this history as well without deleting it.

### Recurring incidents

The **Recurring incident discovery** section groups similar system-resource shapes across captures.
Select **Refresh recurring groups** to scan the newest bounded archive window. Each group shows its
occurrence count, shared feature evidence, maximum member distance, and inspectable members.
Unmatched captures stay under **Noise / unique incidents** rather than being forced into a group.

After refreshing patterns, an automatically grouped incident may show **Confirmed similar
incidents** in Detail. A ready summary means at least two bounded prior confirmations agree strongly
on a symptom category. It is historical context only and does not alter the current diagnosis,
confidence, contributor ranking, or evidence. Sparse/disputed feedback is shown but not reused;
user-created groups are always excluded from learning.

To correct a misleading grouping, select an incident and enter a **Recurring group override**.
Incidents with exactly matching non-empty text appear in the same clearly marked user group even if
their telemetry differs. **Return to automatic grouping** clears the decision. Overrides organize
your archive; they are not diagnostic proof. See `ANALYSIS.md` for feature scaling,
thresholds, bounds, and limitations.

Overlapping triggers merge into the active capture. At most two immutable incidents can be queued;
the UI reports a full queue instead of allowing unbounded memory use. Use **Exit BlackBox** from the
tray for a full shutdown; ordinary window close keeps the recorder active when the tray is usable.

### Statuses and known gaps

- **Warming up** is expected for cumulative rates on their first observation and after resume or a
  provider failure. The next valid delta restores the rate.
- **Inaccessible** means Windows denied an ordinary-user query, commonly for protected/security or
  system processes. BlackBox does not elevate or retry-storm; the process is counted in diagnostics.
- **Unsupported** means the platform/API cannot provide that signal.
- **Temporarily unavailable** means a transient query, counter, or storage operation failed. The
  collector continues, and recovery counters show successful return to service.
- System CPU on machines with more than 64 logical processors currently covers the calling
  processor group. Network quality is machine-wide passive evidence: BlackBox records no RTT,
  destination, packet, DNS, or payload, and cannot prove an application endpoint failed. Physical
  disk quality is not per-process latency. Foreground GPU is PID-correlated engine evidence, not
  complete per-process GPU accounting. DPC/ISR pressure does not identify a driver or prove an
  audio glitch; Application Error event 1000, Application Hang event 1002, Display timeout recovery
  event 4101, and `disk` I/O
  retry event 153 are passive OS-reported symptoms. Event 4101 does not identify the faulty
  driver/application/GPU; event 153 does not identify the storage fault or root cause. Continuous
  frame/audio ETW is not recorded.

The archive defaults to `%LOCALAPPDATA%\BlackBox\incidents.sqlite3`. Its default logical size cap is
1 GiB. Use **Create verified backup** in Settings for a consistent sidecar-free online backup.

### Recorder and product settings

The Recorder settings view offers Conservative (1 s / 5 min), Balanced (500 ms / 5 min), and
Detailed (250 ms / 2 min) profiles. Applying one restarts collection and clears only the rolling
RAM history. Do not change it during an active post-window: collector restart cancels that
incomplete capture. Saved incidents remain intact. The selected profile persists locally in
`%LOCALAPPDATA%\BlackBox\settings.ini`; an invalid file is rejected and conservative defaults are
used. Faster profiles expose their actual cost through the same diagnostics and may be less
appropriate on a busy or low-power machine.

Settings also validates the F1-F12 hotkey/modifiers, capture windows, detector controls,
notifications, archive path/capacity, whether future samples may record executable paths, and five
V0.14 evidence gates: foreground application, process start/exit identity, power/device events,
audio-device events, and selected
Windows Event Log evidence. The new privacy-sensitive gates default off and never retain window
  titles, Event Log messages, queried hostnames, display driver names, adapter identities, storage
  LBAs/device paths/PDO identities, device IDs, endpoint IDs, or payloads. Selected
Windows evidence includes normalized DNS Client timeout event 1014. A matching incident can say
that Windows reported a DNS resolution timeout near the marker, but BlackBox does not infer its
cause or automatically capture from that event alone.
Selected evidence also includes normalized Application Error event 1000. It may request bounded
automatic capture, but retains no application/module name, exception code, fault path, message, or
payload and can assert only that Windows reported a nearby application crash—not why it crashed.
Selected evidence also includes normalized Display timeout recovery event 4101. It may request
bounded automatic capture, but its driver name/message/payload are never retained and its diagnosis
is limited to the Windows-reported recovery symptom.
Selected evidence also includes normalized provider `disk` retry event 153. It may request bounded
disk-scoped automatic capture, but retains no LBA/device/PDO/message/payload and can assert only the
Windows-reported I/O-retry symptom.
Process lifecycle evidence reuses the existing process sample and records only a durable identity
plus start/exit time in the local bounded event history. Initial inventory and recovery uncertainty
are suppressed; lifecycle events do not trigger capture or prove causation. Offline exports replace
the local identity with an incident-local ordinal.
Disabling executable paths and applying clears the RAM metadata cache; existing immutable incidents
remain unchanged until an explicit purge. Hotkey/detector/capture/privacy/notification changes apply
immediately. Archive path and capacity are persisted for the next launch and do not silently move
existing data.

### Guided archive recovery

Settings reports archive path, schema, count, size, capacity, writer health, and any recoverable
failed capture. All actions run on a maintenance worker. Backup leaves collection running; purge/restore temporarily pause recording as explicit archive boundaries.

- **Retry oldest failed incident** retries the oldest of up to three retained accepted captures. Export it to a new standalone
  SQLite file first when you want an additional recovery copy.
- **Create verified backup** uses SQLite online backup and refuses to overwrite a file.
- **Validate source and restore** checks integrity, BlackBox identity, and schema. It requires a new
  path for a pre-restore safety backup before replacing the live archive.
- **Export inspectable evidence dataset** produces the privacy-reduced offline dataset.
- **Apply retention** and **Purge** remain disabled until their adjacent confirmation is checked.
  They are never scheduled or invoked automatically.

### Retention and local privacy purge

The Settings page can keep a confirmed number of newest incidents or purge all local evidence while
BlackBox remains open. The standalone tool remains available for scripted/offline age maintenance;
exit BlackBox first so it has exclusive archive ownership:

```powershell
.\blackbox_dataset_tool.exe keep-newest `
  "$env:LOCALAPPDATA\BlackBox\incidents.sqlite3" 200
.\blackbox_dataset_tool.exe prune-before `
  "$env:LOCALAPPDATA\BlackBox\incidents.sqlite3" 1782864000000
```

To erase all captured incidents, annotations, classifications, cached features, and learned
process profiles, supply the deliberate confirmation token:

```powershell
.\blackbox_dataset_tool.exe purge-all `
  "$env:LOCALAPPDATA\BlackBox\incidents.sqlite3" DELETE-ALL
```

These operations are transactional, use SQLite secure deletion, truncate the WAL, and compact the
archive. They cannot be undone without a backup. BlackBox never applies retention automatically.

### Privacy-preserving offline dataset

After BlackBox exits, export an archive to a new directory:

```powershell
.\blackbox_dataset_tool.exe export `
  "$env:LOCALAPPDATA\BlackBox\incidents.sqlite3" .\blackbox-dataset
```

The tool refuses to overwrite an existing directory. The dataset contains classifications,
classification history, incident-relative telemetry, privacy-normalized system events, explicit
units/status semantics, and pseudonymous keys. It excludes archive/executable paths, process names,
PIDs, creation tokens, foreground identity, native event messages/identifier payloads, and free-form
labels/notes. Review `manifest.json` and `docs/STORAGE.md` before
sharing it. To apply edited category/feedback rows back to matching incidents without changing
telemetry, labels, or notes, use `import-classifications` in place of `export`.

### Diagnostics

Diagnostics exposes collection average/P50/P95/P99/maximum, scheduling jitter, late/deadline/dropped
counts, resume gaps, provider recovery, ring utilization, process access limitations, incident
snapshot time, writer attempts/retries/exhaustion, writer time/failures, and archive/viewer status.
Transient busy/I/O writes are retried at most twice after the first attempt; permanent or exhausted
failures remain visible and never stop collection. A long suspend is counted separately
from scheduling jitter and resets cumulative-counter baselines before collection resumes.
The window follows SDL display-scale, drawable-size, and display-membership events on Windows,
Wayland/X11, and macOS. A scale transition rebuilds the local font atlas and style from canonical
values, so repeated monitor moves cannot accumulate scaling error; Diagnostics reports the effective
scale, display count, and drawable pixel size. Windows remains PerMonitorV2 DPI-aware. While the
dashboard is visible it rechecks platform accessibility preferences at most once per second, applies
or reverses its complete maximum-contrast palette without restarting, and reports whether system
animations are enabled. A preference change made while BlackBox is hidden is applied on the first visible
dashboard refresh; background recording is independent of this UI work.

The same page reports whether the Windows crash handler is armed and how many completed local dumps
exist. **Create local support bundle** writes a new, atomically published direct-v1 directory on a
background worker. The default bundle contains only allowlisted counters and feature/privacy states;
it excludes incidents, samples, processes, paths, settings, hotkeys, usernames, and annotations.
BlackBox never uploads it. Including the newest raw minidump requires both the inclusion checkbox and
the adjacent consent. Minidumps can contain stack memory and module paths. See
[USER_GUIDE.md](USER_GUIDE.md), [PRIVACY_THREAT_MODEL.md](PRIVACY_THREAT_MODEL.md), and
[USER_GUIDE.md](USER_GUIDE.md).

## Supportability and crash evidence

BlackBox provides a local, bounded support path without adding an uploader, network client, or
dependency to the recorder. Support tooling is downstream of collection and cannot be called by a
telemetry provider or the rolling recorder.

### Crash diagnostics

On Windows, the application creates the local crash directory before normal initialization and
pre-opens one uniquely named `.dmp.partial` file for the current process. The top-level exception
filter copies only the exception pointer and crashing thread ID into preallocated state, signals a
dedicated dump thread, and waits at most 15 seconds. The dedicated thread writes `MiniDumpNormal`
evidence through the already-open handle, filters that writer thread from DbgHelp's thread walk,
flushes it, and renames it to `.dmp`; this avoids asking the damaged thread and stack to run DbgHelp
or asking DbgHelp to inspect its own writer. A failed dump write resets the same bounded staging file
and retries at most five times with 100-millisecond delays. Neither path allocates nor opens another
file after the crash. Final publication tolerates only bounded transient access/sharing/lock
contention for at most 4.75 seconds. Permanent write or publication failure leaves the `.partial`
artifact visibly incomplete rather than presenting it as completed evidence. A clean shutdown stops
the worker, closes the handle, and removes the unused partial file. Existing completed dumps are
never deleted automatically.

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

### Local support bundle

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

### Failure and retention semantics

- Failed support publication leaves only the visibly suffixed `.partial` directory for inspection.
  It cannot masquerade as a completed bundle.
- Completed bundles and completed crash evidence have no automatic retention or upload policy.
- A crash during support-bundle creation does not affect the archive or collector. The partial bundle
  can be removed explicitly after inspection.
- Support bundles use one direct v1 contract. There is no pre-release migration or legacy reader.
- Crash evidence is diagnostic context, not incident truth and not proof of causation.

Operational recovery steps are in [USER_GUIDE.md](USER_GUIDE.md), and the security and
privacy boundaries are in [PRIVACY_THREAT_MODEL.md](PRIVACY_THREAT_MODEL.md).

## Recovery runbooks

Live now warns at 80% of configured archive capacity and offers recovery when a capture cannot
be saved. The recovery slot is memory-only and is lost on exit unless exported or retried.

Purge stops capture input, joins writer/viewer work, discards pending captures and queued writes,
clears recovery and rolling history, removes archive data/profiles, and invalidates viewer caches
before recording resumes. Prior exports, backups, settings, logs and crash evidence remain outside
the incident purge. Restore drains queued writes first and refuses to proceed while a failed
incident remains unresolved. Recover it, or export it and explicitly purge before restoring.

Ordinary backups and pre-restore safety copies are reopened read-only and checked for SQLite
integrity and canonical schema before exclusive publication. Occupied destinations/staging files
are refused. POSIX publication requires same-filesystem hard-link support; unsupported filesystems
produce an error rather than an unverified success.

These procedures preserve evidence first. Stop BlackBox before manually copying or moving its active
files. Never rename an incompatible archive into the active location, edit SQLite/WAL files, or
delete a `.partial` artifact until its role is understood.

### A capture could not be saved

1. Keep BlackBox running; collection continues independently of storage.
2. Open **Settings** and check the writer status and the bounded recoverable-capture message.
3. If another safety copy is needed, export the failed incident to a new standalone SQLite file.
4. Correct the underlying issue (free space, permissions, read-only media, or another process holding
   the archive), then choose **Retry oldest failed incident**.
5. If retry fails again, retain the exported standalone copy and create a support bundle. New captures remain blocked until every retained item is recovered; rolling recording continues.

### Archive locked, read-only, or full

1. Pause new reproduction work, but do not purge or replace anything.
2. Exit other tools that may hold the SQLite archive, its `-wal`, or `-shm` files.
3. Check that the archive directory is writable and has space beyond the configured logical limit.
4. Use **Refresh archive health**. If healthy, retry the recoverable incident.
5. If capacity is the intended limit, create a verified backup first, then use explicit retention to
   keep a confirmed number of newest incidents. BlackBox never deletes incidents automatically.

### Suspected corrupt archive

1. Exit BlackBox so the writer and viewer release the archive.
2. Copy the archive, `-wal`, and `-shm` files together to a safe location without modifying them.
3. Restart BlackBox and use health status only; do not allow any tool to “repair” the sole copy.
4. Restore only from a known verified BlackBox v1 backup. The UI validates identity, integrity, and
   the complete canonical table/index layout through a read-only source connection, and requires a
   new pre-restore safety-backup path.
5. Non-v1 or unversioned nonempty archives are rejected unchanged. This prerelease has no migrations
   or legacy readers. Preserve the rejected file, then explicitly select a new empty v1 archive path.

### Invalid settings or startup failure

1. Record the exact visible error and create a support bundle if the Diagnostics page is reachable.
2. Exit BlackBox and copy `product-settings.ini` and `recorder-settings.ini` privately; they are not
   included in a default support bundle.
3. An invalid file is not partially applied. Move it aside explicitly and restart to create/use direct
   v1 defaults. Do not rewrite its version field or attempt a migration.
4. If the app reports an existing instance, activate the tray instance before assuming startup failed.
5. If startup repeatedly fails, preserve completed `.dmp` files and the original settings/archive for
   investigation; do not send them without reviewing their sensitivity.

### BlackBox crashed

1. Restart BlackBox and open **Diagnostics**. Confirm whether completed local crash evidence exists.
2. Create a support bundle without the dump first; it contains safe bounded counters only.
3. Include the latest dump only if the recipient needs it and the explicit memory/path disclosure is
   acceptable. The bundle remains local until the user separately shares it.
4. Preserve the original `.dmp`; bundle creation copies rather than moves it. Completed dumps are not
   automatically deleted.
5. A `.dmp.partial` indicates publication did not complete. Do not rename it blindly. Preserve it for
   engineering inspection; only a completed `.dmp` is offered by the UI.

### Support bundle failed

1. Use a new absolute destination whose parent directory already exists.
2. Confirm neither the final directory nor `<destination>.partial` already exists.
3. Check free space and permissions. If raw-dump inclusion was selected, verify that the source remains
   a completed regular dump no larger than 64 MiB.
4. Preserve a partial directory if diagnosing a write failure. After inspection, remove or move that
   exact directory explicitly and retry with a new destination.
5. Never treat `.partial` as a completed bundle; a complete direct-v1 bundle has exactly the files
   declared in `manifest.ini`.

### Restore and backup verification

1. Create backups through **Create verified backup** to a new file; overwrite is refused.
2. Store the backup away from the active archive and record the application version that created it.
3. Before restore, choose a different new path for the pre-restore safety backup.
4. Let BlackBox validate the source. It checks integrity, application identity, required control
   state, and every canonical table/index definition without writing the source. A restore must
   preserve the exact direct schema-v1 archive and is not a schema conversion.
5. After restore, refresh health, verify incident count, open representative incidents, and confirm the
   writer can save a new controlled capture before deleting any safety copy.

### What to retain for support

- Privacy-safe support bundle (preferred first artifact).
- Exact BlackBox version and whether the build is an unsigned developer build.
- Reproduction time, action, and visible error written separately by the user.
- Verified backup or failed-incident export, shared only when incident evidence is necessary.
- Raw minidump, settings, or archive only after explicit sensitivity review.

BlackBox has no automatic support upload, remote control, updater, or evidence transmission channel.

## Saving, leaving and recovering work

Changing notification, shortcut or next-launch archive preferences preserves live recording.
Changes that interrupt recording are refused while a capture window is still being collected.
Changing only the capture window preserves existing history. Cadence, history capacity and
executable-path privacy changes intentionally begin a new recorder epoch.

Normal Exit checks drafts, accepted captures, pending writes, maintenance and exports. **Save
and exit** saves drafts and waits for accepted work; **Keep working** cancels exit. A failure
keeps the app available for recovery. **Exit without saving** explicitly permits discarding
RAM-only work. OS session termination and forced process termination cannot offer the same
interactive guarantee. Closing to the tray continues recording.

If writes fail after their retry budget, rolling recording continues, but new captures are
blocked. Up to three already accepted captures remain in memory: the in-flight write plus
the two bounded coordinator slots. Settings shows the retained count. **Retry oldest failed
incident** recovers one at a time; export preserves the oldest capture separately. Recovery
is complete only when the retained count reaches zero. Purge explicitly discards this state.

Backup progress reports copied pages and verification. Cancel requests stop cooperative
copy/verification work and cancels queued maintenance jobs. Accepted jobs otherwise drain
when exiting. A backup is published only after integrity and schema verification succeeds;
cancelled or failed partial copies are not presented as verified backups. Restore creates a
verified safety copy before replacing the active archive. Filesystem stalls can still delay
an operation beyond its cooperative timeout.

Installed CJK fallback fonts are loaded on demand when visible text needs missing glyphs;
BlackBox does not download or bundle system fonts. Glyph coverage depends on installed fonts.
