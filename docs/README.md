# BlackBox documentation

BlackBox keeps detailed engineering contracts because telemetry, privacy, diagnostic claims, and
release evidence must remain reviewable. This page is the entry point; most readers should not need
to browse every file.

## Use the application

- [User guide](USER_GUIDE.md): first run, capture, incident review, keyboard controls, tray behavior,
  recovery, and privacy expectations.
- [Configuration](CONFIGURATION.md): validated recorder and product settings.
- [Supportability](SUPPORTABILITY.md): diagnostics, support bundles, and crash evidence.
- [Recovery runbooks](RECOVERY_RUNBOOKS.md): safe archive and settings recovery.

## Understand the product

- [Telemetry](TELEMETRY.md): what each platform can and cannot measure.
- [Analysis](ANALYSIS.md): observation, inference, confidence, and abstention semantics.
- [Privacy threat model](PRIVACY_THREAT_MODEL.md): local-data boundaries and disclosure risks.
- [Platform parity](PLATFORM_PARITY.md): Windows, Linux, and macOS outcome map.
- [Performance](PERFORMANCE.md): measured overhead and resource budgets.

## Build and contribute

- [Architecture](../ARCHITECTURE.md): authoritative dependency and lifetime invariants.
- [Build and release](BUILD_AND_RELEASE.md): supported build graphs and package creation.
- [Quality gates](QUALITY_GATES.md): tests, analysis, fuzzing, coverage, dependencies, and CI.
- [Storage](STORAGE.md), [incident capture](INCIDENTS.md), and [viewer](VIEWER.md): persistence and
  interaction contracts.
- [Logging](LOGGING.md) and [fixtures](FIXTURES.md): bounded diagnostics and test data.

Detailed analysis references are grouped by concern: [intelligent analysis](INTELLIGENT_ANALYSIS.md),
[personalization](PERSONALIZATION.md), [contributor ranking](CONTRIBUTOR_RANKING.md),
[recurrence](RECURRING_INCIDENTS.md), [context](CONTEXT_RECOGNITION.md), and
[classification data](CLASSIFICATION_DATASET.md).

## Qualify a release

- [Release readiness](RELEASE_READINESS.md): current evidence status and exact V1 policy.
- [UI qualification](UI_QUALIFICATION.md) and [client qualification](CLIENT_QUALIFICATION.md):
  deterministic checks plus the required physical-client review.
- [Wall-clock soaks](WALL_CLOCK_SOAKS.md): overnight and operator-assisted 72-hour campaigns.
- [Dogfood protocol](DOGFOOD_PROTOCOL.md) and [diagnostic evaluation](DIAGNOSTIC_EVALUATION.md):
  consented corpus collection and held-out quality gates.
- [V1 release evidence](V1_RELEASE_EVIDENCE.md): same-revision composition of all independent gates.

The remaining files are focused signal, platform, packaging, or evidence specifications linked from
the documents above. Dated pre-release audit and campaign snapshots are intentionally not carried as
living documentation; Git history is the project archive.
