[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$SourceRoot,
    [Parameter(Mandatory = $true)] [string]$FixtureGenerator,
    [Parameter(Mandatory = $true)] [string]$DogfoodTool,
    [Parameter(Mandatory = $true)] [string]$DatasetTool
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
    foreach ($file in Get-ChildItem -LiteralPath $Directory -File -Recurse |
             Sort-Object FullName) {
        $relative = [IO.Path]::GetRelativePath($Directory, $file.FullName)
        $result[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
    return $result
}

function Assert-TreeHashesEqual($Expected, $Actual, [string]$Subject) {
    if (($Expected.Keys -join '|') -cne ($Actual.Keys -join '|')) {
        throw "$Subject file set changed."
    }
    foreach ($key in $Expected.Keys) {
        if ($Expected[$key] -cne $Actual[$key]) {
            throw "$Subject file changed: $key"
        }
    }
}

$helper = Join-Path $SourceRoot 'scripts/new-consented-incident-session-packet.ps1'
$tokens = $null
$errors = $null
[void][Management.Automation.Language.Parser]::ParseFile(
    $helper, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) {
    throw "Incident-session helper has parser errors: $($errors -join ' ')"
}

$source = [IO.File]::ReadAllText($helper)
foreach ($required in @(
    "ValidateSet('PARTICIPANT-CONSENT-CONFIRMED')",
    "ValidateSet('INCIDENT-SESSION-COMPLETED')",
    "ValidateSet('COORDINATOR-CONSENSUS-FIXED')",
    "'validate-ballot'", "'compare-ballots'", "'init-session'",
    "'merge-session'", "'validate'", "'.partial'", '[IO.Directory]::Move',
    'Get-FileHash')) {
    if (-not $source.Contains($required)) {
        throw "Incident-session helper is missing required contract text: $required"
    }
}
if ($source -match '(?i)[''"](?:inspect|evaluate|fingerprint)[''"]') {
    throw 'Incident-session helper invokes a prediction-bearing dogfood command.'
}

$temporary = Join-Path ([IO.Path]::GetTempPath()) (
    'blackbox-incident-session-packet-' + [guid]::NewGuid())
[IO.Directory]::CreateDirectory($temporary) | Out-Null
try {
    $archive = Join-Path $temporary 'incidents.sqlite3'
    [void](Invoke-Checked $FixtureGenerator @($archive))
    $dataset = Join-Path $temporary 'dataset'
    [void](Invoke-Checked $DatasetTool @('export', $archive, $dataset))
    $incidentRows = @(Import-Csv -LiteralPath (Join-Path $dataset 'incidents.tsv') `
        -Delimiter "`t")
    if ($incidentRows.Count -lt 1) { throw 'Fixture dataset contains no incident.' }
    $incidentKey = $incidentRows[0].incident_key
    if ($incidentKey -notmatch '^[0-9a-f]{32}$') { throw 'Fixture incident key is invalid.' }
    $automaticCaptures = if ([uint64]$incidentRows[0].automatic_trigger_count -gt 0) {
        1
    } else {
        0
    }

    $base = Join-Path $temporary 'base'
    [void](Invoke-Checked $DogfoodTool @('init', $base, 'incident-helper-contract'))
    $baseBefore = Get-TreeHashes $base
    $archiveHashBefore = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash

    $header = "incident_key`tannotator_id`tsymptom`tcertainty`tuser_visible`t" +
        "expected_diagnosis`texpected_contributor_ordinal`texpected_context`t" +
        "recurrence_family`tusefulness`n"
    $firstBallot = Join-Path $temporary 'ballot-a.tsv'
    $secondBallot = Join-Path $temporary 'ballot-b.tsv'
    [IO.File]::WriteAllText($firstBallot, $header +
        "$incidentKey`tannotator-a`tapplication_crash`tprobable`t1`t" +
        "application_crash`t0`tdesktop`tfixture-family`tuseful`n",
        [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($secondBallot, $header +
        "$incidentKey`tannotator-b`tapplication_crash`tprobable`t1`t" +
        "application_crash`t0`tdesktop`tfixture-family`tuseful`n",
        [Text.UTF8Encoding]::new($false))

    $common = @{
        DogfoodTool = $DogfoodTool
        BaseCorpusDirectory = $base
        ArchivePath = $archive
        FirstBallotPath = $firstBallot
        SecondBallotPath = $secondBallot
        ProfileId = 'profile-contract'
        OsBuildBucket = 'windows-11-contract'
        CpuFamily = 'x64-contract'
        LogicalProcessors = 8
        MemoryGibBucket = '16-31'
        GpuFamily = 'gpu-contract'
        PowerMode = 'ac'
        SessionId = 'natural-session-contract'
        OperatorId = 'operator-contract'
        Split = 'calibration'
        Symptom = 'application_crash'
        IncidentKey = $incidentKey
        DurationSeconds = 60
        AutomaticCaptures = $automaticCaptures
        Certainty = 'probable'
        UserVisible = '1'
        ExpectedDiagnosis = 'application_crash'
        ExpectedContributorOrdinal = '0'
        ExpectedContext = 'desktop'
        RecurrenceFamily = 'fixture-family'
        DetectorShouldCapture = '1'
        Usefulness = 'useful'
        ConsentAttestation = 'PARTICIPANT-CONSENT-CONFIRMED'
        SessionAttestation = 'INCIDENT-SESSION-COMPLETED'
        ConsensusAttestation = 'COORDINATOR-CONSENSUS-FIXED'
    }

    $packet = Join-Path $temporary 'packet'
    $valid = @(& $helper @common -OutputPacketDirectory $packet 2>&1)
    $validText = $valid -join "`n"
    foreach ($line in @('format=1', 'packet_valid=1', 'prediction_free=1',
                         'kind=natural', 'split=calibration',
                         "incident_key=$incidentKey", 'expected_incidents=1',
                         "automatic_captures=$automaticCaptures", 'annotator_count=2',
                         'disagreement=0', 'consent_attested=1', 'archive_proven=1')) {
        if ($validText -notmatch "(?m)^$([regex]::Escape($line))$") {
            throw "Incident-session helper output omitted: $line"
        }
    }
    [void](Invoke-Checked $DogfoodTool @('validate', $packet))
    $sessionRows = @(Get-Content -LiteralPath (Join-Path $packet 'sessions.tsv'))
    $incidentPacketRows = @(Get-Content -LiteralPath (Join-Path $packet 'incidents.tsv'))
    $annotationRows = @(Get-Content -LiteralPath (Join-Path $packet 'annotations.tsv'))
    if ($sessionRows.Count -ne 2 -or
        $sessionRows[1] -cne
        "natural-session-contract`tprofile-contract`toperator-contract`tcalibration`tnatural`tapplication_crash`t60`t1`t$automaticCaptures`t1") {
        throw 'Incident-session helper emitted the wrong direct-V1 session row.'
    }
    if ($incidentPacketRows.Count -ne 2 -or
        $incidentPacketRows[1] -cne
        "$incidentKey`tnatural-session-contract`tcalibration`tapplication_crash`tprobable`t1`tapplication_crash`t0`tdesktop`tfixture-family`t1`tuseful`t2`t0") {
        throw 'Incident-session helper emitted the wrong direct-V1 truth row.'
    }
    if ($annotationRows.Count -ne 3 -or
        $annotationRows[1] -notmatch "^$incidentKey`tannotator-a`t" -or
        $annotationRows[2] -notmatch "^$incidentKey`tannotator-b`t") {
        throw 'Incident-session helper did not copy the two validated ballots exactly once.'
    }
    foreach ($reserved in @($packet + '.partial', $packet + '.archive-proof',
                            $packet + '.archive-proof.partial')) {
        if (Test-Path -LiteralPath $reserved) {
            throw "Successful incident-session publication left reserved output: $reserved"
        }
    }

    $merged = Join-Path $temporary 'merged'
    [void](Invoke-Checked $DogfoodTool @(
        'merge-session', $base, $packet, $archive, $merged))
    [void](Invoke-Checked $DogfoodTool @('validate', $merged))
    Assert-TreeHashesEqual $baseBefore (Get-TreeHashes $base) 'Base corpus'
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -cne
        $archiveHashBefore) {
        throw 'Incident-session helper or merge modified archive evidence.'
    }

    $badCommon = @{} + $common
    $badCommon.AutomaticCaptures = 1 - $automaticCaptures
    $badOutput = Join-Path $temporary 'bad-archive-proof'
    try {
        & $helper @badCommon -OutputPacketDirectory $badOutput 2>&1 | Out-Null
        throw 'Incorrect automatic-capture provenance was accepted.'
    } catch {
        if ($_.Exception.Message -eq
            'Incorrect automatic-capture provenance was accepted.') { throw }
    }
    foreach ($reserved in @($badOutput, $badOutput + '.partial',
                            $badOutput + '.archive-proof',
                            $badOutput + '.archive-proof.partial')) {
        if (Test-Path -LiteralPath $reserved) {
            throw 'Rejected archive provenance left packet or proof output.'
        }
    }

    $operatorBallot = Join-Path $temporary 'operator-ballot.tsv'
    [IO.File]::WriteAllText($operatorBallot, $header +
        "$incidentKey`toperator-contract`tcpu_starvation`tprobable`t1`t" +
        "cpu_pressure`t0`tdesktop`tfixture-family`tuseful`n",
        [Text.UTF8Encoding]::new($false))
    $operatorCommon = @{} + $common
    $operatorCommon.FirstBallotPath = $operatorBallot
    $operatorOutput = Join-Path $temporary 'operator-output'
    try {
        & $helper @operatorCommon -OutputPacketDirectory $operatorOutput 2>&1 | Out-Null
        throw 'Session operator was accepted as an independent annotator.'
    } catch {
        if ($_.Exception.Message -eq
            'Session operator was accepted as an independent annotator.') { throw }
    }
    if ((Test-Path -LiteralPath $operatorOutput) -or
        (Test-Path -LiteralPath ($operatorOutput + '.partial'))) {
        throw 'Rejected operator-authored ballot created packet output.'
    }

    try {
        & $helper @common -OutputPacketDirectory $packet 2>&1 | Out-Null
        throw 'Occupied incident-session output was overwritten.'
    } catch {
        if ($_.Exception.Message -eq
            'Occupied incident-session output was overwritten.') { throw }
    }
    Assert-TreeHashesEqual $baseBefore (Get-TreeHashes $base) 'Base corpus'
} finally {
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Recurse -Force
    }
}

Write-Output 'Incident-session packet script contracts passed.'
