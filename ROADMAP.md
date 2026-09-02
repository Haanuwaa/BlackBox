# BlackBox engineering roadmap

This is the living roadmap for the unreleased product. Git history retains detailed implementation
checklists and dated campaign narratives; this file keeps the current product state, completed
outcomes, and genuinely open gates visible without making readers traverse years of build notes.

## Product direction

BlackBox is a private, native computer flight recorder. It continuously keeps a bounded local
history, lets a person preserve the moments around a slowdown, and explains recorded evidence while
keeping observation, inference, and uncertainty distinct.

The architectural rules in [ARCHITECTURE.md](ARCHITECTURE.md) are non-negotiable. In particular,
collection never depends on UI lifetime or frame cadence; telemetry providers remain behind the
portable contract; incident snapshots are immutable; persistence and analysis stay downstream; and
the UI observes copied state and emits commands.

The project has not shipped. There is one direct V1 schema and no legacy migration or compatibility
path. Earlier development data must be recollected or expressed in the current format.

## Current status

- Engineering version: `0.26.0`; the first public release is reserved for exactly `1.0.0`.
- Current completed test graph: 378 Release tests.
- V0.24 implementation revision `85044988da5817fa4e759b6c57d014731ce7a528` passed Windows,
  Linux, macOS, and quality/security hosted matrices. Documentation was recorded at `ba4c9c8`.
- Windows is the most qualified implementation. Linux and macOS have broad engineering parity but
  remain engineering targets until physical desktop, package, lifecycle, accessibility, signing,
  and support qualification is complete.
- Runtime diagnosis uses local statistical analysis. Offline feature export and baseline comparison
  exist; runtime ML remains behind representative held-out evidence.

## Completed engineering milestones

| Milestone | Outcome |
|---|---|
| V0.0.1-V0.0.9 | Project graph, telemetry contracts, Windows CPU/memory/disk/network/process collection, circular recording, manual capture, SQLite persistence, and incident viewing. |
| V0.1-V0.9 | Stable Windows recorder, robust anomaly scoring, personalization, bounded automatic capture, classification data, contributor ranking, recurrence, context recognition, and uncertainty-aware analysis. |
| V0.10 | Roadmap reset, immutable persistence handoff, bounded retries, archive integrity, and recovery semantics. |
| V0.11 | Native single-instance background/tray shell, global capture shortcut, notifications, autostart, and hidden-operation efficiency. |
| V0.12 | Product settings, onboarding, archive recovery, accessible UI states, and deterministic UI qualification. |
| V0.13-V0.14 | Storage/network quality, GPU/responsiveness/power context, privacy-reduced system events, and explicit unsupported boundaries. |
| V0.15-V0.16 | Direct-V1 consent/annotation protocol, frozen evaluation mechanics, calibrated statistical explanations, feedback safety, and recurrence context. |
| V0.17 | Support bundles, crash evidence, wall-clock/client/signing evidence composition, exact-revision verification, and V1 qualification tooling. |
| V0.18 | Cross-platform disk, network, power, uptime, and privacy-reduced lifecycle evidence. |
| V0.19 | Linux/macOS native events, desktop integration, engineering packages, and hosted lifecycle tests. |
| V0.20 | Wayland portal shortcuts/settings/notifications, denial and reconnect robustness, and GNOME/KDE/Sway engineering coverage. |
| V0.21 | Durable viewer mutation ordering/recovery, portable event semantics, and shared UI evidence contracts. |
| V0.22 | Linux PSI and macOS thermal pressure behind one portable pressure contract. |
| V0.23 | Audit closure: fuzz/model coverage, app/UI coverage floors, visible performance evidence, decomposition, and documentation correction. |
| V0.24 | 30 Hz idle visible rendering, four-state profiling, repository protection, zero open security alerts, and exact-revision hosted evidence. |
| V0.25 | Consumer workflow, clearer evidence language, graph legibility, keyboard workflows, interaction-aware 60 Hz presentation, and documentation consolidation. |

Detailed contracts and evidence procedures are indexed in [docs/README.md](docs/README.md).

## V0.25 - Consumer workflow and documentation consolidation

**Objective:** Make the default experience read and feel like a polished consumer application while
retaining technical depth on demand and preserving the recorder/UI independence invariant.

- [x] Replace the flat engineering hierarchy with clearer recorder, capture, current-activity,
  saved-incident, and explanation language; keep detailed diagnostics behind disclosures.
- [x] Make tray/background behavior and capture controls discoverable in onboarding and the Live page.
- [x] Add a visible keyboard guide plus capture, archive-refresh, and page-navigation workflows.
- [x] Give recent-activity graphs human time axes, hover inspection, readable labels, missing-data
  explanations, and a useful default-open state.
