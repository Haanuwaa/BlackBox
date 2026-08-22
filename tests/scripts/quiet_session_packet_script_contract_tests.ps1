[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$SourceRoot,
    [Parameter(Mandatory = $true)] [string]$DogfoodTool
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Checked([string]$Executable, [string[]]$Arguments) {
    $output = @(& $Executable @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "$([IO.Path]::GetFileName($Executable)) failed: $($output -join ' ')"
    }
    return $output
}

function Get-TreeHashes([string]$Directory) {
    $result = [ordered]@{}
    foreach ($file in Get-ChildItem -LiteralPath $Directory -File -Recurse | Sort-Object FullName) {
        $relative = [IO.Path]::GetRelativePath($Directory, $file.FullName)
        $result[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
    return $result
}

function Assert-TreeHashesEqual($Expected, $Actual) {
    if (($Expected.Keys -join '|') -cne ($Actual.Keys -join '|')) {
        throw 'Base corpus file set changed while constructing a quiet packet.'
    }
    foreach ($key in $Expected.Keys) {
        if ($Expected[$key] -cne $Actual[$key]) {
            throw "Base corpus file changed while constructing a quiet packet: $key"
        }
    }
}

$helper = Join-Path $SourceRoot 'scripts/new-consented-quiet-session-packet.ps1'
$tokens = $null
$errors = $null
[void][Management.Automation.Language.Parser]::ParseFile(
    $helper, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) {
    throw "Quiet-session helper has parser errors: $($errors -join ' ')"
}

$source = [IO.File]::ReadAllText($helper)
foreach ($required in @(
    "ValidateSet('PARTICIPANT-CONSENT-CONFIRMED')",
    "ValidateSet('QUIET-EXPOSURE-COMPLETED')",
    "ValidateSet('NO-AUTOMATIC-CAPTURES')",
    "'init-session'", "'validate'", "'.partial'", '[IO.Directory]::Move')) {
    if (-not $source.Contains($required)) {
        throw "Quiet-session helper is missing required contract text: $required"
    }
}
$firstValidation = $source.IndexOf("Invoke-Dogfood @('validate', `$stagingPath)")
$move = $source.IndexOf('[IO.Directory]::Move($stagingPath, $finalPath)')
$secondValidation = $source.IndexOf("Invoke-Dogfood @('validate', `$finalPath)")
if ($firstValidation -lt 0 -or $move -le $firstValidation -or $secondValidation -le $move) {
    throw 'Quiet-session helper does not validate before and after atomic publication.'
}
if ($source -match '(?i)[''"](?:inspect|evaluate|fingerprint)[''"]') {
    throw 'Quiet-session helper invokes a prediction-bearing dogfood command.'
}

$temporary = Join-Path ([IO.Path]::GetTempPath()) (
    'blackbox-quiet-session-packet-' + [guid]::NewGuid())
[IO.Directory]::CreateDirectory($temporary) | Out-Null
try {
    $base = Join-Path $temporary 'base'
    [void](Invoke-Checked $DogfoodTool @('init', $base, 'quiet-helper-contract'))
    $before = Get-TreeHashes $base

    $common = @{
        DogfoodTool = $DogfoodTool
        BaseCorpusDirectory = $base
        ProfileId = 'profile-contract'
        OsBuildBucket = 'windows-11-contract'
        CpuFamily = 'x64-contract'
        LogicalProcessors = 8
        MemoryGibBucket = '16-31'
        GpuFamily = 'gpu-contract'
        PowerMode = 'ac'
        SessionId = 'quiet-session-contract'
        OperatorId = 'operator-contract'
        Split = 'calibration'
        DurationSeconds = 3600
        ConsentAttestation = 'PARTICIPANT-CONSENT-CONFIRMED'
        ExposureAttestation = 'QUIET-EXPOSURE-COMPLETED'
        NoCaptureAttestation = 'NO-AUTOMATIC-CAPTURES'
    }

    $packet = Join-Path $temporary 'packet'
    $valid = @(& $helper @common -OutputPacketDirectory $packet 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Quiet-session helper failed: $($valid -join ' ')"
    }
    $validText = $valid -join "`n"
    foreach ($line in @('format=1', 'packet_valid=1', 'prediction_free=1',
                         'kind=quiet', 'split=calibration', 'duration_seconds=3600',
                         'expected_incidents=0', 'automatic_captures=0',
                         'consent_attested=1')) {
        if ($validText -notmatch "(?m)^$([regex]::Escape($line))$") {
            throw "Quiet-session helper output omitted: $line"
        }
    }
    [void](Invoke-Checked $DogfoodTool @('validate', $packet))
    $sessionRows = Get-Content -LiteralPath (Join-Path $packet 'sessions.tsv')
    if ($sessionRows.Count -ne 2 -or
        $sessionRows[1] -cne
        "quiet-session-contract`tprofile-contract`toperator-contract`tcalibration`tquiet`tquiet`t3600`t0`t0`t1") {
        throw 'Quiet-session helper emitted the wrong direct-V1 session row.'
    }
    if (Test-Path -LiteralPath ($packet + '.partial')) {
        throw 'Successful quiet-session publication left a partial directory.'
    }
    Assert-TreeHashesEqual $before (Get-TreeHashes $base)

    $invalidCases = @(
        @('ConsentAttestation', 'UNCONFIRMED'),
        @('ExposureAttestation', 'INCOMPLETE'),
        @('NoCaptureAttestation', 'CAPTURES-UNKNOWN'))
    foreach ($case in $invalidCases) {
        $parameters = @{} + $common
        $parameters[$case[0]] = $case[1]
        $invalidOutput = Join-Path $temporary ("invalid-" + $case[0])
        $parameters.OutputPacketDirectory = $invalidOutput
        try {
            & $helper @parameters 2>&1 | Out-Null
            throw "Invalid $($case[0]) token was accepted."
        } catch {
            if ($_.Exception.Message -eq "Invalid $($case[0]) token was accepted.") { throw }
        }
        if ((Test-Path -LiteralPath $invalidOutput) -or
            (Test-Path -LiteralPath ($invalidOutput + '.partial'))) {
            throw "Rejected $($case[0]) token created packet output."
        }
    }

    try {
        & $helper @common -OutputPacketDirectory $packet 2>&1 | Out-Null
        throw 'Occupied quiet-session output was overwritten.'
    } catch {
        if ($_.Exception.Message -eq 'Occupied quiet-session output was overwritten.') { throw }
    }
    Assert-TreeHashesEqual $before (Get-TreeHashes $base)
} finally {
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Recurse -Force
    }
}

Write-Output 'Quiet-session packet script contracts passed.'
