# Quality and security gates

The quality layer is build and test infrastructure. It does not enter the desktop executable's
runtime dependency graph, change collection, add a recorder thread, or alter any archive/settings
format. BlackBox remains pre-release and accepts only its current direct-v1 formats; these gates do
not introduce migrations, compatibility adapters, or legacy fixtures.

## Hosted jobs

`.github/workflows/quality.yml` defines independent fail-closed jobs:

- dependency-policy validation and a validated CycloneDX 1.5 SBOM artifact;
- pull-request dependency review, failing for newly introduced vulnerabilities of moderate severity
  or higher;
- CodeQL C++ analysis with the extended security query suite and an explicit manual production
  graph. The desktop executable and every product/development tool keep all production translation
  units observable, while tests, fixtures, and benchmarks stay out of the CodeQL database because
  their production dependencies are already traced and their own code is covered by warnings-as-
  errors, MSVC native analysis, sanitizers, coverage, property tests, fuzzing, and execution. Broad
  CodeQL quality queries are intentionally excluded from the Security tab because they report test-
  registration helpers and generated dependency headers. Pinned third-party packages are resolved
  before CodeQL initialization, so manual tracing observes only the subsequent six-target BlackBox
  production build. A 60-minute job timeout fails closed, and same-workflow/same-ref concurrency
  cancels superseded runs instead of allowing obsolete scans to consume runner capacity. This trace
  hygiene reduced the exact hosted CodeQL job from 35:19 to 15:46 (55.3%) without reducing the
  `security-extended` query suite or production graph;
- MSVC native `/analyze` with warnings treated as errors across product and test translation units;
- Windows AddressSanitizer across the app and test graph;
- Linux UndefinedBehaviorSanitizer for the portable headless graph;
- a Clang libFuzzer seed smoke plus a bounded 60-second native campaign; and
- Linux line/branch coverage with 60%/45% minimums and retained HTML/XML reports.

Every GitHub Action reference is an immutable 40-character commit. Dependabot checks those action
pins weekly. The vcpkg registry baseline is an immutable commit, the direct dependency and ImGui
feature sets are allowlisted, and the manifest version must match the CMake project version.

An authored workflow is not evidence that GitHub executed it. After all required jobs succeed on a
push or manual run, each workflow's `hosted-evidence` job publishes a direct-v1 attestation binding
the GitHub SHA, repository, workflow, run ID/attempt, ref, and exact writer script. Pull-request runs
cannot publish release attestations. V0.17 requires both downloaded artifacts to pass
`verify-hosted-ci-attestation.ps1` for the release revision; see `V017_RELEASE_EVIDENCE.md`.

The final V1 composition is a separate fail-closed gate documented in `V1_RELEASE_EVIDENCE.md`.
It independently re-runs V0.17, binds the evaluator to the signed package, recomputes the frozen
held-out report, and requires its calibration/report fingerprints to match the consumed passing
one-shot attempt. `v1_release_evidence_script_contracts` parses both scripts and exercises the
local-uncommitted and partial-output rejection paths in every Windows graph. The contract test does
not claim the external evidence exists.

## Local Windows commands

Run the mechanically checked policy contracts first:

```powershell
./scripts/verify-dependency-policy.ps1
./scripts/verify-quality-gates.ps1
```

Generate a validated SBOM after a manifest configure has produced a vcpkg status file:

```powershell
./scripts/generate-sbom.ps1 `
  -StatusPath out/build/windows-vs2026-release/vcpkg_installed/vcpkg/status `
  -OutputPath out/quality/blackbox.cdx.json
```

Visual Studio 2026 has local presets for the Windows gates:

```powershell
cmake --preset windows-vs2026-static-analysis
cmake --build --preset windows-vs2026-static-analysis

cmake --preset windows-vs2026-address-sanitizer
cmake --build --preset windows-vs2026-address-sanitizer
ctest --preset windows-vs2026-address-sanitizer
```

The ASan preset uses `cmake/triplets/x64-windows-blackbox-asan.cmake`, so Catch2, SDL, ImGui,
ImPlot, and SQLite are compiled with the same instrumentation and MSVC STL annotations as BlackBox.
The compiler ASan runtime is copied beside the app and test binary. The deliberate child-process
unhandled-exception probe is excluded from ASan because its required access violation is the test
input; it remains required in every ordinary Debug/Release graph.

Instrumentation modes are isolated: MSVC analysis and coverage cannot be combined with sanitizer or
fuzzer modes, and coverage cannot be combined with ASan, UBSan, or fuzzing. CMake rejects invalid
combinations rather than silently producing ambiguous evidence.

## Direct-v1 pre-release contract

Every build graph registers `pre_release_direct_v1_contract`. The cross-platform CMake check pins
the SQLite archive schema and the eleven persisted settings, dataset, support, soak, dogfood,
evaluation, truth-review, and campaign-status formats to version 1. It also requires exactly one schema publication
at `PRAGMA user_version=1` and fails if production sources introduce migration, legacy-reader,
compatibility-reader, or versioned alternate-schema tokens. Ordinary parser tests prove that
otherwise valid product and recorder settings changed to format 2 are rejected without conversion.
The source contract complements behavioral parser/archive tests; it does not claim that text
inspection replaces corruption, property, fuzz, or restore coverage.

