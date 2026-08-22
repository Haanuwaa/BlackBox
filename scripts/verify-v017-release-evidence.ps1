[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$ReleaseEvidenceDirectory,
    [Parameter(Mandatory = $true)] [ValidatePattern('^[0-9a-f]{40}$')]
    [string]$SourceRevision,
    [Parameter(Mandatory = $true)] [string]$PackagePath,
    [Parameter(Mandatory = $true)] [string]$OvernightCampaignDirectory,
    [Parameter(Mandatory = $true)] [string]$SeventyTwoHourCampaignDirectory,
    [Parameter(Mandatory = $true)] [string]$UiEvidenceDirectory,
    [Parameter(Mandatory = $true)] [string]$UiReviewDirectory,
    [Parameter(Mandatory = $true)] [string]$UiTestExecutable,
    [Parameter(Mandatory = $true)] [string]$ClientMatrixDirectory,
    [Parameter(Mandatory = $true)] [string[]]$ClientEvidenceDirectory,
    [Parameter(Mandatory = $true)] [string]$WindowsCiAttestationDirectory,
    [Parameter(Mandatory = $true)] [string]$QualityCiAttestationDirectory,
    [switch]$AllowStaging
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-DirectV1([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { throw "Missing direct-v1 artifact: $Path" }
    $fields = @{}
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ([string]::IsNullOrWhiteSpace($line)) { throw 'Direct-v1 artifact contains a blank line.' }
        $separator = $line.IndexOf('=')
        if ($separator -lt 1) { throw 'Direct-v1 artifact contains a malformed field.' }
        $name = $line.Substring(0, $separator)
        if ($fields.ContainsKey($name)) { throw "Duplicate direct-v1 field: $name" }
        $fields[$name] = $line.Substring($separator + 1)
    }
    if ($fields['format'] -cne '1') { throw 'Artifact is not direct format v1.' }
    return $fields
}

function Require-Value($Fields, [string]$Name, [string]$Value) {
    if (-not $Fields.ContainsKey($Name) -or $Fields[$Name] -cne $Value) {
        throw "V0.17 release evidence field mismatch: $Name"
    }
}

function Manifest-Hash([string]$Directory) {
    return (Get-FileHash -LiteralPath (Join-Path $Directory 'manifest.sha256.ini') `
                         -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-PackagedApplicationHash([string]$Package) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($Package)
    try {
        $expected = ([IO.Path]::GetFileNameWithoutExtension($Package) + '/blackbox.exe')
        $matches = @($archive.Entries | Where-Object { $_.FullName -ceq $expected })
        if ($matches.Count -ne 1) { throw 'Signed package does not contain one exact blackbox.exe.' }
        $stream = $matches[0].Open()
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            return [Convert]::ToHexString($sha.ComputeHash($stream)).ToLowerInvariant()
        } finally {
            $sha.Dispose()
            $stream.Dispose()
        }
    } finally { $archive.Dispose() }
}

if ($ClientEvidenceDirectory.Count -lt 5) {
    throw 'V0.17 release evidence requires at least five client bundles.'
}
$releaseEvidence = (Resolve-Path -LiteralPath $ReleaseEvidenceDirectory -ErrorAction Stop).Path
if (-not [IO.Directory]::Exists($releaseEvidence) -or
    ([IO.File]::GetAttributes($releaseEvidence) -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
    (-not $AllowStaging.IsPresent -and $releaseEvidence.TrimEnd('\', '/').EndsWith('.partial'))) {
    throw 'V0.17 release evidence must be a published non-link directory.'
}
$entries = @(Get-ChildItem -LiteralPath $releaseEvidence -Force)
$entryDifferences = @(Compare-Object @('manifest.sha256.ini', 'sources.tsv', 'summary.ini') `
    @($entries.Name | Sort-Object) -CaseSensitive -SyncWindow 0)
if ($entries.Count -ne 3 -or @($entries | Where-Object { $_.PSIsContainer }).Count -ne 0 -or
    $entryDifferences.Count -ne 0) {
    throw 'V0.17 release evidence does not have the exact direct-v1 file set.'
}

$package = (Resolve-Path -LiteralPath $PackagePath -ErrorAction Stop).Path
$overnight = (Resolve-Path -LiteralPath $OvernightCampaignDirectory -ErrorAction Stop).Path
$seventyTwoHour = (Resolve-Path -LiteralPath $SeventyTwoHourCampaignDirectory -ErrorAction Stop).Path
$uiEvidence = (Resolve-Path -LiteralPath $UiEvidenceDirectory -ErrorAction Stop).Path
$uiReview = (Resolve-Path -LiteralPath $UiReviewDirectory -ErrorAction Stop).Path
$uiTest = (Resolve-Path -LiteralPath $UiTestExecutable -ErrorAction Stop).Path
$clientMatrix = (Resolve-Path -LiteralPath $ClientMatrixDirectory -ErrorAction Stop).Path
$clientBundles = @($ClientEvidenceDirectory | ForEach-Object {
    (Resolve-Path -LiteralPath $_ -ErrorAction Stop).Path
})
$windowsCi = (Resolve-Path -LiteralPath $WindowsCiAttestationDirectory -ErrorAction Stop).Path
$qualityCi = (Resolve-Path -LiteralPath $QualityCiAttestationDirectory -ErrorAction Stop).Path
$directories = @($overnight, $seventyTwoHour, $uiEvidence, $uiReview, $clientMatrix,
                 $windowsCi, $qualityCi) + $clientBundles
$unique = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($directory in $directories) {
    if (-not $unique.Add($directory)) { throw 'One evidence directory cannot satisfy two V0.17 roles.' }
}

& (Join-Path $PSScriptRoot 'verify-release.ps1') -PackagePath $package `
    -RequireAuthenticode -ExpectedSourceRevision $SourceRevision | Out-Null
& (Join-Path $PSScriptRoot 'verify-wall-clock-soak.ps1') `
    -CampaignDirectory $overnight | Out-Null
& (Join-Path $PSScriptRoot 'verify-wall-clock-soak.ps1') `
    -CampaignDirectory $seventyTwoHour | Out-Null
& (Join-Path $PSScriptRoot 'verify-ui-qualification.ps1') `
    -EvidenceDirectory $uiEvidence -TestExecutable $uiTest `
    -ExpectedSourceRevision $SourceRevision | Out-Null
& (Join-Path $PSScriptRoot 'verify-ui-visual-review.ps1') `
    -ReviewDirectory $uiReview -UiEvidenceDirectory $uiEvidence `
    -ExpectedSourceRevision $SourceRevision | Out-Null
& (Join-Path $PSScriptRoot 'verify-client-matrix-evidence.ps1') `
    -MatrixDirectory $clientMatrix -EvidenceDirectory $clientBundles `
    -ExpectedSourceRevision $SourceRevision -RequireAuthenticode | Out-Null
& (Join-Path $PSScriptRoot 'verify-hosted-ci-attestation.ps1') `
    -AttestationDirectory $windowsCi -WorkflowKey windows `
    -ExpectedSourceRevision $SourceRevision | Out-Null
& (Join-Path $PSScriptRoot 'verify-hosted-ci-attestation.ps1') `
    -AttestationDirectory $qualityCi -WorkflowKey quality `
    -ExpectedSourceRevision $SourceRevision | Out-Null

$overnightSummary = Read-DirectV1 (Join-Path $overnight 'summary.ini')
$seventyTwoSummary = Read-DirectV1 (Join-Path $seventyTwoHour 'summary.ini')
$matrixSummary = Read-DirectV1 (Join-Path $clientMatrix 'summary.ini')
$windowsSummary = Read-DirectV1 (Join-Path $windowsCi 'summary.ini')
$qualitySummary = Read-DirectV1 (Join-Path $qualityCi 'summary.ini')
foreach ($pair in @(@($overnightSummary, 'overnight'), @($seventyTwoSummary, '72-hour'))) {
    Require-Value $pair[0] 'mode' $pair[1]
    Require-Value $pair[0] 'source_revision' $SourceRevision
}
$packageHash = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant()
$applicationHash = Get-PackagedApplicationHash $package
Require-Value $overnightSummary 'application_sha256' $applicationHash
Require-Value $seventyTwoSummary 'application_sha256' $applicationHash
foreach ($pair in @(
    @('source_revision', $SourceRevision),
    @('package_name', [IO.Path]::GetFileName($package)),
    @('package_sha256', $packageHash), @('clean_client_matrix_satisfied', '1'),
    @('physical_matrix_satisfied', '1'), @('official_signed_matrix_satisfied', '1'))) {
    Require-Value $matrixSummary $pair[0] $pair[1]
}
if ($windowsSummary['repository'] -cne $qualitySummary['repository'] -or
    $windowsSummary['run_id'] -ceq $qualitySummary['run_id']) {
    throw 'Hosted workflow attestations must bind one repository and distinct successful runs.'
}

$summary = Read-DirectV1 (Join-Path $releaseEvidence 'summary.ini')
$runnerHash = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'run-v017-release-qualification.ps1') `
                              -Algorithm SHA256).Hash.ToLowerInvariant()
$verifierHash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
$expectedFields = @(
    @('state', 'passed'), @('source_revision', $SourceRevision),
    @('repository', $windowsSummary['repository']),
    @('package_name', [IO.Path]::GetFileName($package)), @('package_sha256', $packageHash),
    @('application_sha256', $applicationHash),
    @('client_bundle_count', [string]$clientBundles.Count), @('overnight_soak_satisfied', '1'),
    @('seventy_two_hour_soak_satisfied', '1'), @('ui_raster_satisfied', '1'),
    @('ui_visual_review_satisfied', '1'), @('clean_client_matrix_satisfied', '1'),
    @('physical_matrix_satisfied', '1'), @('hosted_windows_satisfied', '1'),
    @('hosted_quality_satisfied', '1'), @('official_signed_package_satisfied', '1'),
    @('v017_release_evidence_satisfied', '1'), @('runner_sha256', $runnerHash),
    @('verifier_sha256', $verifierHash)
)
foreach ($pair in $expectedFields) { Require-Value $summary $pair[0] $pair[1] }
if ($summary.Count -ne 20) { throw 'V0.17 release summary contains unexpected fields.' }

$sourceRows = @(
    "package`t$packageHash",
    "overnight_soak`t$(Manifest-Hash $overnight)",
    "seventy_two_hour_soak`t$(Manifest-Hash $seventyTwoHour)",
    "ui_raster`t$(Manifest-Hash $uiEvidence)",
    "ui_visual_review`t$(Manifest-Hash $uiReview)",
    "ui_test_executable`t$((Get-FileHash $uiTest -Algorithm SHA256).Hash.ToLowerInvariant())",
    "client_matrix`t$(Manifest-Hash $clientMatrix)",
    "hosted_windows`t$(Manifest-Hash $windowsCi)",
    "hosted_quality`t$(Manifest-Hash $qualityCi)"
)
$sourceRows += @($clientBundles | ForEach-Object { "client_bundle`t$(Manifest-Hash $_)" })
$expectedSources = ((@("kind`tsha256") + @($sourceRows | Sort-Object)) -join "`r`n") + "`r`n"
if ([IO.File]::ReadAllText((Join-Path $releaseEvidence 'sources.tsv')) -cne $expectedSources) {
    throw 'V0.17 release source ledger does not match the independently verified inputs.'
}
$manifest = Read-DirectV1 (Join-Path $releaseEvidence 'manifest.sha256.ini')
Require-Value $manifest 'algorithm' 'sha256'
Require-Value $manifest 'file_count' '2'
if ($manifest.Count -ne 5) { throw 'V0.17 release manifest field count is invalid.' }
foreach ($relative in @('sources.tsv', 'summary.ini')) {
    Require-Value $manifest $relative (
        (Get-FileHash -LiteralPath (Join-Path $releaseEvidence $relative) `
                      -Algorithm SHA256).Hash.ToLowerInvariant())
}
Write-Output "Verified complete V0.17 release evidence: $releaseEvidence"
