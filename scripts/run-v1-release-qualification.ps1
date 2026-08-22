[CmdletBinding()]
param(
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
    [Parameter(Mandatory = $true)] [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-CanonicalFields([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -eq 0 -or $bytes[$bytes.Length - 1] -ne 10 -or
        $bytes -contains 13 -or $bytes.Length -gt 65536) {
        throw "Direct-v1 input is empty, oversized, or noncanonical: $Path"
    }
    $text = [Text.UTF8Encoding]::new($false, $true).GetString($bytes)
    $fields = @{}
    foreach ($line in $text.Substring(0, $text.Length - 1).Split("`n")) {
        $separator = $line.IndexOf('=')
        if ($separator -lt 1) { throw "Malformed direct-v1 line: $Path" }
        $name = $line.Substring(0, $separator)
        if ($fields.ContainsKey($name)) { throw "Duplicate direct-v1 field: $name" }
        $fields[$name] = $line.Substring($separator + 1)
    }
    if ($fields['format'] -cne '1') { throw "Input is not direct format v1: $Path" }
    return $fields
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

function Write-AtomicText([string]$Path, [string]$Contents) {
    $temporary = "$Path.tmp"
    [IO.File]::WriteAllText($temporary, $Contents, [Text.UTF8Encoding]::new($false))
    [IO.File]::Move($temporary, $Path)
}

function File-Hash([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

$output = [IO.Path]::GetFullPath($OutputDirectory)
$staging = "$output.partial"
if ([IO.Directory]::Exists($output) -or [IO.File]::Exists($output) -or
    [IO.Directory]::Exists($staging) -or [IO.File]::Exists($staging)) {
    throw 'V1 release evidence output and staging destinations must not exist.'
}
if ($ClientEvidenceDirectory.Count -lt 5) {
    throw 'V1 release evidence requires the complete V0.17 client bundle set.'
}

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
$lock = Join-Path $corpus 'heldout-evaluation.lock'
$v017Summary = Read-CanonicalFields (Join-Path $v017 'summary.ini')
if ($v017Summary['source_revision'] -cne $SourceRevision -or
    $v017Summary['v017_release_evidence_satisfied'] -cne '1') {
    throw 'V0.17 evidence does not bind the requested passing source revision.'
}
$evaluation = Invoke-DogfoodFields $dogfood @(
    'verify-evaluation', $corpus, $heldOut, $calibration)
$status = Invoke-DogfoodFields $dogfood @('heldout-status', $corpus)
$pipeline = Invoke-DogfoodFields $dogfood @('fingerprint')
if ($evaluation['qualification_passed'] -cne '1' -or
    $status['state'] -cne 'complete' -or $status['qualification_passed'] -cne '1' -or
    $evaluation['report_artifact_fingerprint'] -cne
        $status['report_artifact_fingerprint']) {
    throw 'Diagnostic evidence is not one complete passing one-shot result.'
}

$runner = [IO.Path]::GetFullPath($PSCommandPath)
$verifier = Join-Path $PSScriptRoot 'verify-v1-release-evidence.ps1'
$packageHash = File-Hash $package
$dogfoodHash = File-Hash $dogfood
$sourceRows = @(
    "package`t$packageHash",
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
$sources = @("kind`tsha256") + @($sourceRows | Sort-Object)
$summary = @(
    'format=1', 'state=passed', "source_revision=$SourceRevision",
    "repository=$($v017Summary['repository'])",
    "product_version=$finalProductVersion",
    "package_name=$([IO.Path]::GetFileName($package))", "package_sha256=$packageHash",
    "v017_release_manifest_sha256=$(File-Hash (Join-Path $v017 'manifest.sha256.ini'))",
    "dogfood_tool_sha256=$dogfoodHash", "pipeline_version=$($pipeline['pipeline_version'])",
    "configuration_fingerprint=$($evaluation['configuration_fingerprint'])",
    "annotation_fingerprint=$($evaluation['annotation_fingerprint'])",
    "calibration_artifact_fingerprint=$($evaluation['calibration_artifact_fingerprint'])",
    "report_artifact_fingerprint=$($evaluation['report_artifact_fingerprint'])",
    "truth_rows=$($evaluation['truth_rows'])", "prediction_rows=$($evaluation['prediction_rows'])",
    'diagnostic_qualification_satisfied=1', 'v017_release_evidence_satisfied=1',
    'v1_release_evidence_satisfied=1', "runner_sha256=$(File-Hash $runner)",
    "verifier_sha256=$(File-Hash $verifier)"
)

[IO.Directory]::CreateDirectory($staging) | Out-Null
try {
    Write-AtomicText (Join-Path $staging 'sources.tsv') (($sources -join "`n") + "`n")
    Write-AtomicText (Join-Path $staging 'summary.ini') (($summary -join "`n") + "`n")
    $manifest = @('format=1', 'algorithm=sha256', 'file_count=2')
    foreach ($relative in @('sources.tsv', 'summary.ini')) {
        $manifest += "$relative=$(File-Hash (Join-Path $staging $relative))"
    }
    Write-AtomicText (Join-Path $staging 'manifest.sha256.ini') (($manifest -join "`n") + "`n")
    $verificationArguments = @{
        SourceRevision = $SourceRevision
        V017ReleaseEvidenceDirectory = $v017
        PackagePath = $package
        OvernightCampaignDirectory = $OvernightCampaignDirectory
        SeventyTwoHourCampaignDirectory = $SeventyTwoHourCampaignDirectory
        UiEvidenceDirectory = $UiEvidenceDirectory
        UiReviewDirectory = $UiReviewDirectory
        UiTestExecutable = $UiTestExecutable
        ClientMatrixDirectory = $ClientMatrixDirectory
        ClientEvidenceDirectory = $ClientEvidenceDirectory
        WindowsCiAttestationDirectory = $WindowsCiAttestationDirectory
        QualityCiAttestationDirectory = $QualityCiAttestationDirectory
        DogfoodTool = $dogfood
        FrozenCorpusDirectory = $corpus
        CalibrationArtifactPath = $calibration
        HeldOutEvaluationDirectory = $heldOut
    }
    & $verifier -V1EvidenceDirectory $staging @verificationArguments -AllowStaging | Out-Null
    [IO.Directory]::Move($staging, $output)
    & $verifier -V1EvidenceDirectory $output @verificationArguments | Out-Null
    Write-Output "V1 release evidence verified and published: $output"
} catch {
    throw
}