Every graph also registers `release_claims_documentation_contract`. It binds the public telemetry
matrix's implemented V0.14 GPU and responsiveness claims to the Windows provider and rejects the
superseded pre-V0.14 GPU-research wording. It also requires the consented corpus, one-shot held-out,
physical-client, unsigned-package, and clean-client disclosures to remain explicitly open. This
contract pins application crash in the nine-class acquisition matrix, requires exact `1.0.0`
package names in final client/V0.17/V1 evidence instructions, and rejects a prerelease package name
from either final composition guide. This does not manufacture external evidence; it prevents local
documentation edits from claiming missing evidence exists or directing the final campaign at the
wrong artifact.

Windows resource and release-binary contracts also bind build provenance. Each enabled executable
must carry the configure-time source revision in its standard signed version resource. The signing
preflight requires a clean exact Git HEAD before binary inspection or certificate access, and
package/final-evidence verification rejects a different embedded revision. Contract fixtures prove
clean acceptance plus wrong-HEAD, tracked-dirty, untracked, binary mismatch, package mismatch, and
renamed-executable rejection. `local-uncommitted` remains valid only for unsigned development work.

Qualification producers fail early on the same boundary. Client qualification verifies the copied
package against its declared revision before extraction or execution. UI qualification compiles the
effective revision into the raster test executable and rejects a different requested revision before
writing any BMP. The UI contract launches the real generator with a wrong revision and proves that
the staging directory remains empty.

Every graph also registers `prediction_free_dogfood_cli_contract`. It proves the CLI main dispatch
contains no eager analyzer construction, permits exactly three explicit pipeline-identity calls,
and limits analyzer construction sites to identity/initialization, prediction-bearing development
inspection, and evaluation. This is a source-boundary guard for blinded annotation and campaign
coordination; the end-to-end acquisition contract separately executes those paths over a real
schema-v1 fixture archive. That CLI contract also completes a truth-review ballot, validates its
incident/operator binding without prediction-bearing output, and proves wrong-incident and
collection-operator ballots are rejected. It then compares agreeing and differing independent
ballots, proves only the disagreement bit changes, and rejects a repeated annotator identity.
Native corpus tests also corrupt session counts, ballot counts, disagreement flags, annotator
identity, and duplicate ballots and require each failure to identify the exact privacy-safe row and
numeric mismatch.

Diagnostic artifact tests serialize the exact V1 prediction/report pair, independently parse the
prediction rows, recompute every metric from frozen truth, and require canonical byte equality. They
exercise metric tampering, sparse-row removal, calibration-fingerprint declaration changes, excess
rows, oversized rows, duplicate contributor ordinals, and corpus-provenance mismatch. Publication
also runs this verifier both before and after its atomic rename; the CLI contract proves a collecting
corpus cannot enter standalone `verify-evaluation`.

Confidence-calibration artifact tests exercise the single offline V1 codec independently of the
CLI. They require exact-precision canonical round trips and verified new-file publication, then
reject alternate numeric spellings, CRLF, reordered fields, inconsistent sample totals, decreasing
knots, missing final LF, oversized lines/files, directories, occupied destinations/staging paths,
and inconsistent assertion state. The direct-v1 source contract additionally requires the non-link
check, canonical rejection path, and sole serializer, preventing a second permissive app reader.

## Property and fuzz scope

The ordinary suite deterministically truncates and mutates the strict product/recorder settings
inputs. Every accepted mutation must still pass the direct-v1 validator. It also presents 128
bounded deterministic random non-database files to the archive and proves rejection does not modify
their bytes.

The native fuzzer calls the same in-memory strict-v1 settings parsers with a 20 KiB input ceiling.
Its checked-in product and recorder seeds are copied into a build-local corpus. CI first runs 2,000
deterministic iterations, then a 60-second ASan-backed campaign with explicit timeout, input, and
memory limits. A short bounded campaign is a regression gate, not a claim of exhaustive fuzzing.

## Coverage meaning

Coverage measures the portable `src/` graph on Linux and excludes Windows-only platform and
telemetry sources that are not compiled there. The initial 60% line and 45% branch floors prevent
silent regression; they are minimum gates, not a statement that the unmeasured Windows paths are
covered. Windows behavior is covered by its ordinary, integration, static-analysis, and ASan jobs.

## Failure policy

Sanitizer findings, analyzer warnings, policy mismatches, malformed/unapproved dependency metadata,
SBOM validation errors, vulnerability-floor violations, CodeQL failures, fuzz crashes/timeouts, and
coverage-floor regressions fail their jobs. Exceptions require a source-reviewed narrow change to
the relevant gate and documentation; broad warning or sanitizer suppression is not the default.
