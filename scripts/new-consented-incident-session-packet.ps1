[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$DogfoodTool,
    [Parameter(Mandatory = $true)] [string]$BaseCorpusDirectory,
    [Parameter(Mandatory = $true)] [string]$ArchivePath,
    [Parameter(Mandatory = $true)] [string]$FirstBallotPath,
    [Parameter(Mandatory = $true)] [string]$SecondBallotPath,
    [Parameter(Mandatory = $true)] [string]$OutputPacketDirectory,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]{1,64}$')] [string]$ProfileId,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]{1,64}$')] [string]$OsBuildBucket,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]{1,64}$')] [string]$CpuFamily,
    [Parameter(Mandatory = $true)] [ValidateRange(1, 4096)] [int]$LogicalProcessors,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]{1,64}$')] [string]$MemoryGibBucket,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]{1,64}$')] [string]$GpuFamily,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]{1,64}$')] [string]$PowerMode,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]{1,64}$')] [string]$SessionId,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]{1,64}$')] [string]$OperatorId,
    [Parameter(Mandatory = $true)]
    [ValidateSet('calibration', 'held_out')] [string]$Split,
    [Parameter(Mandatory = $true)]
    [ValidateSet('cpu_starvation', 'disk_stall', 'network_interruption',
                 'application_crash', 'application_hang', 'game_stutter', 'audio_interruption',
                 'ambiguous')] [string]$Symptom,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{32}$')] [string]$IncidentKey,
    [Parameter(Mandatory = $true)] [ValidateRange(1, 604800)] [int]$DurationSeconds,
    [Parameter(Mandatory = $true)] [ValidateRange(0, 1)] [int]$AutomaticCaptures,
    [Parameter(Mandatory = $true)]
    [ValidateSet('confirmed', 'probable', 'uncertain', 'unresolvable')]
    [string]$Certainty,
    [Parameter(Mandatory = $true)] [ValidateSet('0', '1')] [string]$UserVisible,
    [Parameter(Mandatory = $true)]
    [ValidateSet('unknown', 'cpu_pressure', 'memory_pressure', 'storage_pressure',
                 'network_pressure', 'multi_resource_pressure', 'application_crash',
                 'application_hang', 'dns_resolution_timeout',
                 'display_driver_recovery', 'storage_io_retry')]
    [string]$ExpectedDiagnosis,
    [ValidatePattern('^$|^(?:0|[1-7]?[0-9]{1,3}|8(?:0[0-9]{2}|1[0-8][0-9]|19[01]))$')]
    [string]$ExpectedContributorOrdinal = '',
    [Parameter(Mandatory = $true)]
    [ValidateSet('unknown', 'idle', 'gaming', 'development', 'compilation',
                 'video_playback_or_call', 'heavy_download', 'desktop')]
    [string]$ExpectedContext,
    [ValidatePattern('^$|^[A-Za-z0-9._-]{1,64}$')] [string]$RecurrenceFamily = '',
    [Parameter(Mandatory = $true)]
    [ValidateSet('0', '1')] [string]$DetectorShouldCapture,
    [Parameter(Mandatory = $true)]
    [ValidateSet('unscored', 'not_useful', 'unsure', 'useful')] [string]$Usefulness,
    [Parameter(Mandatory = $true)]
    [ValidateSet('PARTICIPANT-CONSENT-CONFIRMED')] [string]$ConsentAttestation,
    [Parameter(Mandatory = $true)]
    [ValidateSet('INCIDENT-SESSION-COMPLETED')] [string]$SessionAttestation,
    [Parameter(Mandatory = $true)]
    [ValidateSet('COORDINATOR-CONSENSUS-FIXED')] [string]$ConsensusAttestation
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Dogfood([string[]]$Arguments) {
    $output = @(& $script:ResolvedDogfoodTool @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "blackbox_dogfood_tool failed: $($output -join ' ')"
    }
    return ,$output
}

function Append-Utf8([string]$Path, [string]$Line) {
    [IO.File]::AppendAllText($Path, $Line + "`n", [Text.UTF8Encoding]::new($false))
}

function Require-RegularFile([string]$Path, [string]$Name) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or
        (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw "$Name must be a regular, non-link file."
    }
    return $item
}

function Output-Value([object[]]$Lines, [string]$Name) {
    $prefix = $Name + '='
    $matches = @($Lines | ForEach-Object { [string]$_ } |
        Where-Object { $_.StartsWith($prefix, [StringComparison]::Ordinal) })
    if ($matches.Count -ne 1) {
        throw "blackbox_dogfood_tool omitted the exact $Name output."
    }
    return $matches[0].Substring($prefix.Length)
}

$toolItem = Require-RegularFile $DogfoodTool 'DogfoodTool'
$script:ResolvedDogfoodTool = $toolItem.FullName
$archiveItem = Require-RegularFile $ArchivePath 'ArchivePath'
$firstBallotItem = Require-RegularFile $FirstBallotPath 'FirstBallotPath'
$secondBallotItem = Require-RegularFile $SecondBallotPath 'SecondBallotPath'

