# V0.17 release evidence chain

V0.17 is a same-revision trust gate, not a feature toggle and not a V1 certificate. Its aggregate
attestation can exist only after the signed package, wall-clock campaigns, UI review, physical client
matrix, and hosted workflows independently pass for one lowercase 40-character source revision.
V1 additionally requires the separate V0.15.1 diagnostic-quality corpus and held-out gate.

## Hosted workflow evidence

The `hosted-evidence` jobs in `windows.yml` and `quality.yml` run only for `push` or
`workflow_dispatch`, after every required upstream job succeeds. A pull-request run cannot publish
release evidence. Each job calls `write-hosted-ci-attestation.ps1` inside GitHub Actions and uploads
an exact two-file direct-v1 artifact named for `github.sha`.

After downloading both artifacts without editing them, verify their workflow identity, run identity,
writer hash, manifest, and revision:

```powershell
./scripts/verify-hosted-ci-attestation.ps1 `
  -AttestationDirectory C:/qualification/hosted-windows `
  -WorkflowKey windows -ExpectedSourceRevision <40-hex-revision>
./scripts/verify-hosted-ci-attestation.ps1 `
  -AttestationDirectory C:/qualification/hosted-quality `
  -WorkflowKey quality -ExpectedSourceRevision <40-hex-revision>
```

The attestation proves that the named aggregate workflow job became eligible after its declared
dependencies passed. It is not produced locally, by pull requests, or after an upstream failure.

## UI raster and explicit review

Generate and independently verify a new raster bundle from the release revision:

```powershell
./scripts/run-ui-qualification.ps1 `
  -OutputDirectory C:/qualification/ui-raster `
  -SourceRevision <40-hex-revision>
./scripts/verify-ui-qualification.ps1 `
  -EvidenceDirectory C:/qualification/ui-raster `
  -TestExecutable ./out/build/windows-vs2026-release/tests/Release/blackbox_tests.exe `
  -ExpectedSourceRevision <40-hex-revision>
```

The verifier streams every exact V5 BMP header, checks its complete payload length and hash, binds
`summary.ini`, and rechecks source/test/runner/verifier provenance. It rejects links, extras, partial
directories, and old unbound bundles.

After one reviewer actually checks all 32 images at native pixel size, record a pseudonymous
attestation. The confirmation switch is intentionally explicit:

```powershell
./scripts/record-ui-visual-review.ps1 `
  -EvidenceDirectory C:/qualification/ui-raster `
  -OutputDirectory C:/qualification/ui-raster-review `
  -ReviewerId reviewer-1 -SourceRevision <40-hex-revision> `
  -ConfirmAllCasesPassed
./scripts/verify-ui-visual-review.ps1 `
  -ReviewDirectory C:/qualification/ui-raster-review `
  -UiEvidenceDirectory C:/qualification/ui-raster `
  -ExpectedSourceRevision <40-hex-revision>
```

Review attestation binds the exact raster manifest. It does not satisfy real keyboard, DPI,
multi-monitor, high-contrast transition, low-end, battery, or power-mode testing.

## Client matrix regeneration

Retain the five or more original interactive client bundles alongside the three-file matrix. Later
review deterministically regenerates the matrix from those bundles and requires byte-for-byte
agreement:

```powershell
./scripts/verify-client-matrix-evidence.ps1 `
  -MatrixDirectory C:/qualification/client-matrix `
  -EvidenceDirectory C:/qualification/win10-standard, `
                     C:/qualification/win11-standard, `
                     C:/qualification/multimonitor, `
                     C:/qualification/low-end, `
                     C:/qualification/battery `
  -ExpectedSourceRevision <40-hex-revision> -RequireAuthenticode
```

## Aggregate V0.17 gate

Only after all inputs exist, publish the aggregate evidence directory:

```powershell
./scripts/run-v017-release-qualification.ps1 `
  -SourceRevision <40-hex-revision> `
  -PackagePath C:/qualification/BlackBox-1.0.0-windows-x64.zip `
  -OvernightCampaignDirectory C:/qualification/overnight `
  -SeventyTwoHourCampaignDirectory C:/qualification/72-hour `
  -UiEvidenceDirectory C:/qualification/ui-raster `
  -UiReviewDirectory C:/qualification/ui-raster-review `
  -UiTestExecutable ./out/build/windows-vs2026-release/tests/Release/blackbox_tests.exe `
  -ClientMatrixDirectory C:/qualification/client-matrix `
  -ClientEvidenceDirectory C:/qualification/win10-standard, `
                           C:/qualification/win11-standard, `
                           C:/qualification/multimonitor, `
                           C:/qualification/low-end, `
                           C:/qualification/battery `
  -WindowsCiAttestationDirectory C:/qualification/hosted-windows `
  -QualityCiAttestationDirectory C:/qualification/hosted-quality `
  -OutputDirectory C:/qualification/v017-release
```

The runner re-invokes every independent verifier, requires valid timestamped Authenticode, rejects
role reuse, checks the two soak modes, binds the physical matrix to the exact package, requires two
distinct successful hosted runs for one repository, and publishes an exact hash ledger atomically.
It also requires all three signed package executables to embed the requested source revision in
their Windows identity; matching evidence text cannot relabel a package built from another tree.
`local-uncommitted`, an unsigned rehearsal, a local smoke, a partial campaign, or authored-but-unrun
workflow cannot produce `v017_release_evidence_satisfied=1`.

Re-run `verify-v017-release-evidence.ps1` with the same inputs before relying on the final directory.
The aggregate artifact does not embed or replace its source evidence; retain every bound input.
