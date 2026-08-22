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

$runner = Join-Path $SourceRoot 'scripts\run-v017-release-qualification.ps1'
$verifier = Join-Path $SourceRoot 'scripts\verify-v017-release-evidence.ps1'
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
    '& $verifier -ReleaseEvidenceDirectory $staging',
    "'official_signed_package_satisfied=1'",
    "'v017_release_evidence_satisfied=1'")) {
    if (-not $runnerText.Contains($clause)) {
        throw "V0.17 release runner lacks its fail-closed contract: $clause"
    }
}
$verifierText = [IO.File]::ReadAllText($verifier)
foreach ($clause in @(
    "'verify-release.ps1') -PackagePath `$package",
    '-ExpectedSourceRevision $SourceRevision',
    '-RequireAuthenticode | Out-Null',
    "'verify-wall-clock-soak.ps1'",
    "Require-Value `$overnightSummary 'application_sha256' `$applicationHash",
    "Require-Value `$seventyTwoSummary 'application_sha256' `$applicationHash",
    "'verify-ui-qualification.ps1'",
    "'verify-ui-visual-review.ps1'",
    "'verify-client-matrix-evidence.ps1'",
    "'verify-hosted-ci-attestation.ps1'",
    "@('official_signed_matrix_satisfied', '1')")) {
    if (-not $verifierText.Contains($clause)) {
        throw "V0.17 release verifier lacks a required gate: $clause"
    }
}

$root = Join-Path ([IO.Path]::GetTempPath()) ("blackbox-v017-contract-" + [guid]::NewGuid())
[IO.Directory]::CreateDirectory($root) | Out-Null
try {
    $output = Join-Path $root 'output'
    $arguments = @{
        SourceRevision = 'local-uncommitted'
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
        OutputDirectory = $output
    }
    Expect-Failure { & $runner @arguments | Out-Null } 'local-uncommitted V0.17 release claim'
    if ((Test-Path -LiteralPath $output) -or (Test-Path -LiteralPath "$output.partial")) {
        throw 'Rejected V0.17 source revision created output.'
    }

    $partial = Join-Path $root 'evidence.partial'
    [IO.Directory]::CreateDirectory($partial) | Out-Null
    foreach ($name in @('manifest.sha256.ini', 'sources.tsv', 'summary.ini')) {
        [IO.File]::WriteAllText((Join-Path $partial $name), 'fixture')
    }
    $verifyArguments = @{
        ReleaseEvidenceDirectory = $partial
        SourceRevision = '0123456789abcdef0123456789abcdef01234567'
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
    }
    Expect-Failure { & $verifier @verifyArguments | Out-Null } 'partial V0.17 release evidence'
    Write-Output 'V0.17 release evidence contracts passed.'
} finally {
    if ([IO.Directory]::Exists($root)) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
