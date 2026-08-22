[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$MatrixDirectory,

    [Parameter(Mandatory = $true)]
    [string[]]$EvidenceDirectory,

    [string]$ExpectedSourceRevision = '',
    [switch]$RequireAuthenticode
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

$matrix = (Resolve-Path -LiteralPath $MatrixDirectory -ErrorAction Stop).Path
if (-not [IO.Directory]::Exists($matrix) -or
    ([IO.File]::GetAttributes($matrix) -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
    $matrix.TrimEnd('\', '/').EndsWith('.partial')) {
    throw 'Client matrix evidence must be a published non-link directory.'
}
$entries = @(Get-ChildItem -LiteralPath $matrix -Force)
$differences = @(Compare-Object @('manifest.sha256.ini', 'sources.tsv', 'summary.ini') `
    @($entries.Name | Sort-Object) -CaseSensitive -SyncWindow 0)
if ($entries.Count -ne 3 -or @($entries | Where-Object { $_.PSIsContainer }).Count -ne 0 -or
    $differences.Count -ne 0) {
    throw 'Client matrix evidence does not have the exact direct-v1 file set.'
}
foreach ($entry in $entries) {
    if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        $entry.Length -le 0 -or $entry.Length -gt 1MB) {
        throw 'Client matrix evidence contains a linked, empty, or oversized file.'
    }
}
$summary = Read-DirectV1 (Join-Path $matrix 'summary.ini')
if (-not [string]::IsNullOrWhiteSpace($ExpectedSourceRevision)) {
    if ($ExpectedSourceRevision -notmatch '^[0-9a-f]{40}$' -or
        $summary['source_revision'] -cne $ExpectedSourceRevision) {
        throw 'Client matrix source revision does not match the expected release revision.'
    }
}

$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'blackbox-client-matrix-verify-' + [guid]::NewGuid())
$regenerated = Join-Path $temporaryRoot 'matrix'
[IO.Directory]::CreateDirectory($temporaryRoot) | Out-Null
try {
    & (Join-Path $PSScriptRoot 'verify-client-matrix.ps1') `
        -EvidenceDirectory $EvidenceDirectory -OutputDirectory $regenerated `
        -RequireAuthenticode:$RequireAuthenticode.IsPresent | Out-Null
    foreach ($relative in @('sources.tsv', 'summary.ini', 'manifest.sha256.ini')) {
        $expectedHash = (Get-FileHash -LiteralPath (Join-Path $regenerated $relative) `
                                        -Algorithm SHA256).Hash.ToLowerInvariant()
        $actualHash = (Get-FileHash -LiteralPath (Join-Path $matrix $relative) `
                                      -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -cne $expectedHash) {
            throw "Published client matrix differs from strict regeneration: $relative"
        }
    }
} finally {
    if ([IO.Directory]::Exists($temporaryRoot)) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}

Write-Output "Verified client matrix evidence: $matrix"
