[CmdletBinding()]
param(
    [string]$TestExecutable =
        (Join-Path $PSScriptRoot '..\out\build\windows-vs2026-release\tests\Release\blackbox_tests.exe'),

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [string]$SourceRevision = 'local-uncommitted'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-AtomicText([string]$Path, [string]$Contents) {
    $temporary = "$Path.tmp"
    [IO.File]::WriteAllText($temporary, $Contents, [Text.UTF8Encoding]::new($false))
    if ([IO.File]::Exists($Path)) {
        $backup = "$Path.previous"
        [IO.File]::Replace($temporary, $Path, $backup)
        [IO.File]::Delete($backup)
    } else {
        [IO.File]::Move($temporary, $Path)
    }
}

$test = [IO.Path]::GetFullPath($TestExecutable)
$output = [IO.Path]::GetFullPath($OutputDirectory)
$staging = "$output.partial"
if (-not [IO.File]::Exists($test)) { throw 'The Release test executable does not exist.' }
if ($SourceRevision -notmatch '^(local-uncommitted|[0-9A-Fa-f]{40})$') {
    throw 'SourceRevision must be local-uncommitted or a 40-character revision.'
}
$SourceRevision = $SourceRevision.ToLowerInvariant()
if ([IO.Directory]::Exists($output) -or [IO.File]::Exists($output) -or
    [IO.Directory]::Exists($staging) -or [IO.File]::Exists($staging)) {
    throw 'The UI qualification output and staging destinations must not already exist.'
}

[IO.Directory]::CreateDirectory($staging) | Out-Null
$runner = [IO.Path]::GetFullPath($PSCommandPath)
$verifier = Join-Path $PSScriptRoot 'verify-ui-qualification.ps1'
if (-not [IO.File]::Exists($verifier)) { throw 'The UI evidence verifier is missing.' }
$testHash = (Get-FileHash -LiteralPath $test -Algorithm SHA256).Hash.ToLowerInvariant()
$runnerHash = (Get-FileHash -LiteralPath $runner -Algorithm SHA256).Hash.ToLowerInvariant()
$verifierHash = (Get-FileHash -LiteralPath $verifier -Algorithm SHA256).Hash.ToLowerInvariant()
$oldEvidence = [Environment]::GetEnvironmentVariable('BLACKBOX_UI_EVIDENCE_DIR', 'Process')
$oldRevision = [Environment]::GetEnvironmentVariable('BLACKBOX_UI_SOURCE_REVISION', 'Process')
try {
    [Environment]::SetEnvironmentVariable('BLACKBOX_UI_EVIDENCE_DIR', $staging, 'Process')
    [Environment]::SetEnvironmentVariable('BLACKBOX_UI_SOURCE_REVISION', $SourceRevision, 'Process')
    & $test '[ui][viewer][smoke]'
    if ($LASTEXITCODE -ne 0) { throw "UI raster test exited with status $LASTEXITCODE." }
    [Environment]::SetEnvironmentVariable('BLACKBOX_UI_EVIDENCE_DIR', $oldEvidence, 'Process')
    [Environment]::SetEnvironmentVariable('BLACKBOX_UI_SOURCE_REVISION', $oldRevision, 'Process')

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
    $actualFiles = @(Get-ChildItem -LiteralPath $staging -File)
    $actual = @($actualFiles | Where-Object { $_.Extension -ieq '.bmp' })
    if ($actualFiles.Count -ne $actual.Count) {
        throw 'UI evidence contains an unexpected non-BMP file.'
    }
    if ($actual.Count -ne $expected.Count) { throw 'UI evidence does not contain exactly 30 BMP files.' }
    $manifest = @('format=1', 'algorithm=sha256', 'case_count=30')
    foreach ($case in $expected) {
        $path = Join-Path $staging $case.Name
        if (-not [IO.File]::Exists($path)) { throw "Missing UI evidence case: $($case.Name)" }
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
                [BitConverter]::ToInt32($header, 30) -ne 3 -or
                [uint64][BitConverter]::ToInt32($header, 34) -ne
                    ([uint64]$case.Width * [uint64]$case.Height * 4) -or
                [uint64]$stream.Length -ne
                    138 + ([uint64]$case.Width * [uint64]$case.Height * 4)) {
                throw "Invalid BMP evidence: $($case.Name)"
            }
        } finally { $stream.Dispose() }
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        $manifest += "image.$($case.Name)=$hash"
    }
    $summary = @"
format=1
state=passed
render_backend=sdl-software
source_revision=$SourceRevision
fixture_count=2
page_count=6
display_mode_count=2
timeline_cursor_case_count=4
feedback_control_case_count=2
case_count=30
test_executable_sha256=$testHash
runner_sha256=$runnerHash
verifier_sha256=$verifierHash
manual_visual_review_required=1
physical_matrix_satisfied=0
"@
    Write-AtomicText (Join-Path $staging 'summary.ini') $summary
    if ((Get-FileHash -LiteralPath $test -Algorithm SHA256).Hash.ToLowerInvariant() -cne $testHash -or
        (Get-FileHash -LiteralPath $runner -Algorithm SHA256).Hash.ToLowerInvariant() -cne $runnerHash -or
        (Get-FileHash -LiteralPath $verifier -Algorithm SHA256).Hash.ToLowerInvariant() -cne $verifierHash) {
        throw 'The UI test executable, runner, or verifier changed during qualification.'
    }
    $summaryHash = (Get-FileHash -LiteralPath (Join-Path $staging 'summary.ini') `
                                    -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest = @('format=1', 'algorithm=sha256', 'file_count=31', 'case_count=30',
                  "summary.ini=$summaryHash") + @($manifest | Select-Object -Skip 3)
    Write-AtomicText (Join-Path $staging 'manifest.sha256.ini') (($manifest -join "`n") + "`n")
    & $verifier -EvidenceDirectory $staging -TestExecutable $test `
        -ExpectedSourceRevision $SourceRevision -AllowStaging | Out-Null
    [IO.Directory]::Move($staging, $output)
    Write-Host "UI raster qualification evidence generated: $output"
} catch {
    throw
} finally {
    [Environment]::SetEnvironmentVariable('BLACKBOX_UI_EVIDENCE_DIR', $oldEvidence, 'Process')
    [Environment]::SetEnvironmentVariable('BLACKBOX_UI_SOURCE_REVISION', $oldRevision, 'Process')
}
