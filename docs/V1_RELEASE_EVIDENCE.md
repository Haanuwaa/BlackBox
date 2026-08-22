# V1 release evidence composition

V1 is satisfied only when one source revision has both independently passing evidence chains:

1. the V0.17 signed-package, wall-clock, UI/review, physical-client, and hosted-CI aggregate; and
2. the V0.15.1 consented multi-hardware corpus, canonical calibration, and passing one-shot held-out
   result.

Neither chain can substitute for the other. These scripts are downstream release infrastructure;
they do not alter the recorder, corpus, evaluation, package, or any source evidence.

## Required inputs

Retain every source input used to create the V0.17 aggregate, including the signed ZIP, both soak
campaigns, UI raster and explicit review, UI test executable, physical matrix and all client source
bundles, and both hosted attestations. The final verifier re-invokes
`verify-v017-release-evidence.ps1`; the three-file V0.17 hash ledger is not trusted by itself.

Also retain:

- the exact frozen five-file corpus with its complete `heldout-evaluation.lock`;
- the canonical `calibration.tsv` used by the one-shot run;
- the exact two-file held-out output directory; and
- `blackbox_dogfood_tool.exe` extracted from the signed ZIP alongside its packaged dependencies.

The verifier hashes the supplied dogfood executable and requires it to equal the exact
`blackbox_dogfood_tool.exe` ZIP entry already covered by Authenticode verification. A developer
build, differently signed copy, or another package's evaluator is rejected. It also requires all
three signed package executables to embed the requested source revision; agreement among surrounding
evidence manifests cannot hide a package built from another or uncommitted tree.

## Publish the final V1 ledger

After both evidence chains exist for the same lowercase 40-character revision, run:

```powershell
./scripts/run-v1-release-qualification.ps1 `
  -SourceRevision <40-hex-revision> `
  -V017ReleaseEvidenceDirectory C:/qualification/v017-release `
  -PackagePath C:/qualification/BlackBox-1.0.0-windows-x64.zip `
  -OvernightCampaignDirectory C:/qualification/overnight `
  -SeventyTwoHourCampaignDirectory C:/qualification/72-hour `
  -UiEvidenceDirectory C:/qualification/ui-raster `
  -UiReviewDirectory C:/qualification/ui-raster-review `
  -UiTestExecutable C:/qualification/blackbox_tests.exe `
  -ClientMatrixDirectory C:/qualification/client-matrix `
  -ClientEvidenceDirectory C:/qualification/win10-standard, `
                           C:/qualification/win11-standard, `
                           C:/qualification/multimonitor, `
                           C:/qualification/low-end, `
                           C:/qualification/battery `
  -WindowsCiAttestationDirectory C:/qualification/hosted-windows `
  -QualityCiAttestationDirectory C:/qualification/hosted-quality `
  -DogfoodTool C:/qualification/extracted/blackbox_dogfood_tool.exe `
  -FrozenCorpusDirectory C:/qualification/diagnostic-corpus `
  -CalibrationArtifactPath C:/qualification/calibration/calibration.tsv `
  -HeldOutEvaluationDirectory C:/qualification/held-out `
  -OutputDirectory C:/qualification/v1-release
```

The runner refuses occupied final or sibling `.partial` destinations. It writes only
`summary.ini`, `sources.tsv`, and `manifest.sha256.ini`, verifies the staging directory, publishes
with one same-volume rename, and verifies the final directory again.

## Independent verification

Re-run `verify-v1-release-evidence.ps1` with the same arguments and
`-V1EvidenceDirectory C:/qualification/v1-release`. Never use `-AllowStaging` during independent
review.

The verifier:

- independently re-runs the complete V0.17 chain and signed-package verification;
- rejects reuse of one directory across V1 output, V0.17, soak, UI, client, hosted, corpus,
  calibration, and held-out evidence roles;
- requires exact non-link corpus, one-shot lock, and held-out directory contents;
- invokes the exact packaged evaluator's `verify-evaluation`, `heldout-status`, and `fingerprint`
  commands;
- requires a frozen canonical report with `qualification_passed=1`;
- requires the one-shot state to be `complete` and passing;
- requires annotation, configuration, calibration, and recomputed report fingerprints to agree
  between the report and the consumed attempt record; and
- regenerates the complete source hash ledger and checks every final byte.

`local-uncommitted`, an unsigned package, a local V0.17 rehearsal, a failed or running held-out
attempt, a calibration from another corpus/configuration, sparse or tampered predictions, or a
different dogfood executable cannot produce `v1_release_evidence_satisfied=1`.

The final directory is a compact composition ledger, not a replacement for source evidence. Retain
the package, every V0.17 input, corpus, calibration, held-out output, and one-shot lock.
