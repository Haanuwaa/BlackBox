[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$V1EvidenceDirectory,
    [Parameter(Mandatory = $true)] [ValidatePattern('^[0-9a-f]{40}$')]
    [string]$SourceRevision,
    [Parameter(Mandatory = $true)] [string]$V017ReleaseEvidenceDirectory,
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
    [Parameter(Mandatory = $true)] [string]$DogfoodTool,
    [Parameter(Mandatory = $true)] [string]$FrozenCorpusDirectory,
    [Parameter(Mandatory = $true)] [string]$CalibrationArtifactPath,
    [Parameter(Mandatory = $true)] [string]$HeldOutEvaluationDirectory,
    [switch]$AllowStaging
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-NonLinkFile([string]$Path, [long]$MaximumBytes = 67108864) {
    if (-not [IO.File]::Exists($Path)) { throw "Missing V1 evidence input: $Path" }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        $item.PSIsContainer -or $item.Length -le 0 -or $item.Length -gt $MaximumBytes) {
        throw "V1 evidence input is linked, empty, oversized, or not a regular file: $Path"
    }
}

function Assert-PublishedDirectory([string]$Path, [bool]$PermitStaging = $false) {
    if (-not [IO.Directory]::Exists($Path)) { throw "Missing V1 evidence directory: $Path" }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        (-not $PermitStaging -and $Path.TrimEnd('\', '/').EndsWith('.partial'))) {
        throw "V1 evidence input must be a published non-link directory: $Path"
    }
}

function Assert-ExactEntries([string]$Directory, [string[]]$Names,
                             [string[]]$DirectoryNames = @()) {
    $entries = @(Get-ChildItem -LiteralPath $Directory -Force)
    $expected = @($Names + $DirectoryNames | Sort-Object)
    $differences = @(Compare-Object $expected @($entries.Name | Sort-Object) `
        -CaseSensitive -SyncWindow 0)
    if ($entries.Count -ne $expected.Count -or $differences.Count -ne 0) {
        throw "V1 evidence directory has an unexpected direct-v1 entry: $Directory"
    }
    foreach ($entry in $entries) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "V1 evidence directory contains a link: $Directory"
        }
        $shouldBeDirectory = $DirectoryNames -ccontains $entry.Name
        if ($entry.PSIsContainer -ne $shouldBeDirectory) {
            throw "V1 evidence entry has the wrong type: $($entry.Name)"
        }
    }
}

function Read-CanonicalFields([string]$Path, [string]$ExpectedFormat = '1') {
    Assert-NonLinkFile $Path 65536
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes[$bytes.Length - 1] -ne 10 -or $bytes -contains 13) {
        throw "Direct-v1 artifact is not canonical LF text: $Path"
    }
    $text = [Text.UTF8Encoding]::new($false, $true).GetString($bytes)
    $lines = $text.Substring(0, $text.Length - 1).Split("`n")
    $fields = @{}
    foreach ($line in $lines) {
        if ([string]::IsNullOrWhiteSpace($line)) { throw "Blank direct-v1 line: $Path" }
        $separator = $line.IndexOf('=')
        if ($separator -lt 1) { throw "Malformed direct-v1 line: $Path" }
        $name = $line.Substring(0, $separator)
        if ($fields.ContainsKey($name)) { throw "Duplicate direct-v1 field: $name" }
        $fields[$name] = $line.Substring($separator + 1)
    }
    if ($fields['format'] -cne $ExpectedFormat) {
        throw "Unexpected direct-v1 format: $Path"
    }
    return $fields
}

function Require-Value($Fields, [string]$Name, [string]$Value) {
    if (-not $Fields.ContainsKey($Name) -or $Fields[$Name] -cne $Value) {
        throw "V1 release evidence field mismatch: $Name"
    }
}

function Require-PositiveInteger($Fields, [string]$Name) {
    if (-not $Fields.ContainsKey($Name) -or $Fields[$Name] -notmatch '^[1-9][0-9]*$') {
        throw "V1 release evidence requires a positive integer: $Name"
    }
}

function Invoke-DogfoodFields([string]$Tool, [string[]]$Arguments) {
    $lines = @(& $Tool @Arguments)
    if ($LASTEXITCODE -ne 0) {
        throw "Packaged dogfood verifier command failed: $($Arguments[0])"
    }
    $fields = @{}
    foreach ($lineValue in $lines) {
        $line = [string]$lineValue
        $separator = $line.IndexOf('=')
        if ($separator -lt 1) { throw 'Packaged dogfood verifier emitted a malformed line.' }
        $name = $line.Substring(0, $separator)
        if ($fields.ContainsKey($name)) { throw "Duplicate dogfood verifier field: $name" }
        $fields[$name] = $line.Substring($separator + 1)
    }
    return $fields
}

function Get-ZipEntryHash([string]$Package, [string]$LeafName) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($Package)
    try {
        $expected = ([IO.Path]::GetFileNameWithoutExtension($Package) + '/' + $LeafName)
        $matches = @($archive.Entries | Where-Object { $_.FullName -ceq $expected })
        if ($matches.Count -ne 1) { throw "Package lacks one exact $LeafName entry." }
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

function File-Hash([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

$v1Evidence = (Resolve-Path -LiteralPath $V1EvidenceDirectory -ErrorAction Stop).Path
Assert-PublishedDirectory $v1Evidence $AllowStaging.IsPresent
Assert-ExactEntries $v1Evidence @('manifest.sha256.ini', 'sources.tsv', 'summary.ini')

$v017 = (Resolve-Path -LiteralPath $V017ReleaseEvidenceDirectory -ErrorAction Stop).Path
$package = (Resolve-Path -LiteralPath $PackagePath -ErrorAction Stop).Path
$finalProductVersion = '1.0.0'
$expectedPackageName = "BlackBox-$finalProductVersion-windows-x64.zip"
if ([IO.Path]::GetFileName($package) -cne $expectedPackageName) {
    throw "Final V1 evidence requires the exact $expectedPackageName package."
}
$dogfood = (Resolve-Path -LiteralPath $DogfoodTool -ErrorAction Stop).Path
$corpus = (Resolve-Path -LiteralPath $FrozenCorpusDirectory -ErrorAction Stop).Path
$calibration = (Resolve-Path -LiteralPath $CalibrationArtifactPath -ErrorAction Stop).Path
$heldOut = (Resolve-Path -LiteralPath $HeldOutEvaluationDirectory -ErrorAction Stop).Path
$overnight = (Resolve-Path -LiteralPath $OvernightCampaignDirectory -ErrorAction Stop).Path
$seventyTwoHour = (Resolve-Path -LiteralPath $SeventyTwoHourCampaignDirectory -ErrorAction Stop).Path
$uiEvidence = (Resolve-Path -LiteralPath $UiEvidenceDirectory -ErrorAction Stop).Path
$uiReview = (Resolve-Path -LiteralPath $UiReviewDirectory -ErrorAction Stop).Path
$clientMatrix = (Resolve-Path -LiteralPath $ClientMatrixDirectory -ErrorAction Stop).Path
$clientBundles = @($ClientEvidenceDirectory | ForEach-Object {
    (Resolve-Path -LiteralPath $_ -ErrorAction Stop).Path
})
$windowsCi = (Resolve-Path -LiteralPath $WindowsCiAttestationDirectory -ErrorAction Stop).Path
$qualityCi = (Resolve-Path -LiteralPath $QualityCiAttestationDirectory -ErrorAction Stop).Path
$calibrationDirectory = [IO.Path]::GetDirectoryName($calibration)
$roleDirectories = @(
    $v1Evidence, $v017, $overnight, $seventyTwoHour, $uiEvidence, $uiReview,
    $clientMatrix, $windowsCi, $qualityCi, $corpus, $calibrationDirectory, $heldOut
) + $clientBundles
$uniqueRoleDirectories =
    [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($directory in $roleDirectories) {
    if (-not $uniqueRoleDirectories.Add($directory)) {
        throw 'One evidence directory cannot satisfy two V1 release roles.'
    }
}
Assert-PublishedDirectory $v017
Assert-NonLinkFile $package
Assert-NonLinkFile $dogfood
Assert-PublishedDirectory $corpus
Assert-NonLinkFile $calibration 65536
Assert-PublishedDirectory $heldOut
Assert-ExactEntries $corpus @('annotations.tsv', 'hardware.tsv', 'incidents.tsv',
    'manifest.ini', 'sessions.tsv') @('heldout-evaluation.lock')
$lock = Join-Path $corpus 'heldout-evaluation.lock'
Assert-PublishedDirectory $lock
Assert-ExactEntries $lock @('attempt.ini', 'result.ini')
Assert-ExactEntries $heldOut @('evaluation.json', 'predictions.tsv')

foreach ($campaignDirectory in @($overnight, $seventyTwoHour)) {
    $applicationReport = Read-CanonicalFields (Join-Path $campaignDirectory 'app-report.ini')
    Require-Value $applicationReport 'application_version' $finalProductVersion
}

& (Join-Path $PSScriptRoot 'verify-v017-release-evidence.ps1') `
    -ReleaseEvidenceDirectory $v017 -SourceRevision $SourceRevision `
    -PackagePath $package -OvernightCampaignDirectory $overnight `
    -SeventyTwoHourCampaignDirectory $seventyTwoHour `
    -UiEvidenceDirectory $uiEvidence -UiReviewDirectory $uiReview `
    -UiTestExecutable $UiTestExecutable -ClientMatrixDirectory $ClientMatrixDirectory `
    -ClientEvidenceDirectory $clientBundles `
    -WindowsCiAttestationDirectory $windowsCi `
    -QualityCiAttestationDirectory $qualityCi | Out-Null

& (Join-Path $PSScriptRoot 'verify-release.ps1') `
    -PackagePath $package -RequireAuthenticode `
    -ExpectedVersion $finalProductVersion `
    -ExpectedSourceRevision $SourceRevision | Out-Null

$packageToolHash = Get-ZipEntryHash $package 'blackbox_dogfood_tool.exe'
$dogfoodHash = File-Hash $dogfood
if ($dogfoodHash -cne $packageToolHash) {
    throw 'Dogfood verifier is not the exact signed executable from the release package.'
}
$evaluation = Invoke-DogfoodFields $dogfood @(
    'verify-evaluation', $corpus, $heldOut, $calibration)
$status = Invoke-DogfoodFields $dogfood @('heldout-status', $corpus)
$pipeline = Invoke-DogfoodFields $dogfood @('fingerprint')
if ($evaluation.Count -ne 10 -or $status.Count -ne 6 -or $pipeline.Count -ne 2) {
    throw 'Packaged dogfood verifier emitted an unexpected field set.'
}
foreach ($pair in @(
    @('artifact_valid', '1'), @('format', '1'), @('split', 'held_out'),
    @('qualification_passed', '1'))) {
    Require-Value $evaluation $pair[0] $pair[1]
}
foreach ($name in @('prediction_rows', 'truth_rows', 'annotation_fingerprint',
                    'configuration_fingerprint', 'calibration_artifact_fingerprint',
                    'report_artifact_fingerprint')) {
    Require-PositiveInteger $evaluation $name
}
Require-Value $status 'state' 'complete'
Require-Value $status 'qualification_passed' '1'
foreach ($name in @('annotation_fingerprint', 'configuration_fingerprint',
                    'calibration_artifact_fingerprint', 'report_artifact_fingerprint')) {
    Require-Value $status $name $evaluation[$name]
}
Require-Value $pipeline 'configuration_fingerprint' $evaluation['configuration_fingerprint']
Require-PositiveInteger $pipeline 'pipeline_version'

$v017Summary = Read-CanonicalFields (Join-Path $v017 'summary.ini')
Require-Value $v017Summary 'source_revision' $SourceRevision
Require-Value $v017Summary 'package_sha256' (File-Hash $package)
Require-Value $v017Summary 'v017_release_evidence_satisfied' '1'

$summary = Read-CanonicalFields (Join-Path $v1Evidence 'summary.ini')
$runnerHash = File-Hash (Join-Path $PSScriptRoot 'run-v1-release-qualification.ps1')
$verifierHash = File-Hash $PSCommandPath
$expectedFields = @(
    @('state', 'passed'), @('source_revision', $SourceRevision),
    @('repository', $v017Summary['repository']),
    @('product_version', $finalProductVersion),
    @('package_name', [IO.Path]::GetFileName($package)),
    @('package_sha256', (File-Hash $package)),
    @('v017_release_manifest_sha256', (File-Hash (Join-Path $v017 'manifest.sha256.ini'))),
    @('dogfood_tool_sha256', $dogfoodHash),
    @('pipeline_version', $pipeline['pipeline_version']),
    @('configuration_fingerprint', $evaluation['configuration_fingerprint']),
    @('annotation_fingerprint', $evaluation['annotation_fingerprint']),
    @('calibration_artifact_fingerprint', $evaluation['calibration_artifact_fingerprint']),
    @('report_artifact_fingerprint', $evaluation['report_artifact_fingerprint']),
    @('truth_rows', $evaluation['truth_rows']),
    @('prediction_rows', $evaluation['prediction_rows']),
    @('diagnostic_qualification_satisfied', '1'),
    @('v017_release_evidence_satisfied', '1'), @('v1_release_evidence_satisfied', '1'),
    @('runner_sha256', $runnerHash), @('verifier_sha256', $verifierHash)
)
foreach ($pair in $expectedFields) { Require-Value $summary $pair[0] $pair[1] }
if ($summary.Count -ne 21) { throw 'V1 release summary contains unexpected fields.' }

$sourceRows = @(
    "package`t$(File-Hash $package)",
    "v017_release_evidence`t$(File-Hash (Join-Path $v017 'manifest.sha256.ini'))",
    "dogfood_tool`t$dogfoodHash",
    "calibration_artifact`t$(File-Hash $calibration)",
    "corpus_manifest`t$(File-Hash (Join-Path $corpus 'manifest.ini'))",
    "corpus_hardware`t$(File-Hash (Join-Path $corpus 'hardware.tsv'))",
    "corpus_sessions`t$(File-Hash (Join-Path $corpus 'sessions.tsv'))",
    "corpus_incidents`t$(File-Hash (Join-Path $corpus 'incidents.tsv'))",
    "corpus_annotations`t$(File-Hash (Join-Path $corpus 'annotations.tsv'))",
    "heldout_attempt`t$(File-Hash (Join-Path $lock 'attempt.ini'))",
    "heldout_result`t$(File-Hash (Join-Path $lock 'result.ini'))",
    "heldout_evaluation`t$(File-Hash (Join-Path $heldOut 'evaluation.json'))",
    "heldout_predictions`t$(File-Hash (Join-Path $heldOut 'predictions.tsv'))"
)
$expectedSources = ((@("kind`tsha256") + @($sourceRows | Sort-Object)) -join "`n") + "`n"
if ([IO.File]::ReadAllText((Join-Path $v1Evidence 'sources.tsv')) -cne $expectedSources) {
    throw 'V1 source ledger does not match the independently verified inputs.'
}
$manifest = Read-CanonicalFields (Join-Path $v1Evidence 'manifest.sha256.ini')
Require-Value $manifest 'algorithm' 'sha256'
Require-Value $manifest 'file_count' '2'
if ($manifest.Count -ne 5) { throw 'V1 manifest field count is invalid.' }
foreach ($relative in @('sources.tsv', 'summary.ini')) {
    Require-Value $manifest $relative (File-Hash (Join-Path $v1Evidence $relative))
}
Write-Output "Verified complete V1 release evidence: $v1Evidence"
