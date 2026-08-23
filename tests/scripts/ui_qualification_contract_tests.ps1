[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,

    [Parameter(Mandatory = $true)]
    [string]$TestExecutable,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedSourceRevision
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Expect-Failure([scriptblock]$Action, [string]$Name) {
    $failed = $false
    try { & $Action } catch { $failed = $true }
    if (-not $failed) { throw "Expected rejection: $Name" }
    Write-Output "Expected rejection: $Name"
}

function Write-Int16([byte[]]$Bytes, [int]$Offset, [int16]$Value) {
    [BitConverter]::GetBytes($Value).CopyTo($Bytes, $Offset)
}

function Write-Int32([byte[]]$Bytes, [int]$Offset, [int32]$Value) {
    [BitConverter]::GetBytes($Value).CopyTo($Bytes, $Offset)
}

function New-Bitmap([string]$Path, [int]$Width, [int]$Height) {
    $imageBytes = $Width * $Height * 4
    $header = [byte[]]::new(138)
    $header[0] = 0x42
    $header[1] = 0x4d
    Write-Int32 $header 2 (138 + $imageBytes)
    Write-Int32 $header 10 138
    Write-Int32 $header 14 124
    Write-Int32 $header 18 $Width
    Write-Int32 $header 22 $Height
    Write-Int16 $header 26 1
    Write-Int16 $header 28 32
    Write-Int32 $header 30 3
    Write-Int32 $header 34 $imageBytes
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
    try {
        $stream.Write($header, 0, $header.Length)
        $stream.SetLength(138 + $imageBytes)
    } finally { $stream.Dispose() }
}

function Write-Fixture([string]$Directory, [string]$TestExecutable,
                       [string]$Runner, [string]$Verifier, [string]$Revision) {
    [IO.Directory]::CreateDirectory($Directory) | Out-Null
    $fixtures = @('representative', 'large')
    $modes = @(
        @{ Name = '100pct'; Width = 1100; Height = 700 },
        @{ Name = '150pct-high-contrast'; Width = 1650; Height = 1050 }
    )
    $pages = @('live', 'incidents', 'detail', 'detail-timeline-cursor',
        'patterns', 'settings', 'diagnostics')
    $cases = @()
    foreach ($fixture in $fixtures) {
        foreach ($mode in $modes) {
            foreach ($page in $pages) {
                $cases += @{
                    Name = "$fixture-$($mode.Name)-$page.bmp"
                    Width = $mode.Width
                    Height = $mode.Height
                }
            }
            if ($fixture -eq 'representative') {
                $cases += @{
                    Name = "$fixture-$($mode.Name)-detail-feedback-controls.bmp"
                    Width = $mode.Width
                    Height = $mode.Height
                }
            }
        }
    }
    $cases += @(
        @{ Name = 'representative-compact-100pct-onboarding.bmp'; Width = 800; Height = 600 },
        @{ Name = 'representative-200pct-high-contrast-onboarding.bmp'; Width = 1600; Height = 1200 }
    )
    foreach ($case in $cases) {
        New-Bitmap (Join-Path $Directory $case.Name) $case.Width $case.Height
    }
    $summary = @(
        'format=1', 'state=passed', 'render_backend=sdl-software',
        "source_revision=$Revision", 'fixture_count=2', 'page_count=6',
        'display_mode_count=2', 'timeline_cursor_case_count=4',
        'feedback_control_case_count=2', 'onboarding_case_count=2', 'case_count=32',
        "test_executable_sha256=$((Get-FileHash $TestExecutable -Algorithm SHA256).Hash.ToLowerInvariant())",
        "runner_sha256=$((Get-FileHash $Runner -Algorithm SHA256).Hash.ToLowerInvariant())",
        "verifier_sha256=$((Get-FileHash $Verifier -Algorithm SHA256).Hash.ToLowerInvariant())",
        'manual_visual_review_required=1', 'physical_matrix_satisfied=0'
    )
    [IO.File]::WriteAllText((Join-Path $Directory 'summary.ini'),
        (($summary -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
    $summaryHash = (Get-FileHash (Join-Path $Directory 'summary.ini') `
                                    -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest = @('format=1', 'algorithm=sha256', 'file_count=33', 'case_count=32',
                  "summary.ini=$summaryHash")
    foreach ($case in $cases) {
        $hash = (Get-FileHash (Join-Path $Directory $case.Name) `
                              -Algorithm SHA256).Hash.ToLowerInvariant()
        $manifest += "image.$($case.Name)=$hash"
    }
    [IO.File]::WriteAllText((Join-Path $Directory 'manifest.sha256.ini'),
        (($manifest -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
}

$runner = Join-Path $SourceRoot 'scripts\run-ui-qualification.ps1'
$verifier = Join-Path $SourceRoot 'scripts\verify-ui-qualification.ps1'
$recordReview = Join-Path $SourceRoot 'scripts\record-ui-visual-review.ps1'
$verifyReview = Join-Path $SourceRoot 'scripts\verify-ui-visual-review.ps1'
foreach ($script in @($runner, $verifier, $recordReview, $verifyReview)) {
    $tokens = $null
    $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile(
        $script, [ref]$tokens, [ref]$errors)
    if ($errors.Count -ne 0) { throw "PowerShell parser rejected $script" }
}
$runnerText = [IO.File]::ReadAllText($runner)
foreach ($clause in @('source_revision=$SourceRevision', 'runner_sha256=$runnerHash',
                       'verifier_sha256=$verifierHash',
                       "SetEnvironmentVariable('BLACKBOX_UI_SOURCE_REVISION', `$SourceRevision",
                       '& $verifier -EvidenceDirectory $staging')) {
    if (-not $runnerText.Contains($clause)) {
        throw "UI qualification runner lacks its provenance contract: $clause"
    }
}

$compiledTest = (Resolve-Path -LiteralPath $TestExecutable -ErrorAction Stop).Path
$mismatchOutput = Join-Path ([IO.Path]::GetTempPath()) `
    ("blackbox-ui-revision-mismatch-" + [guid]::NewGuid())
try {
    $wrongRevision = if ($ExpectedSourceRevision -ceq ('f' * 40)) {
        'e' * 40
    } else {
        'f' * 40
    }
    Expect-Failure {
        & $runner -TestExecutable $compiledTest -OutputDirectory $mismatchOutput `
            -SourceRevision $wrongRevision | Out-Null
    } 'UI generator built from another revision'
    $stagingOutput = "$mismatchOutput.partial"
    if (-not [IO.Directory]::Exists($stagingOutput) -or
        @(Get-ChildItem -LiteralPath $stagingOutput -File).Count -ne 0) {
        throw 'Revision-mismatched UI generation did not fail before raster publication.'
    }
} finally {
    foreach ($path in @($mismatchOutput, "$mismatchOutput.partial")) {
        if ([IO.Directory]::Exists($path)) {
            Remove-Item -LiteralPath $path -Recurse -Force
        }
    }
}

$root = Join-Path ([IO.Path]::GetTempPath()) ("blackbox-ui-contract-" + [guid]::NewGuid())
[IO.Directory]::CreateDirectory($root) | Out-Null
$test = Join-Path $root 'blackbox_tests.exe'
[IO.File]::WriteAllText($test, 'test-executable-fixture')
$revision = '0123456789abcdef0123456789abcdef01234567'
$evidence = Join-Path $root 'evidence'
try {
    Write-Fixture $evidence $test $runner $verifier $revision
    & $verifier -EvidenceDirectory $evidence -TestExecutable $test `
        -ExpectedSourceRevision $revision | Out-Null

    $review = Join-Path $root 'review'
    Expect-Failure {
        & $recordReview -EvidenceDirectory $evidence -OutputDirectory $review `
            -ReviewerId reviewer-a -SourceRevision $revision | Out-Null
    } 'unconfirmed UI visual review'
    & $recordReview -EvidenceDirectory $evidence -OutputDirectory $review `
        -ReviewerId reviewer-a -SourceRevision $revision -ConfirmAllCasesPassed | Out-Null
    & $verifyReview -ReviewDirectory $review -UiEvidenceDirectory $evidence `
        -ExpectedSourceRevision $revision | Out-Null
    Expect-Failure {
        & $verifyReview -ReviewDirectory $review -UiEvidenceDirectory $evidence `
            -ExpectedSourceRevision ('f' * 40) | Out-Null
    } 'wrong UI review revision'
    $reviewSummaryPath = Join-Path $review 'summary.ini'
    $reviewSummary = [IO.File]::ReadAllText($reviewSummaryPath)
    [IO.File]::AppendAllText($reviewSummaryPath, "all_cases_passed=1`n")
    Expect-Failure {
        & $verifyReview -ReviewDirectory $review -UiEvidenceDirectory $evidence `
            -ExpectedSourceRevision $revision | Out-Null
    } 'tampered UI visual review'
    [IO.File]::WriteAllText($reviewSummaryPath, $reviewSummary,
        [Text.UTF8Encoding]::new($false))
    $reviewPartial = "$review.partial"
    [IO.Directory]::Move($review, $reviewPartial)
    Expect-Failure {
        & $verifyReview -ReviewDirectory $reviewPartial -UiEvidenceDirectory $evidence `
            -ExpectedSourceRevision $revision | Out-Null
    } 'partial UI visual review'
    & $verifyReview -ReviewDirectory $reviewPartial -UiEvidenceDirectory $evidence `
        -ExpectedSourceRevision $revision -AllowStaging | Out-Null

    Expect-Failure {
        & $verifier -EvidenceDirectory $evidence -ExpectedSourceRevision ('f' * 40) | Out-Null
    } 'wrong UI source revision'
    $wrongTest = Join-Path $root 'wrong.exe'
    [IO.File]::WriteAllText($wrongTest, 'wrong')
    Expect-Failure {
        & $verifier -EvidenceDirectory $evidence -TestExecutable $wrongTest | Out-Null
    } 'wrong UI test executable'

    $summaryPath = Join-Path $evidence 'summary.ini'
    $summary = [IO.File]::ReadAllText($summaryPath)
    [IO.File]::AppendAllText($summaryPath, "state=passed`n")
    Expect-Failure { & $verifier -EvidenceDirectory $evidence | Out-Null } 'tampered UI summary'
    [IO.File]::WriteAllText($summaryPath, $summary, [Text.UTF8Encoding]::new($false))

    $bitmap = Join-Path $evidence 'representative-100pct-live.bmp'
    $originalLength = (Get-Item -LiteralPath $bitmap).Length
    $stream = [IO.File]::Open($bitmap, [IO.FileMode]::Append, [IO.FileAccess]::Write)
    try { $stream.WriteByte(1) } finally { $stream.Dispose() }
    Expect-Failure { & $verifier -EvidenceDirectory $evidence | Out-Null } 'changed UI bitmap'
    $stream = [IO.File]::Open($bitmap, [IO.FileMode]::Open, [IO.FileAccess]::Write)
    try { $stream.SetLength($originalLength) } finally { $stream.Dispose() }

    $extra = Join-Path $evidence 'extra.txt'
    [IO.File]::WriteAllText($extra, 'extra')
    Expect-Failure { & $verifier -EvidenceDirectory $evidence | Out-Null } 'extra UI evidence file'
    [IO.File]::Delete($extra)

    $partial = "$evidence.partial"
    [IO.Directory]::Move($evidence, $partial)
    Expect-Failure { & $verifier -EvidenceDirectory $partial | Out-Null } 'partial UI evidence'
    & $verifier -EvidenceDirectory $partial -AllowStaging | Out-Null
    Write-Output 'UI qualification evidence contracts passed.'
} finally {
    if ([IO.Directory]::Exists($root)) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
