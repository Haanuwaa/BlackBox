[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$EvidenceDirectory,

    [string]$TestExecutable = '',
    [string]$ExpectedSourceRevision = '',
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
        throw "UI evidence field mismatch: $Name"
    }
}

$fixtures = @('representative', 'large')
$modes = @(
    @{ Name = '100pct'; Width = 1100; Height = 700 },
    @{ Name = '150pct-high-contrast'; Width = 1650; Height = 1050 }
)
$pages = @('live', 'incidents', 'detail', 'detail-timeline-cursor',
    'patterns', 'settings', 'diagnostics')
$expected = @()
foreach ($fixture in $fixtures) {
    foreach ($mode in $modes) {
        foreach ($page in $pages) {
            $expected += @{
                Name = "$fixture-$($mode.Name)-$page.bmp"
                Width = $mode.Width
                Height = $mode.Height
            }
        }
        if ($fixture -eq 'representative') {
            $expected += @{
                Name = "$fixture-$($mode.Name)-detail-feedback-controls.bmp"
                Width = $mode.Width
                Height = $mode.Height
            }
        }
    }
}

$directory = (Resolve-Path -LiteralPath $EvidenceDirectory -ErrorAction Stop).Path
if (-not [IO.Directory]::Exists($directory) -or
    ([IO.File]::GetAttributes($directory) -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
    (-not $AllowStaging.IsPresent -and $directory.TrimEnd('\', '/').EndsWith('.partial'))) {
    throw 'UI evidence must be a published non-link directory.'
}
$entries = @(Get-ChildItem -LiteralPath $directory -Force)
if (@($entries | Where-Object { $_.PSIsContainer }).Count -ne 0) {
    throw 'UI evidence cannot contain directories.'
}
$expectedNames = @('manifest.sha256.ini', 'summary.ini') + @($expected.Name)
$differences = @(Compare-Object ($expectedNames | Sort-Object) `
    @($entries.Name | Sort-Object) -CaseSensitive -SyncWindow 0)
if ($entries.Count -ne 32 -or $differences.Count -ne 0) {
    throw 'UI evidence does not contain the exact 30-case direct-v1 file set.'
}
foreach ($entry in $entries) {
    if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        $entry.Length -le 0 -or $entry.Length -gt 16MB) {
        throw 'UI evidence contains a linked, empty, or oversized file.'
    }
}

$summary = Read-DirectV1 (Join-Path $directory 'summary.ini')
foreach ($pair in @(
    @('state', 'passed'), @('render_backend', 'sdl-software'),
    @('fixture_count', '2'), @('page_count', '6'), @('display_mode_count', '2'),
    @('timeline_cursor_case_count', '4'), @('feedback_control_case_count', '2'),
    @('case_count', '30'), @('manual_visual_review_required', '1'),
    @('physical_matrix_satisfied', '0'))) {
    Require-Value $summary $pair[0] $pair[1]
}
if ($summary.Count -ne 15 -or
    $summary['source_revision'] -notmatch '^(local-uncommitted|[0-9a-f]{40})$') {
    throw 'UI evidence summary has malformed provenance or unexpected fields.'
}
if (-not [string]::IsNullOrWhiteSpace($ExpectedSourceRevision)) {
    if ($ExpectedSourceRevision -notmatch '^(local-uncommitted|[0-9a-f]{40})$') {
        throw 'ExpectedSourceRevision is malformed.'
    }
    Require-Value $summary 'source_revision' $ExpectedSourceRevision
}
foreach ($name in @('test_executable_sha256', 'runner_sha256', 'verifier_sha256')) {
    if ($summary[$name] -notmatch '^[0-9a-f]{64}$') {
        throw "UI evidence provenance is malformed: $name"
    }
}
$verifierHash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
Require-Value $summary 'verifier_sha256' $verifierHash
$runnerHash = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'run-ui-qualification.ps1') `
                           -Algorithm SHA256).Hash.ToLowerInvariant()
Require-Value $summary 'runner_sha256' $runnerHash
if (-not [string]::IsNullOrWhiteSpace($TestExecutable)) {
    $test = (Resolve-Path -LiteralPath $TestExecutable -ErrorAction Stop).Path
    if (-not [IO.File]::Exists($test)) { throw 'UI test executable is not a regular file.' }
    Require-Value $summary 'test_executable_sha256' (
        (Get-FileHash -LiteralPath $test -Algorithm SHA256).Hash.ToLowerInvariant())
}

$manifest = Read-DirectV1 (Join-Path $directory 'manifest.sha256.ini')
Require-Value $manifest 'algorithm' 'sha256'
Require-Value $manifest 'file_count' '31'
Require-Value $manifest 'case_count' '30'
if ($manifest.Count -ne 35) { throw 'UI evidence manifest field count is invalid.' }
$summaryHash = (Get-FileHash -LiteralPath (Join-Path $directory 'summary.ini') `
                                -Algorithm SHA256).Hash.ToLowerInvariant()
Require-Value $manifest 'summary.ini' $summaryHash

foreach ($case in $expected) {
    $path = Join-Path $directory $case.Name
    $stream = [IO.File]::OpenRead($path)
    try {
        $header = [byte[]]::new(138)
        if ($stream.Read($header, 0, $header.Length) -ne $header.Length -or
            $header[0] -ne 0x42 -or $header[1] -ne 0x4d -or
            [BitConverter]::ToInt32($header, 10) -ne 138 -or
            [BitConverter]::ToInt32($header, 14) -ne 124 -or
            [BitConverter]::ToInt32($header, 18) -ne $case.Width -or
            [math]::Abs([BitConverter]::ToInt32($header, 22)) -ne $case.Height -or
            [BitConverter]::ToInt16($header, 26) -ne 1 -or
            [BitConverter]::ToInt16($header, 28) -ne 32 -or
            [BitConverter]::ToInt32($header, 30) -ne 3) {
            throw "UI evidence BMP header is invalid: $($case.Name)"
        }
        $imageBytes = [uint64]$case.Width * [uint64]$case.Height * 4
        if ([uint64][BitConverter]::ToInt32($header, 34) -ne $imageBytes -or
            [uint64]$stream.Length -ne 138 + $imageBytes) {
            throw "UI evidence BMP payload length is invalid: $($case.Name)"
        }
    } finally { $stream.Dispose() }
    $key = "image.$($case.Name)"
    if ($manifest[$key] -notmatch '^[0-9a-f]{64}$') {
        throw "UI evidence manifest omits or malforms: $key"
    }
    Require-Value $manifest $key (
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant())
}

Write-Output "Verified UI raster qualification evidence: $directory"
