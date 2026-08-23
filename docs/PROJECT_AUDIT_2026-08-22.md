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
| Native ML | Not adopted; offline harness implemented | No model or ML runtime ships. A label-free versioned feature exporter and verified baseline/candidate comparison tool now support research, but the representative held-out dataset needed to prove a material improvement does not exist yet. |
| Windows release quality | Strong locally and hosted, externally incomplete | Exact revision `949e919...` passes 313/313 local Release and Debug tests plus both hosted Windows and quality/security workflows with locally verified attestations. The successor UI branch passes 316/316 Release tests. The overnight campaign is active; 72-hour, physical-client, signed-package, and multi-hardware diagnostic gates remain open. |
| Linux | Measured system/process engineering preview | A Linux-only provider reads bounded CPU, memory, physical-device I/O, non-loopback network I/O, and per-process identity/CPU/RSS/I/O evidence behind `ITelemetryProvider`. Portable strict-parser tests, a native hosted provider contract, a bounded overhead gate, and an extracted-package desktop smoke exist. The TGZ is explicitly an engineering preview; shell, representative distribution/physical validation, and a support claim remain absent. |
| macOS | Reserved boundary only | There is no provider or native shell implementation and no product-support claim. |
| Security/dependencies | Hardened, cleanup in progress | The security-focused CodeQL graph passes, no open high/critical path alert remains after review, and Dependabot reports no open vulnerability alert. Historical broad quality-query alerts still need administrative closure after the narrowed workflow is merged. |
| UI | Coherent preview visual system implemented; physical review pending | A semantic native palette, system-font fallback, consistent geometry, selected navigation, metric cards, and grouped archive/detail/settings surfaces now reinforce the three-action recorder/capture/review path. Technical counters and raw factors remain available through progressive disclosure. Explicit archive states and keyboard/high-contrast behavior are tested. Physical keyboard, display, and accessibility review remains open. |

The roadmap contains 308 completed and 21 open checklist items (93.6% mechanically complete), but
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

1. **Finish the active qualification clock.** Settings preflight, compiled revision binding,
   automatic-capture counters, crash publication contention, and same-revision hosted CI are green.
   Preserve the frozen executable and scripts until the active overnight campaign either publishes
   verified evidence or retains a diagnostic `.partial` failure; then schedule the operator-assisted
   72-hour run.
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

## Implementation update — 2026-08-22

Recommendations 3 through 5 have completed their first bounded slices on the isolated maintenance
branch. Application shutdown/reporting, settings rendering, and SQLite backup/restore are separate
translation units; the largest originals fell from approximately 87/93/169 KiB to 78/88/161 KiB
without changing module ownership. Linux CPU/memory parsing and provider composition are implemented
without adding unsupported metrics or a non-Linux target edge. The offline model tool is build-only,
opens evidence read-only, excludes truth and local identity from feature matrices, and compares only
independently verified same-corpus reports. The complete Windows Release graph passes 324/324 tests.

The next bounded slice adds the code-native semantic UI system and grouping described above, broadens
Linux through the shared normalizer with disk/network/process evidence, and makes the complete Linux
desktop target a hosted build-and-diagnostic-smoke contract. macOS remains deliberately deferred
until the Linux shell/packaging boundary is proven; no mock-backed macOS build is described as native
telemetry. Runtime ML remains unadopted because the held-out evidence gate has not changed.

This update completes maintainability scaffolding, broader cross-platform provider engineering, and ML research
infrastructure. A subsequent bounded slice reuses Linux pseudo-file buffers, adds a native overhead gate and
extract-and-launch engineering package smoke, and makes core logging bounded, single-line, component-tagged,
elapsed-time stamped, and reentrancy-safe. The complete Windows Release graph passes 330/330 tests.
It does not change the audit's release conclusion: a fresh wall-clock campaign,
72-hour actions, physical matrix, multi-hardware corpus, signing, and one-shot held-out quality gates
remain external evidence work.
