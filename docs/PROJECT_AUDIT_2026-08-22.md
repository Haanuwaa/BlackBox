# Project audit — 2026-08-22

BlackBox has implemented most of the original Windows product architecture. The remaining distance
to a trustworthy public `1.0.0` is dominated by real-world evidence, distribution trust, and product
polish rather than missing recorder mechanics. This audit separates work that can proceed on one
development machine from gates that require outside hardware, participants, or signing authority.

## Current capability assessment

| Area | State | Assessment |
| --- | --- | --- |
| Native rolling recorder | Implemented | The bounded RAM recorder, tiered collection, manual capture, asynchronous persistence, and self-diagnostics preserve the original recorder-first design. |
| Windows telemetry | Implemented, qualification continuing | CPU, memory, storage, network, processes, GPU/context, responsiveness, power, audio-device, lifecycle, and privacy-reduced Windows events are capability-gated. Some desired symptoms remain explicitly unsupported when Windows exposes no reliable low-cost signal. |
| Incident product workflow | Implemented | Tray recording, capture, incident archive, detail timelines, feedback, recurrence, settings, diagnostics, recovery, and support bundles exist. |
| Statistical intelligence | Implemented | Robust baselines, personalized executable history, automatic detection, context recognition, recurrence, evidence-linked contributor ranking, uncertainty, and conservative local feedback are native and optional. |
| Native ML | Not adopted | No model or ML runtime ships. The representative held-out dataset needed to prove a material improvement over the statistical baseline does not exist yet. |
| Windows release quality | Strong locally, externally incomplete | The current local Release and Debug graphs pass 313/313 tests, including deterministic crash-dump publication contention. Long wall-clock, physical-client, signed-package, and multi-hardware diagnostic gates remain open. |
| Linux | Architecture/test host only | Portable headless code is compiled and tested on Linux CI, but `telemetry/linux` and `platform/linux` contain no production backend. |
| macOS | Reserved boundary only | There is no provider or native shell implementation and no product-support claim. |
| Security/dependencies | Hardened, cleanup in progress | The security-focused CodeQL graph passes, no open high/critical path alert remains after review, and Dependabot reports no open vulnerability alert. Historical broad quality-query alerts still need administrative closure after the narrowed workflow is merged. |
| UI | Preview simplification implemented; physical review pending | First run is a three-action recorder/capture/review path. Live and Detail lead with glanceable status and conclusions while technical counters, histories, process tables, and raw factors remain available through progressive disclosure. Explicit archive loading/empty/no-match/unavailable states are tested. Physical keyboard, display, and accessibility review remains open. |

The roadmap contains 305 completed and 21 open checklist items (93.6% mechanically complete), but
that percentage is not release readiness. Several open items are the highest-value acceptance gates:
diagnostic accuracy on new natural evidence, 72-hour reliability, physical usability, trusted
signing, and final evidence composition.

## What is genuinely blocked by resources

- A qualifying diagnostic corpus needs consented natural and quiet sessions on at least three real
  hardware profiles plus two independent non-operator annotators per incident. Synthetic machines,
  relabeled old data, or CI runners cannot satisfy it.
- Windows 10/11, mixed-DPI/multi-monitor, low-end, battery, accessibility, sleep, lock, and device
  behavior require physical observation. One host can provide useful rehearsal evidence but not the
  supported-matrix claim.
- An official build needs a trusted Authenticode certificate and timestamp. Unsigned packages can be
  useful private previews but cannot be described as official releases.
- The 72-hour campaign consumes elapsed time and requires safe operator actions. It can run on the
  current machine once the source revision is frozen, but it should not prevent other work in a
  separate build/worktree.

These constraints mean the project can produce a strong unsigned Windows preview now, but it cannot
honestly flip to `1.0.0` under the current release contract.

## Recommended work that can proceed now

1. **Close qualification correctness first.** Land the settings-parser preflight, compiled revision
   binding, and automatic-capture counters; obtain a clean same-revision build/CI result; then repeat
   the overnight campaign. Treat the interrupted partial campaign as a retained diagnostic failure,
   not evidence.
2. **Physically review the preview-quality UX pass.** Live and Incident Detail now lead with status,
   one plain-language conclusion, uncertainty, and the top few contributors; raw counters and
   detailed factors remain behind progressive disclosure. First-run and archive empty/failure states
   have automated render coverage. Complete the keyboard-focus and real-display walkthrough before
   visual restyling or a public preview claim.
3. **Reduce maintenance hotspots without changing boundaries.** Split the 169 KiB SQLite archive,
   93 KiB dashboard, 87 KiB application composition, and 85 KiB viewer service by existing domain
   responsibilities. Characterization tests and performance measurements must precede each split;
   no new service may enter the collection path.
4. **Start Linux as an engineering MVP.** Implement Linux CPU/memory first behind
   `ITelemetryProvider`, then disk/network and process identity/rates. Run the provider conformance,
   normalization, recorder, storage, headless, and sanitizer graphs on hosted Linux. Add a Linux
   background shell and package only after the provider is trustworthy. Do not advertise Linux
   support from compilation alone.
5. **Prepare ML research, not runtime ML.** Add an offline feature export and benchmark adapter around
   the existing versioned incident features. Predeclare statistical-baseline, candidate-model,
   latency, memory, calibration, abstention, and subgroup comparisons. Training may use separate
   tooling; shipped inference must remain native and optional. Do not add ONNX Runtime until a new
   held-out corpus shows a material, reproducible gain with no recorder dependency.
6. **Keep macOS after the Linux boundary is proven.** Linux will expose portability assumptions with
   cheaper hosted feedback. macOS then needs its own telemetry, hotkey, tray/background, crash,
   accessibility, packaging, and physical validation work; it is not a small compiler switch.
7. **Make documentation easier to navigate.** Preserve the current audit trail, but separate live
   status/commands from milestone history. `ROADMAP.md` and `ARCHITECTURE.md` should keep concise
   current contracts and link to dated evidence notes for historical diagnostics.

## Suggested execution order

The next engineering sequence is:

1. qualification fix and CodeQL workflow integration;
2. clean-revision full Release and hosted CI;
3. a fresh overnight soak on that revision;
4. preview UX/onboarding and maintainability slices while the 72-hour/corpus/signing gates are
   resource-blocked;
5. Linux CPU/memory provider MVP;
6. offline ML experiment interface and baseline report;
7. broader Linux telemetry, followed later by macOS.

Cross-platform and ML work are therefore appropriate now as parallel, explicitly non-release
engineering tracks. They must not dilute Windows reliability, invent diagnostic-quality claims, or
change the invariant that collection succeeds with UI, storage, analysis, and ML absent.
