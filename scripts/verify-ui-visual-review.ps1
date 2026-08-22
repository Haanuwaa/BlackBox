[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ReviewDirectory,

    [Parameter(Mandatory = $true)]
    [string]$UiEvidenceDirectory,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^(local-uncommitted|[0-9a-f]{40})$')]
    [string]$ExpectedSourceRevision,

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
        throw "UI review field mismatch: $Name"
    }
}

$review = (Resolve-Path -LiteralPath $ReviewDirectory -ErrorAction Stop).Path
if (-not [IO.Directory]::Exists($review) -or
    ([IO.File]::GetAttributes($review) -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
    (-not $AllowStaging.IsPresent -and $review.TrimEnd('\', '/').EndsWith('.partial'))) {
    throw 'UI visual review must be a published non-link directory.'
}
$entries = @(Get-ChildItem -LiteralPath $review -Force)
$differences = @(Compare-Object @('manifest.sha256.ini', 'summary.ini') `
    @($entries.Name | Sort-Object) -CaseSensitive -SyncWindow 0)
if ($entries.Count -ne 2 -or @($entries | Where-Object { $_.PSIsContainer }).Count -ne 0 -or
    $differences.Count -ne 0) {
    throw 'UI visual review does not have the exact direct-v1 file set.'
}
foreach ($entry in $entries) {
    if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        $entry.Length -le 0 -or $entry.Length -gt 16KB) {
        throw 'UI visual review contains a linked, empty, or oversized file.'
    }
}

$summary = Read-DirectV1 (Join-Path $review 'summary.ini')
$manifest = Read-DirectV1 (Join-Path $review 'manifest.sha256.ini')
Require-Value $manifest 'algorithm' 'sha256'
Require-Value $manifest 'file_count' '1'
if ($manifest.Count -ne 4 -or $manifest['summary.ini'] -notmatch '^[0-9a-f]{64}$') {
    throw 'UI visual review manifest is malformed.'
}
Require-Value $manifest 'summary.ini' (
    (Get-FileHash -LiteralPath (Join-Path $review 'summary.ini') `
                  -Algorithm SHA256).Hash.ToLowerInvariant())
foreach ($pair in @(
    @('state', 'passed'), @('source_revision', $ExpectedSourceRevision),
    @('case_count', '30'), @('all_cases_reviewed', '1'), @('all_cases_passed', '1'))) {
    Require-Value $summary $pair[0] $pair[1]
}
[DateTimeOffset]$reviewedUtc = [DateTimeOffset]::MinValue
if ($summary.Count -ne 11 -or
    $summary['reviewed_ui_manifest_sha256'] -notmatch '^[0-9a-f]{64}$' -or
    $summary['reviewer_id'] -notmatch '^[A-Za-z0-9_.-]{1,64}$' -or
    -not [DateTimeOffset]::TryParse($summary['reviewed_utc'],
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::RoundtripKind, [ref]$reviewedUtc) -or
    $summary['recorder_sha256'] -notmatch '^[0-9a-f]{64}$' -or
    $summary['verifier_sha256'] -notmatch '^[0-9a-f]{64}$') {
    throw 'UI visual review metadata is malformed.'
}
Require-Value $summary 'verifier_sha256' (
    (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant())
Require-Value $summary 'recorder_sha256' (
    (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'record-ui-visual-review.ps1') `
                  -Algorithm SHA256).Hash.ToLowerInvariant())
$uiEvidence = (Resolve-Path -LiteralPath $UiEvidenceDirectory -ErrorAction Stop).Path
& (Join-Path $PSScriptRoot 'verify-ui-qualification.ps1') `
    -EvidenceDirectory $uiEvidence -ExpectedSourceRevision $ExpectedSourceRevision | Out-Null
Require-Value $summary 'reviewed_ui_manifest_sha256' (
    (Get-FileHash -LiteralPath (Join-Path $uiEvidence 'manifest.sha256.ini') `
                  -Algorithm SHA256).Hash.ToLowerInvariant())
Write-Output "Verified UI visual review attestation: $review"
