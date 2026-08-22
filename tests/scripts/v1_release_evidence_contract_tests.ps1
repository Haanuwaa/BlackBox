[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Expect-Failure([scriptblock]$Action, [string]$Name) {
    $failed = $false
    try { & $Action } catch { $failed = $true }
    if (-not $failed) { throw "Expected rejection: $Name" }
    Write-Output "Expected rejection: $Name"
}

$runner = Join-Path $SourceRoot 'scripts\run-v1-release-qualification.ps1'
$verifier = Join-Path $SourceRoot 'scripts\verify-v1-release-evidence.ps1'
foreach ($script in @($runner, $verifier)) {
    $tokens = $null
    $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile(
        $script, [ref]$tokens, [ref]$errors)
    if ($errors.Count -ne 0) { throw "PowerShell parser rejected $script" }
}

$runnerText = [IO.File]::ReadAllText($runner)
foreach ($clause in @(
    "[ValidatePattern('^[0-9a-f]{40}$')]",
    "`$finalProductVersion = '1.0.0'",
    'Final V1 evidence requires the exact $expectedPackageName package.',
    '"product_version=$finalProductVersion"',
    "'diagnostic_qualification_satisfied=1'",
    "'v017_release_evidence_satisfied=1'",
    "'v1_release_evidence_satisfied=1'",
    '& $verifier -V1EvidenceDirectory $staging',
    '& $verifier -V1EvidenceDirectory $output')) {
    if (-not $runnerText.Contains($clause)) {
        throw "V1 release runner lacks its fail-closed contract: $clause"
    }
}
$verifierText = [IO.File]::ReadAllText($verifier)
foreach ($clause in @(
    "'verify-v017-release-evidence.ps1'",
    "`$finalProductVersion = '1.0.0'",
    'Final V1 evidence requires the exact $expectedPackageName package.',
    "Require-Value `$applicationReport 'application_version' `$finalProductVersion",
    "@('product_version', `$finalProductVersion)",
    "'verify-release.ps1')",
    '-ExpectedSourceRevision $SourceRevision',
    '-ExpectedVersion $finalProductVersion',
    "Get-ZipEntryHash `$package 'blackbox_dogfood_tool.exe'",
    "'verify-evaluation', `$corpus, `$heldOut, `$calibration",
    "'heldout-status', `$corpus",
    "Require-Value `$status 'state' 'complete'",
    "Require-Value `$status 'qualification_passed' '1'",
    "Require-Value `$status `$name `$evaluation[`$name]",
    "'One evidence directory cannot satisfy two V1 release roles.'",
    "'heldout-evaluation.lock'",
    "Assert-ExactEntries `$heldOut @('evaluation.json', 'predictions.tsv')")) {
    if (-not $verifierText.Contains($clause)) {
        throw "V1 release verifier lacks a required gate: $clause"
    }
}

$root = Join-Path ([IO.Path]::GetTempPath()) (
    'blackbox-v1-release-contract-' + [guid]::NewGuid())
[IO.Directory]::CreateDirectory($root) | Out-Null
try {
    $output = Join-Path $root 'output'
    $arguments = @{
        SourceRevision = 'local-uncommitted'
        V017ReleaseEvidenceDirectory = $SourceRoot
        PackagePath = $SourceRoot
        OvernightCampaignDirectory = $SourceRoot
        SeventyTwoHourCampaignDirectory = $SourceRoot
        UiEvidenceDirectory = $SourceRoot
        UiReviewDirectory = $SourceRoot
        UiTestExecutable = $SourceRoot
        ClientMatrixDirectory = $SourceRoot
        ClientEvidenceDirectory = @($SourceRoot) * 5
        WindowsCiAttestationDirectory = $SourceRoot
        QualityCiAttestationDirectory = $SourceRoot
        DogfoodTool = $SourceRoot
        FrozenCorpusDirectory = $SourceRoot
        CalibrationArtifactPath = $SourceRoot
        HeldOutEvaluationDirectory = $SourceRoot
        OutputDirectory = $output
    }
    Expect-Failure { & $runner @arguments | Out-Null } 'local-uncommitted V1 release claim'
    if ((Test-Path -LiteralPath $output) -or (Test-Path -LiteralPath "$output.partial")) {
        throw 'Rejected V1 source revision created output.'
    }

    $partial = Join-Path $root 'evidence.partial'
    [IO.Directory]::CreateDirectory($partial) | Out-Null
    foreach ($name in @('manifest.sha256.ini', 'sources.tsv', 'summary.ini')) {
        [IO.File]::WriteAllText((Join-Path $partial $name), 'fixture')
    }
    $verifyArguments = $arguments.Clone()
    $verifyArguments.Remove('OutputDirectory')
    $verifyArguments['SourceRevision'] = '0123456789abcdef0123456789abcdef01234567'
    $verifyArguments['V1EvidenceDirectory'] = $partial
    Expect-Failure { & $verifier @verifyArguments | Out-Null } 'partial V1 release evidence'
    Write-Output 'V1 release evidence contracts passed.'
} finally {
    if ([IO.Directory]::Exists($root)) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
