[CmdletBinding()]
param(
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
    [Parameter(Mandatory = $true)] [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-DirectV1([string]$Path) {
    $fields = @{}
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ([string]::IsNullOrWhiteSpace($line)) { throw "Blank direct-v1 line: $Path" }
        $separator = $line.IndexOf('=')
        if ($separator -lt 1) { throw "Malformed direct-v1 line: $Path" }
        $name = $line.Substring(0, $separator)
        if ($fields.ContainsKey($name)) { throw "Duplicate direct-v1 field: $name" }
        $fields[$name] = $line.Substring($separator + 1)
    }
    if ($fields['format'] -cne '1') { throw "Artifact is not direct format v1: $Path" }
    return $fields
}

function Write-AtomicText([string]$Path, [string]$Contents) {
    $temporary = "$Path.tmp"
    [IO.File]::WriteAllText($temporary, $Contents, [Text.UTF8Encoding]::new($false))
    [IO.File]::Move($temporary, $Path)
}

function Manifest-Hash([string]$Directory) {
    return (Get-FileHash -LiteralPath (Join-Path $Directory 'manifest.sha256.ini') `
                         -Algorithm SHA256).Hash.ToLowerInvariant()
}

$output = [IO.Path]::GetFullPath($OutputDirectory)
$staging = "$output.partial"
if ([IO.Directory]::Exists($output) -or [IO.File]::Exists($output) -or
    [IO.Directory]::Exists($staging) -or [IO.File]::Exists($staging)) {
    throw 'V0.17 release evidence output and staging destinations must not exist.'
}
if ($ClientEvidenceDirectory.Count -lt 5) {
    throw 'V0.17 release evidence requires at least five client bundles.'
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
$matrixSummary = Read-DirectV1 (Join-Path $clientMatrix 'summary.ini')
$firstClientSummary = Read-DirectV1 (Join-Path $clientBundles[0] 'summary.ini')
$windowsSummary = Read-DirectV1 (Join-Path $windowsCi 'summary.ini')
$runner = [IO.Path]::GetFullPath($PSCommandPath)
$verifier = Join-Path $PSScriptRoot 'verify-v017-release-evidence.ps1'
$runnerHash = (Get-FileHash -LiteralPath $runner -Algorithm SHA256).Hash.ToLowerInvariant()
$verifierHash = (Get-FileHash -LiteralPath $verifier -Algorithm SHA256).Hash.ToLowerInvariant()
$packageHash = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant()

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
$sources = @("kind`tsha256") + @($sourceRows | Sort-Object)
$summary = @(
    'format=1', 'state=passed', "source_revision=$SourceRevision",
    "repository=$($windowsSummary['repository'])",
    "package_name=$([IO.Path]::GetFileName($package))", "package_sha256=$packageHash",
    "application_sha256=$($firstClientSummary['application_sha256'])",
    "client_bundle_count=$($clientBundles.Count)", 'overnight_soak_satisfied=1',
    'seventy_two_hour_soak_satisfied=1', 'ui_raster_satisfied=1',
    'ui_visual_review_satisfied=1', 'clean_client_matrix_satisfied=1',
    'physical_matrix_satisfied=1', 'hosted_windows_satisfied=1',
    'hosted_quality_satisfied=1', 'official_signed_package_satisfied=1',
    'v017_release_evidence_satisfied=1', "runner_sha256=$runnerHash",
    "verifier_sha256=$verifierHash"
)

[IO.Directory]::CreateDirectory($staging) | Out-Null
try {
    Write-AtomicText (Join-Path $staging 'sources.tsv') (($sources -join "`r`n") + "`r`n")
    Write-AtomicText (Join-Path $staging 'summary.ini') (($summary -join "`n") + "`n")
    $manifest = @('format=1', 'algorithm=sha256', 'file_count=2')
    foreach ($relative in @('sources.tsv', 'summary.ini')) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $staging $relative) `
                              -Algorithm SHA256).Hash.ToLowerInvariant()
        $manifest += "$relative=$hash"
    }
    Write-AtomicText (Join-Path $staging 'manifest.sha256.ini') (($manifest -join "`n") + "`n")
    & $verifier -ReleaseEvidenceDirectory $staging -SourceRevision $SourceRevision `
        -PackagePath $package -OvernightCampaignDirectory $overnight `
        -SeventyTwoHourCampaignDirectory $seventyTwoHour -UiEvidenceDirectory $uiEvidence `
        -UiReviewDirectory $uiReview -UiTestExecutable $uiTest `
        -ClientMatrixDirectory $clientMatrix -ClientEvidenceDirectory $clientBundles `
        -WindowsCiAttestationDirectory $windowsCi -QualityCiAttestationDirectory $qualityCi `
        -AllowStaging | Out-Null
    [IO.Directory]::Move($staging, $output)
    Write-Output "V0.17 release evidence verified and published: $output"
} catch {
    throw
}
