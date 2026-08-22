# V0.14 Windows GPU, responsiveness, power, and event evidence

V0.14 adds passive ordinary-user evidence for game/UI stutter, audio-device changes, thermal or
power transitions, and background Windows activity. It preserves two independent paths:

```text
WindowsTelemetryProvider -> Normalizer -> system ring
WindowsSystemEventProvider -> event collector -> event ring
```

The main collector never polls the event provider. Completed snapshots join only records inside
the requested monotonic window. Both rings are fixed-capacity and overwrite oldest entries while
reporting drops/overwrites.

## One-second gauges

- A persistent English PDH `GPU Engine(*)\\Utilization Percentage` query groups instances by
  physical engine and reports the busiest engine. It does not sum engines into an impossible
  utilization percentage. `GPU Adapter Memory(*)` dedicated/shared usage is summed across exposed
  adapters. Foreground GPU is the maximum engine instance belonging to the foreground PID.
- `GetForegroundWindow`, `GetWindowThreadProcessId`, and `GetProcessTimes` produce a durable
  `(PID, creation token)` only when the privacy control is enabled. No title, class, command line,
  or screen content is read or stored. Foreground correlation identifies the visible application,
  not the cause of a stall.
- Persistent Processor Information PDH counters expose `_Total` DPC/ISR time and DPC rate. High
  values can corroborate scheduling pressure relevant to real-time audio/video work, but they do
  not identify a driver or prove that an audible glitch occurred.
- `CallNtPowerInformation(ProcessorInformation)` records average current MHz, maximum MHz, and
  `MhzLimit`; the last is Windows' thermal-throttle ceiling. `GetSystemPowerStatus` records AC vs
  battery, battery fraction when available, and battery-saver state. `GetTickCount64` supplies
  uptime. Frequency and limit values describe the sampled system, not a specific core or reason.

PDH is sampled at the existing one-second normal cadence. Microsoft explicitly cautions that PDH
is not intended for collection more frequently than once per second, so these counters are not
promoted to the faster recorder profiles' fast tier.

## Discrete events

Native callbacks enqueue only source, normalized kind, severity, native event ID, optional source
UTC milliseconds, and a small numeric detail:

- `PowerRegisterSuspendResumeNotification`: suspend and automatic/user resume;
- `CM_Register_Notification`: device enumerate/start/remove;
- `IMMNotificationClient`: audio endpoint add/remove/state/default-device changes;
- `EvtSubscribe`, future events only: Service Control Manager 7031/7034/7036/7040, Defender
  1000/1001/1116-1119/5007, Windows Update 19/20/25/31/34/41/43/44, Application Error 1000,
  Application Hang 1002,
  DNS Client resolution timeout 1014, Display timeout recovery 4101, and `disk` I/O retry 153.

The callback queue holds 1,024 events, the portable ring defaults to 4,096, one drain is capped at
256, and the configurable ring hard cap is 65,536. Native callback drops, provider failures and
recoveries, poll timing, and ring overwrites are visible in Diagnostics.
The collector also maintains one aggregate counter for each bounded source family (power, device,
audio, service control manager, Defender, Windows Update, application, network, graphics, storage,
and opt-in process lifecycle). These counts
contain no native message text or identifiers and must sum exactly to the recorded-event total. They support
wall-clock device-churn qualification without weakening the privacy boundary.

## Privacy and disablement

All new privacy-sensitive sources default off for fresh product settings. Settings
separately control foreground identity, process lifecycle identity, power/device events, audio endpoint events, and selected
Windows Event Log evidence. Disabling every discrete source performs no native registration and
the base recorder continues unchanged. Reconfiguration stops and joins the event worker, clears
the event epoch/queue, applies the new gates, and restarts only if recording was active.

Process lifecycle reuses the existing one-second process enumeration and does not register another
native source. It suppresses initial inventory, post-resume/reconfigure resynchronization, and failed
or incomplete enumeration observations. Accepted start/exit rows contain only the durable PID and
creation token already used by process samples; they are context only and cannot trigger capture.

The core event type cannot store Event Log messages, device instance IDs, endpoint IDs, storage
LBAs/device paths/PDO identities, service names, threat names, update titles, queried hostnames,
crashing application/module names, exception codes, fault paths,
window titles, or any free-form payload. Dataset v1 excludes foreground identity and converts
lifecycle identity to an incident-local ordinal. Existing immutable incidents retain whatever the user previously
chose to capture until explicit retention or privacy purge.

## Researched non-adoptions

BlackBox does not use `IsHungAppWindow` or active `SendMessageTimeout` probes. Microsoft says
`IsHungAppWindow` is not intended for general use, and active probing would alter application
behavior. Future-only Application Hang event 1002 is passive and is presented as a recorded
symptom.

With automatic detection enabled, a recorded event 1002 requests capture through the same bounded
two-slot coordinator used by manual and resource triggers. The incident header retains the
`application_hang` signal, the event remains in the bounded event stream, and pipeline v13 can emit
a Windows-reported application-hang explanation without inventing resource pressure. Request,
merge, and rejection counts are visible in Diagnostics.

