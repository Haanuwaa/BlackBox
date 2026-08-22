# Incident capture

## Capture behavior

A manual trigger records a monotonic event time `T`. The default requested range is `T - 120 s` through `T + 30 s`; both durations are part of `RecorderConfiguration`, are independently configurable, and must be nonnegative. The collector continues its normal schedule during the post-window. The first observation at or after the requested end completes the capture, so the immutable snapshot's actual end can be up to one sample interval later than the requested end.

Short uptime and ring boundaries never fabricate history. Snapshot construction selects only samples whose observation times are at or after the requested start and at or before the completion observation. `actual_start` and `actual_end` report the first and last system sample that were really retained. Recorder epochs are copied into the incident header so a future storage layer cannot silently join samples across a reconfiguration.

The incident contains:

- normalized system samples with explicit units and availability states;
- flattened process samples carrying their frame observation time and full PID/creation-token identity;
- only process metadata referenced by those selected samples;
- the original event, requested range, actual range, manual/automatic trigger counts, strongest
  automatic resource evidence, and recorder epochs.

The core snapshot exposes const spans only. Completed work is shared as `std::shared_ptr<const IncidentSnapshot>`, allowing the writer to consume the same immutable object without another full telemetry copy. The dedicated writer persists one complete incident transaction and reports its success, latency, or failure through dashboard diagnostics.

## Overlap and queue policy

BlackBox permits one collecting post-window at a time. A manual or automatic trigger received while
that window is active merges into it: the earliest requested start is retained, the requested end
extends to the latest trigger plus its post-window, and source-specific trigger counts increment.
The strongest automatic score/evidence is retained. This prevents repeated hotkeys or detector
observations from multiplying large overlapping snapshots.

## Automatic capture and feedback

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

## Windows hotkey

The default chord is `Ctrl+Shift+F12`. `IGlobalHotkeyManager` contains no Win32 types. `WindowsGlobalHotkeyManager` registers the chord with `MOD_NOREPEAT` on a dedicated ordinary-user message thread, receives `WM_HOTKEY`, and emits the capture callback. Clean shutdown posts `WM_QUIT`; the same thread calls `UnregisterHotKey` before exiting.

Windows may reject a chord already owned by other software. Registration status is visible in the dashboard and the capture button remains usable. Microsoft also documents F12 as debugger-reserved, so a failure is treated as an explicit conflict/unavailable state rather than silently changing the requested default. The V0.0.7 validation host successfully registered and unregistered `Ctrl+Shift+F12` without elevation. Sources: [RegisterHotKey](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-registerhotkey), [WM_HOTKEY](https://learn.microsoft.com/windows/win32/inputdev/wm-hotkey), and [UnregisterHotKey](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-unregisterhotkey).
