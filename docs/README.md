# BlackBox documentation

Start with the [user guide](USER_GUIDE.md) to use the app, or
[current candidate](CURRENT_CANDIDATE.md) to see what is implemented, tested and still open.
The engineering version is defined in `CMakeLists.txt`; test counts and CI status belong only
in the candidate page, with the revision and date that produced them.

## Use and recover

- [User guide](USER_GUIDE.md): first run, capture, review, export, support bundles and recovery.
- [Configuration](CONFIGURATION.md): defaults, valid settings, paths and privacy controls.
- [Privacy](PRIVACY_THREAT_MODEL.md): local data, disclosure risks and trust boundaries.

## Build and understand

- [Build, CI and packaging](BUILD_AND_RELEASE.md): prerequisites, presets, quality gates and fixtures.
- [Architecture](../ARCHITECTURE.md): dependency rules, data flow, threads and lifetime invariants.
- [Telemetry](TELEMETRY.md): normalized metrics, event evidence and pressure semantics.
- [Storage](STORAGE.md): capture, schema, durability, viewer, datasets and logging.
- [Analysis](ANALYSIS.md): statistical explanations, personalization, contributors, recurrence and context.
- [Platform parity](PLATFORM_PARITY.md): capabilities and explicit OS limitations.
- [Performance](PERFORMANCE.md): budgets, measurement procedures and historical reference values.

## Qualify and release

- [Testing](TESTING.md): UI, physical clients and wall-clock campaigns.
- [Evaluation](EVALUATION.md): consent, annotation, held-out quality gates and offline ML.
- [Release readiness](RELEASE_READINESS.md): acceptance ledger and exact V1 policy.
- [Release evidence](RELEASE_EVIDENCE.md): composition of independent, same-revision evidence chains.
- [Roadmap](../ROADMAP.md): completed outcomes and open product gates.

## Keeping this documentation accurate

Update the owning guide when behavior changes, and link to it instead of duplicating its contract.
Record validation with an exact revision, configuration and scope; a historical pass is not a pass
for edited source. Keep manual qualification separate from automated fixtures. Old implementation
diaries and removed guides remain in Git history. Run `python scripts/check-docs.py` after edits;
CI checks local links and heading anchors as well as the release-claims contract. External URLs
are not fetched by that check.
