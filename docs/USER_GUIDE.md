# BlackBox user guide

BlackBox records a bounded telemetry history in RAM. It writes an incident to SQLite
only after a manual or automatic capture; ordinary recording does not stream telemetry to disk.

Windows is the only production target in this pre-release. Linux DEB/RPM/TGZ and macOS TGZ outputs
are explicitly unsupported engineering previews used to exercise platform boundaries. Linux can
collect native telemetry, publish bounded notifications through the XDG portal with a freedesktop
service fallback, publish coalesced background status, and request its configured global shortcut
through the XDG Desktop Portal. Shortcut sessions recover after a portal restart, while a shortcut
removed by the desktop remains visibly unavailable until reapplied. Desktop policy may require user
approval or leave any service unavailable. Wayland foreground-application identity remains explicitly
unsupported because no standardized permission-bounded portal exposes an active-window PID. Both
previews supply native CPU/memory/disk/network/power/uptime/process
telemetry and passive local-link/TCP-quality evidence, but no qualified product experience. macOS's
unsigned engineering `.app` can expose menu-bar controls, request launch at
login through macOS ServiceManagement, deliver permission-gated local notifications, and follow the
system's increased-contrast/reduced-motion preferences. Its passive Input Monitoring-gated global
shortcut is implemented; signing/notarization and physical-client qualification remain open.

## Start and capture

1. Start `blackbox.exe` as an ordinary desktop user. Administrator rights are not required.
   First-run onboarding follows three steps: keep a short bounded history in RAM, capture after a
   slowdown, then review the saved local evidence. It also makes clear that correlation is not proof.
2. Leave BlackBox running while reproducing or waiting for a short performance problem.
3. At the problem, press the configured hotkey (**Ctrl+Shift+F12** by default) or select
   **Capture incident** in Live.
4. Keep BlackBox open during the 30-second post-window. Live shows capture and writer state.
5. Open Incidents, select the saved item, and inspect system graphs and process peaks. Labels and
   notes are editable and survive restart. Choose Unknown, System freeze, Game stutter,
   Application slowdown/hang, Network, or Audio and save to classify the incident.

## Background and tray controls

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

## Product pages

- **Live** leads with recorder readiness, the configured hotkey, one **Capture what just happened**
  action, saved-incident count, and archive state. Current CPU/memory/throughput remains visible;
  platform/capture internals, forensic signals, rolling histories, and active processes remain
  available through named disclosure controls.
- **Incidents** searches, sorts, and pages immutable saved captures. Loading, a genuinely empty
  archive, no search matches, and archive failure are separate states with direct next actions.
- **Detail** leads with symptom, likely contributor, uncertainty, plain-language evidence, and the
  correlation caveat plus at most three standout contributors before deeper evidence is expanded.
  GPU and responsiveness/power plots, foreground
  transitions, and privacy-normalized Windows events share the incident-relative timeline. Pan or
  wheel-zoom one plot to update every plot. Hover any plot to place one sticky cyan inspection
  cursor at the same marker-relative second across all timelines; clear it separately from the
  orange incident marker. These are correlations/context, not proof of cause.
- **Patterns** shows bounded recurring-evidence groups and explicit noise/unique captures.
- **Settings** owns validated recorder/product controls and guided archive recovery.
- **Diagnostics** shows provider, ring, timing, writer, accessibility, DPI, unavailable-state,
  crash-evidence, and local support-bundle detail. Use **Ctrl+1** through **Ctrl+6** to switch pages.

## Automatic capture

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

## Statistical ranking

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
separate name-based profile. See `PERSONALIZATION.md` before interpreting these identities as
cryptographic binary identity—they are not hashes or signatures.

V0.6 adds a **Potential contributors** table. “Likely contributor (correlation only)” means the
recorded process activity was large, preceded the marker, matched a system resource anomaly, had
adequate duration, and had adequate evidence coverage. “Potential contributor” is weaker or
incomplete preceding correlation. **Ambiguous correlate across marker** began before the marker but
has more anomalous samples after it. **Possible victim/reaction** began only after the marker and is
not a causal rank. Expand the evidence rather than treating any label as proof: the row exposes all
five score factors, pre/post sample counts, coverage, missing metrics, onset, and duration. See
`CONTRIBUTOR_RANKING.md` for
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

## Recurring incidents

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
your archive; they are not diagnostic proof. See `RECURRING_INCIDENTS.md` for feature scaling,
thresholds, bounds, and limitations.

Overlapping triggers merge into the active capture. At most two immutable incidents can be queued;
the UI reports a full queue instead of allowing unbounded memory use. Use **Exit BlackBox** from the
tray for a full shutdown; ordinary window close keeps the recorder active when the tray is usable.

## Statuses and known gaps

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

## Recorder and product settings

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

## Guided archive recovery

Settings reports archive path, schema, count, size, capacity, writer health, and any recoverable
failed capture. All actions run on a maintenance worker; collection does not wait for them.

- **Retry failed incident** retries the one bounded failed-capture slot. Export it to a new standalone
  SQLite file first when you want an additional recovery copy.
- **Create verified backup** uses SQLite online backup and refuses to overwrite a file.
- **Validate source and restore** checks integrity, BlackBox identity, and schema. It requires a new
  path for a pre-restore safety backup before replacing the live archive.
- **Export inspectable evidence dataset** produces the privacy-reduced offline dataset.
- **Apply retention** and **Purge** remain disabled until their adjacent confirmation is checked.
  They are never scheduled or invoked automatically.

## Retention and local privacy purge

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

## Privacy-preserving offline dataset

After BlackBox exits, export an archive to a new directory:

```powershell
.\blackbox_dataset_tool.exe export `
  "$env:LOCALAPPDATA\BlackBox\incidents.sqlite3" .\blackbox-dataset
```

The tool refuses to overwrite an existing directory. The dataset contains classifications,
classification history, incident-relative telemetry, privacy-normalized system events, explicit
units/status semantics, and pseudonymous keys. It excludes archive/executable paths, process names,
PIDs, creation tokens, foreground identity, native event messages/identifier payloads, and free-form
labels/notes. Review `manifest.json` and `docs/CLASSIFICATION_DATASET.md` before
sharing it. To apply edited category/feedback rows back to matching incidents without changing
telemetry, labels, or notes, use `import-classifications` in place of `export`.

## Diagnostics

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
[SUPPORTABILITY.md](SUPPORTABILITY.md), [PRIVACY_THREAT_MODEL.md](PRIVACY_THREAT_MODEL.md), and
[RECOVERY_RUNBOOKS.md](RECOVERY_RUNBOOKS.md).
