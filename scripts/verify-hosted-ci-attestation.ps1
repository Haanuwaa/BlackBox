[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$AttestationDirectory,

    [Parameter(Mandatory = $true)]
    [ValidateSet('windows', 'quality')]
    [string]$WorkflowKey,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{40}$')]
    [string]$ExpectedSourceRevision
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
        throw "Hosted CI attestation field mismatch: $Name"
    }
}

$expected = @{
    windows = @{ Name = 'Windows validation'; File = '.github/workflows/windows.yml' }
    quality = @{ Name = 'Quality and security'; File = '.github/workflows/quality.yml' }
}[$WorkflowKey]
$directory = (Resolve-Path -LiteralPath $AttestationDirectory -ErrorAction Stop).Path
if (-not [IO.Directory]::Exists($directory) -or
    ([IO.File]::GetAttributes($directory) -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
    $directory.TrimEnd('\', '/').EndsWith('.partial')) {
    throw 'Hosted CI attestation must be a published non-link directory.'
}
$entries = @(Get-ChildItem -LiteralPath $directory -Force)
$entryDifferences = @(Compare-Object @('manifest.sha256.ini', 'summary.ini') `
    @($entries.Name | Sort-Object) -CaseSensitive -SyncWindow 0)
if ($entries.Count -ne 2 -or @($entries | Where-Object { $_.PSIsContainer }).Count -ne 0 -or
    $entryDifferences.Count -ne 0) {
    throw 'Hosted CI attestation does not have the exact direct-v1 file set.'
}
foreach ($entry in $entries) {
    if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        $entry.Length -le 0 -or $entry.Length -gt 16KB) {
        throw 'Hosted CI attestation file is linked, empty, or oversized.'
    }
}

$summary = Read-DirectV1 (Join-Path $directory 'summary.ini')
$manifest = Read-DirectV1 (Join-Path $directory 'manifest.sha256.ini')
Require-Value $manifest 'algorithm' 'sha256'
Require-Value $manifest 'file_count' '1'
if ($manifest.Count -ne 4 -or $manifest['summary.ini'] -notmatch '^[0-9a-f]{64}$') {
    throw 'Hosted CI attestation manifest is malformed.'
}
$summaryHash = (Get-FileHash -LiteralPath (Join-Path $directory 'summary.ini') `
                                -Algorithm SHA256).Hash.ToLowerInvariant()
Require-Value $manifest 'summary.ini' $summaryHash

foreach ($pair in @(
    @('state', 'passed'), @('provider', 'github-actions'), @('workflow_key', $WorkflowKey),
    @('workflow_name', $expected.Name), @('workflow_file', $expected.File),
    @('source_revision', $ExpectedSourceRevision))) {
    Require-Value $summary $pair[0] $pair[1]
}
if ($summary.Count -ne 14 -or
    $summary['repository'] -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' -or
    $summary['run_id'] -notmatch '^[1-9][0-9]{0,19}$' -or
    $summary['run_attempt'] -notmatch '^[1-9][0-9]{0,9}$' -or
    $summary['event_name'] -notin @('push', 'workflow_dispatch') -or
    $summary['ref'] -notmatch '^refs/(heads|tags)/[^\r\n\t=]{1,240}$' -or
    $summary['workflow_ref'] -cnotmatch ('^' + [regex]::Escape(
        "$($summary['repository'])/$($expected.File)@") + 'refs/(heads|tags)/.+$') -or
    $summary['writer_sha256'] -notmatch '^[0-9a-f]{64}$') {
    throw 'Hosted CI attestation metadata is malformed.'
}
$writerHash = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'write-hosted-ci-attestation.ps1') `
                               -Algorithm SHA256).Hash.ToLowerInvariant()
Require-Value $summary 'writer_sha256' $writerHash
Write-Output "Verified hosted CI $WorkflowKey attestation: $directory"
