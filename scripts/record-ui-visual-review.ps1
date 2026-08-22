[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$EvidenceDirectory,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9_.-]{1,64}$')]
    [string]$ReviewerId,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^(local-uncommitted|[0-9a-f]{40})$')]
    [string]$SourceRevision,

    [switch]$ConfirmAllCasesPassed
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-AtomicText([string]$Path, [string]$Contents) {
    $temporary = "$Path.tmp"
    [IO.File]::WriteAllText($temporary, $Contents, [Text.UTF8Encoding]::new($false))
    [IO.File]::Move($temporary, $Path)
}

if (-not $ConfirmAllCasesPassed.IsPresent) {
    throw 'Visual review publication requires explicit confirmation that all 30 cases passed.'
}
$evidence = (Resolve-Path -LiteralPath $EvidenceDirectory -ErrorAction Stop).Path
$uiVerifier = Join-Path $PSScriptRoot 'verify-ui-qualification.ps1'
$reviewVerifier = Join-Path $PSScriptRoot 'verify-ui-visual-review.ps1'
& $uiVerifier -EvidenceDirectory $evidence -ExpectedSourceRevision $SourceRevision | Out-Null

$output = [IO.Path]::GetFullPath($OutputDirectory)
$staging = "$output.partial"
if ([IO.Directory]::Exists($output) -or [IO.File]::Exists($output) -or
    [IO.Directory]::Exists($staging) -or [IO.File]::Exists($staging)) {
    throw 'UI review output and staging destinations must not already exist.'
}
$recorderHash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
$verifierHash = (Get-FileHash -LiteralPath $reviewVerifier -Algorithm SHA256).Hash.ToLowerInvariant()
$uiManifestHash = (Get-FileHash -LiteralPath (Join-Path $evidence 'manifest.sha256.ini') `
                                   -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.Directory]::CreateDirectory($staging) | Out-Null
try {
    $summary = @(
        'format=1', 'state=passed', "source_revision=$SourceRevision",
        "reviewed_ui_manifest_sha256=$uiManifestHash", 'case_count=30',
        "reviewer_id=$ReviewerId", "reviewed_utc=$([DateTimeOffset]::UtcNow.ToString('O'))",
        'all_cases_reviewed=1', 'all_cases_passed=1',
        "recorder_sha256=$recorderHash", "verifier_sha256=$verifierHash"
    )
    Write-AtomicText (Join-Path $staging 'summary.ini') (($summary -join "`n") + "`n")
    $summaryHash = (Get-FileHash -LiteralPath (Join-Path $staging 'summary.ini') `
                                    -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-AtomicText (Join-Path $staging 'manifest.sha256.ini') (
        "format=1`nalgorithm=sha256`nfile_count=1`nsummary.ini=$summaryHash`n")
    if ((Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne
            $recorderHash -or
        (Get-FileHash -LiteralPath $reviewVerifier -Algorithm SHA256).Hash.ToLowerInvariant() -cne
            $verifierHash -or
        (Get-FileHash -LiteralPath (Join-Path $evidence 'manifest.sha256.ini') `
                      -Algorithm SHA256).Hash.ToLowerInvariant() -cne $uiManifestHash) {
        throw 'UI evidence or review tools changed during review publication.'
    }
    & $reviewVerifier -ReviewDirectory $staging -UiEvidenceDirectory $evidence `
        -ExpectedSourceRevision $SourceRevision -AllowStaging | Out-Null
    [IO.Directory]::Move($staging, $output)
    Write-Output "UI visual review attestation generated: $output"
} catch {
    throw
}