- [x] Raise visible cadence to 60 Hz only during recent direct interaction, retaining the 30 Hz idle
  ceiling, hidden/minimized behavior, immediate restore, and no catch-up bursts.
- [x] Add focused interaction/scheduler tests, regenerate deterministic UI evidence, and pass the
  complete Release graph.
- [x] Replace dated pre-release audit/campaign snapshots with one documentation map and concise
  living roadmap while retaining every claim boundary and gate.

## V0.26 - Honest platform evidence expansion

**Objective:** Add useful native evidence where public platform APIs support it without manufacturing
cross-platform equivalence or weakening the privacy boundary.

- [x] Add event-driven macOS normal/warning/critical memory-pressure state as a separate metric,
  never as Linux PSI, utilization, thermal state, or CPU-frequency evidence.
- [x] Expose only BlackBox's own macOS `SMAppService` launch-at-login state, including approval and
  unavailable outcomes; keep general launchd service activity unsupported.
- [x] Add a capability-gated wlroots Wayland foreground application key that immediately hashes
  bounded `app_id`, discards titles, never guesses PID, and fails closed on ambiguity or source loss.
- [x] Preserve typed process identity for Windows, macOS, and X11 while making the compositor-specific
  opaque identity explicitly non-correlatable with process and GPU evidence.
- [x] Carry the new optional evidence through normalization, immutable incidents, direct schema V1,
  local viewing, and truth review while excluding both identity forms from offline dataset export.
- [x] Add bounded tracker, privacy, capability-contract, normalization, direct-V1 round-trip, and
  owned-service diagnostics tests; pass the complete 378-test Windows Release graph.

## Release qualification gates

These are evidence-execution tasks, not missing schema or architecture work.

### Diagnostic usefulness

- [ ] Collect the qualifying consented, independently annotated, multi-hardware natural and quiet
  direct-V1 corpus on at least three hardware profiles.
- [ ] Meet the predeclared gate on the new one-shot held-out split with nonzero denominators: at
  least 80% supported-diagnosis precision, 60% supported recall, 90% Unknown-truth abstention, and
  70% contributor top-3.
- [ ] Adopt runtime ML only if a separately frozen comparison materially beats the statistical
  baseline on representative held-out evidence within privacy, footprint, and latency budgets.

### Reliability and client experience

- [ ] Run a new exact-revision operator-assisted 72-hour Windows campaign after the earlier complete
  campaign exposed scheduling/handle defects and the replacement was cancelled for a PC restart.
- [ ] Run three controlled 30-minute visible/minimized/hidden/background repetitions on the frozen
  candidate to extend the short visible-efficiency characterization.
- [ ] Complete accessibility, DPI, multi-monitor, low-end hardware, battery, and power-mode validation
  on qualifying physical clients.
- [ ] Physically validate Linux/macOS installation, tray/background behavior, portal permissions,
  notifications, GPU/pressure availability, suspend/resume, and uninstall lifecycle.

### Distribution and trust

- [ ] Produce signed and timestamped Windows packages and signed/notarized macOS packages.
- [ ] Run clean-client Windows 10 22H2 and supported Windows 11 package matrices.
- [ ] Retain exact-revision hosted Windows, Linux, macOS, quality, security, UI, client, soak, corpus,
  and package attestations required by the V1 evidence composer.

## V1.0 - First public release

- [ ] All V0.10-V0.17 acceptance criteria are proven by current evidence.
- [ ] Set every final product/package/evidence semantic-version surface to exactly `1.0.0` only after
  every independent gate passes on the same source revision.
- [ ] The app quietly records in the tray and captures manual and automatic short-lived incidents.
- [ ] Supported symptom classes have sufficient signals and measured real-world diagnostic quality.
- [ ] Incident explanations distinguish probable contributors, victims/reactions, and unknown causes.
- [ ] Feedback and recurrence improve the local experience within explicit safety/privacy bounds.
- [ ] Release packages are signed, clean-client qualified, recoverable, documented, and supportable.
- [ ] Published overhead, false-positive, accuracy, reliability, and compatibility claims match the
  retained evidence.

## Later possibilities

- Promote Linux or macOS from engineering target to supported product only after their physical
  qualification gates pass.
- Consider native ML only behind the held-out adoption gate; model complexity is not a milestone by
  itself.
- Treat opt-in anonymous feature-level collective intelligence as a separate privacy-reviewed
  product decision, never as an implicit extension of local recording.

## Exact next milestone

V0.26 platform evidence expansion is complete locally and the full 378-test Windows Release graph is
green. The next gate is to freeze one clean revision and repeat Windows/Linux/macOS/quality hosted
matrices, with special attention to the native Dispatch/ServiceManagement compile and the optional
Wayland protocol build. Then run the controlled visible-runtime comparison. Resource-dependent
physical, signing, 72-hour, and corpus gates remain open and must not be replaced by local simulation.