Future-only Application Error event 1000 is normalized as `application_crash` with only its source,
kind, level, numeric ID, and time. The application name, faulting module, exception code, fault path,
message, and event payload never enter the portable record. With automatic detection enabled, the
OS-reported crash may request capture through the same bounded coordinator. Pipeline v13 may report
only that Windows reported a nearby application crash; it cannot name the application or module or
infer the defect or any other root cause. Exact application/`application_crash`/1000 matching within
five seconds is required.

DNS Client event 1014 is recorded only as the normalized `dns_resolution_timeout` kind, warning
level, numeric event ID, and time. Its Event Log message and queried hostname are never rendered or
retained. When it falls within five seconds of an incident marker, pipeline v13 can report the
precise Windows-observed symptom. It does not call that event the root cause and does not request
automatic capture: resolver timeouts are too common and context-dependent to satisfy the current
false-positive gate by themselves. If the incident has an independently aligned resource diagnosis,
that stronger explanation wins and the DNS event remains visible context.

Future-only Display event 4101 is normalized as `display_driver_recovery` with the graphics source,
warning level, numeric event ID, and time. The event message, driver name, adapter identity, and
payload are never read into the portable event. Microsoft documents Timeout Detection and Recovery
as Windows detecting an apparently frozen GPU task, resetting the graphics stack/GPU, restoring the
desktop, and logging that the display driver stopped responding and recovered. Accordingly, with
automatic detection enabled, this OS-confirmed symptom can request capture through the same bounded
two-slot coordinator. Pipeline v13 may report only the exact Windows display-timeout-recovery
symptom; it cannot identify a faulty driver, application, GPU, or other root cause. Mismatched
source/kind/ID tuples are ignored.

Future-only provider `disk` event 153 is normalized as `storage_io_retry` with the storage source,
its numeric level/ID, and time. Its LBA, device path, PDO identity, Event Log message, and payload are
never read into the portable record. Microsoft documents event 153 as Storport reporting that a
request timed out and the I/O operation was retried. With automatic detection enabled, that exact
OS-reported symptom may request a disk-scoped capture through the bounded two-slot coordinator.
Pipeline v13 can report only that Windows retried a storage I/O operation; it cannot infer overload,
cabling, controller, driver, media, firmware, application, or hardware failure. Exact
storage/`storage_io_retry`/153 matching is required.

Continuous frame-presentation ETW and high-rate audio ETW were not adopted for the always-on base
recorder. They materially increase event volume/overhead, have workload- and driver-dependent
semantics, and would require a separate validated opt-in design. Audio endpoint transitions plus
DPC/ISR pressure are defensible context, not a direct audio-glitch counter. The UI therefore reports
frame pacing and audio-glitch automatic capture as unsupported. Endpoint transitions and DPC/ISR
pressure remain inspectable context and never become a frame/audio symptom claim.

The low-rate `DwmGetCompositionTimingInfo(NULL, ...)` alternative was also evaluated and rejected.
On the current ordinary-user Windows validation host its detailed displayed/late/dropped/missed
counters remained zero across 19 normalized intervals, and Microsoft's compatibility guidance
[*discourages the API after Windows 8*](https://learn.microsoft.com/windows/compatibility/queued-present-model-is-being-deprecated)
because its detailed queued-present model was retired. BlackBox
therefore stores none of these fields and makes no desktop-wide frame-quality claim. A future frame
signal still requires a separately bounded, opt-in design with current presentation semantics and
real workload/quiet calibration.

Primary API semantics are documented by Microsoft: [performance counters](https://learn.microsoft.com/windows/win32/perfctrs/about-performance-counters),
[audio device events](https://learn.microsoft.com/windows/win32/coreaudio/device-events),
[configuration notifications](https://learn.microsoft.com/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_register_notification),
[suspend/resume notifications](https://learn.microsoft.com/windows/desktop/api/Powerbase/nf-powerbase-powerregistersuspendresumenotification),
[Event Log subscriptions](https://learn.microsoft.com/windows/win32/wes/subscribing-to-events),
and [`PROCESSOR_POWER_INFORMATION`](https://learn.microsoft.com/windows/win32/power/processor-power-information-str).
Microsoft's [WDDM timeout detection and recovery documentation](https://learn.microsoft.com/windows-hardware/drivers/display/timeout-detection-and-recovery)
defines the recovery semantics used for the Display 4101 normalization.
Microsoft's [disk-error troubleshooting guidance](https://learn.microsoft.com/troubleshoot/windows-server/backup-and-storage/troubleshoot-data-corruption-and-disk-errors)
defines the timeout-and-retry semantics used for the `disk` event 153 normalization.
Microsoft's [application/service crash troubleshooting guidance](https://learn.microsoft.com/troubleshoot/windows-server/performance/troubleshoot-application-service-crashing-behavior)
defines Application Error event 1000 as the crash symptom used by this normalization.