$baseItem = Get-Item -LiteralPath $BaseCorpusDirectory -Force
if (-not $baseItem.PSIsContainer -or
    (($baseItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
    throw 'BaseCorpusDirectory must be a non-link directory.'
}

$finalPath = [IO.Path]::GetFullPath($OutputPacketDirectory)
$stagingPath = $finalPath + '.partial'
$proofPath = $finalPath + '.archive-proof'
$proofStagingPath = $proofPath + '.partial'
foreach ($path in @($finalPath, $stagingPath, $proofPath, $proofStagingPath)) {
    if (Test-Path -LiteralPath $path) {
        throw "OutputPacketDirectory or its reserved sibling already exists: $path"
    }
}

$archiveHashBefore = (Get-FileHash -LiteralPath $archiveItem.FullName -Algorithm SHA256).Hash
$published = $false
try {
    $firstValidation = Invoke-Dogfood @(
        'validate-ballot', $firstBallotItem.FullName, $IncidentKey, $OperatorId)
    $secondValidation = Invoke-Dogfood @(
        'validate-ballot', $secondBallotItem.FullName, $IncidentKey, $OperatorId)
    $comparison = Invoke-Dogfood @(
        'compare-ballots', $firstBallotItem.FullName, $secondBallotItem.FullName,
        $IncidentKey, $OperatorId)
    if ((Output-Value $firstValidation 'ballot_valid') -cne '1' -or
        (Output-Value $secondValidation 'ballot_valid') -cne '1' -or
        (Output-Value $comparison 'ballots_valid') -cne '1' -or
        (Output-Value $comparison 'annotator_count') -cne '2') {
        throw 'The native ballot contracts did not confirm two independent ballots.'
    }
    $disagreement = Output-Value $comparison 'disagreement'
    if ($disagreement -cne '0' -and $disagreement -cne '1') {
        throw 'The native ballot comparison returned an invalid disagreement value.'
    }

    $firstLines = [IO.File]::ReadAllLines($firstBallotItem.FullName)
    $secondLines = [IO.File]::ReadAllLines($secondBallotItem.FullName)
    if ($firstLines.Count -ne 2 -or $secondLines.Count -ne 2) {
        throw 'Validated ballots changed before packet construction.'
    }

    [void](Invoke-Dogfood @('init-session', $baseItem.FullName, $stagingPath))
    Append-Utf8 (Join-Path $stagingPath 'hardware.tsv') (
        "$ProfileId`twindows`t$OsBuildBucket`t$CpuFamily`t$LogicalProcessors`t" +
        "$MemoryGibBucket`t$GpuFamily`t$PowerMode")
    Append-Utf8 (Join-Path $stagingPath 'sessions.tsv') (
        "$SessionId`t$ProfileId`t$OperatorId`t$Split`tnatural`t$Symptom`t" +
        "$DurationSeconds`t1`t$AutomaticCaptures`t1")
    Append-Utf8 (Join-Path $stagingPath 'incidents.tsv') (
        "$IncidentKey`t$SessionId`t$Split`t$Symptom`t$Certainty`t$UserVisible`t" +
        "$ExpectedDiagnosis`t$ExpectedContributorOrdinal`t$ExpectedContext`t" +
        "$RecurrenceFamily`t$DetectorShouldCapture`t$Usefulness`t2`t$disagreement")
    Append-Utf8 (Join-Path $stagingPath 'annotations.tsv') $firstLines[1]
    Append-Utf8 (Join-Path $stagingPath 'annotations.tsv') $secondLines[1]

    [void](Invoke-Dogfood @('validate', $stagingPath))
    [void](Invoke-Dogfood @(
        'merge-session', $baseItem.FullName, $stagingPath, $archiveItem.FullName,
        $proofPath))
    [void](Invoke-Dogfood @('validate', $proofPath))

    $archiveHashAfter = (Get-FileHash -LiteralPath $archiveItem.FullName -Algorithm SHA256).Hash
    if ($archiveHashBefore -cne $archiveHashAfter) {
        throw 'Read-only archive proof changed the archive evidence.'
    }

    [IO.Directory]::Move($stagingPath, $finalPath)
    [void](Invoke-Dogfood @('validate', $finalPath))
    $published = $true
} catch {
    if (-not $published -and (Test-Path -LiteralPath $stagingPath)) {
        [IO.Directory]::Delete($stagingPath, $true)
    }
    throw
} finally {
    foreach ($path in @($proofPath, $proofStagingPath)) {
        if (Test-Path -LiteralPath $path) {
            [IO.Directory]::Delete($path, $true)
        }
    }
}

Write-Output 'format=1'
Write-Output 'packet_valid=1'
Write-Output 'prediction_free=1'
Write-Output 'kind=natural'
Write-Output "split=$Split"
Write-Output "incident_key=$IncidentKey"
Write-Output 'expected_incidents=1'
Write-Output "automatic_captures=$AutomaticCaptures"
Write-Output 'annotator_count=2'
Write-Output "disagreement=$disagreement"
Write-Output 'consent_attested=1'
Write-Output 'archive_proven=1'
