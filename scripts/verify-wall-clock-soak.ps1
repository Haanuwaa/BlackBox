[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CampaignDirectory,

    [switch]$AllowStaging
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$invariant = [Globalization.CultureInfo]::InvariantCulture

function Read-DirectV1([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { throw "Missing direct-v1 artifact: $Path" }
    $fields = @{}
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            throw "Direct-v1 artifact contains a blank line: $Path"
        }
        $separator = $line.IndexOf('=')
        if ($separator -lt 1) { throw "Malformed direct-v1 field: $Path" }
        $name = $line.Substring(0, $separator)
        if ($fields.ContainsKey($name)) { throw "Duplicate direct-v1 field: $name" }
        $fields[$name] = $line.Substring($separator + 1)
    }
    if ($fields['format'] -ne '1') { throw "Artifact is not direct format v1: $Path" }
    return $fields
}

function Require-Field($Fields, [string]$Name) {
    if (-not $Fields.ContainsKey($Name)) { throw "Missing required field: $Name" }
    return [string]$Fields[$Name]
}

function Read-UInt($Fields, [string]$Name) {
    $text = Require-Field $Fields $Name
    [uint64]$value = 0
    if (-not [uint64]::TryParse($text, [Globalization.NumberStyles]::None,
                                $invariant, [ref]$value)) {
        throw "Field is not an unsigned integer: $Name"
    }
    return $value
}

function Read-Int($Fields, [string]$Name) {
    $text = Require-Field $Fields $Name
    [int64]$value = 0
    if (-not [int64]::TryParse($text, [Globalization.NumberStyles]::AllowLeadingSign,
                               $invariant, [ref]$value)) {
        throw "Field is not a signed integer: $Name"
    }
    return $value
}

function Read-Double($Fields, [string]$Name) {
    $text = Require-Field $Fields $Name
    [double]$value = 0.0
    if (-not [double]::TryParse($text, [Globalization.NumberStyles]::AllowDecimalPoint,
                                $invariant, [ref]$value) -or
        [double]::IsNaN($value) -or [double]::IsInfinity($value)) {
        throw "Field is not a finite invariant decimal: $Name"
    }
    return $value
}

function Assert-SchedulingDropEvidence($Fields) {
    $drops = Read-UInt $Fields 'dropped_samples'
    $misses = Read-UInt $Fields 'deadline_misses'
    $count = Read-UInt $Fields 'scheduling_drop_event_count'
    $overflow = Read-UInt $Fields 'scheduling_drop_event_overflow'
    $encoded = Require-Field $Fields 'scheduling_drop_events'
    if ($count -gt 256) {
        throw 'Scheduling drop evidence exceeds its fixed collector capacity.'
    }
    if ($count -eq 0) {
        if ($encoded -cne 'none' -or $drops -ne 0 -or $misses -ne 0 -or
            $overflow -ne 0) {
            throw 'Empty scheduling drop evidence contradicts scheduling counters.'
        }
        return
    }
    if ($encoded -ceq 'none') {
        throw 'Scheduling drop evidence is missing its event records.'
    }
    $records = @($encoded.Split(';'))
    if ([uint64]$records.Count -ne $count) {
        throw 'Scheduling drop evidence count does not match its event records.'
    }
    [uint64]$previousCollection = 0
    [uint64]$previousTimestamp = 0
    [uint64]$recordedDrops = 0
    foreach ($record in $records) {
        $parts = $record.Split(':')
        if ($parts.Count -ne 4) {
            throw 'A scheduling drop event is malformed.'
        }
        [uint64[]]$values = @(0, 0, 0, 0)
        for ($index = 0; $index -lt 4; ++$index) {
            [uint64]$parsedValue = 0
            if (-not [uint64]::TryParse(
                    $parts[$index], [Globalization.NumberStyles]::None,
                    $invariant, [ref]$parsedValue)) {
                throw 'A scheduling drop event contains a non-unsigned value.'
            }
            $values[$index] = $parsedValue
        }
        if ($values[0] -le $previousCollection -or
            $values[1] -lt $previousTimestamp -or $values[1] -eq 0 -or
            $values[3] -eq 0) {
            throw 'Scheduling drop events are not ordered, timestamped, and nonzero.'
        }
        $previousCollection = $values[0]
        $previousTimestamp = $values[1]
        $recordedDrops += $values[3]
    }
    if (($overflow -eq 0 -and $recordedDrops -ne $drops) -or
        ($overflow -ne 0 -and ($count -ne 256 -or $recordedDrops -gt $drops)) -or
        $misses -gt ($count + $overflow)) {
        throw 'Scheduling drop event details contradict aggregate scheduling counters.'
    }
}

function Require-Zero($Fields, [string[]]$Names) {
    foreach ($name in $Names) {
        if ((Read-UInt $Fields $name) -ne 0) {
            throw "Soak evidence requires $name=0."
        }
    }
}

function Get-BundleRelativePath([string]$Root, [string]$Path) {
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $pathFull = [IO.Path]::GetFullPath($Path)
    $prefix = $rootFull + [IO.Path]::DirectorySeparatorChar
    $comparison = if ([IO.Path]::DirectorySeparatorChar -eq '\') {
        [StringComparison]::OrdinalIgnoreCase
    } else {
        [StringComparison]::Ordinal
    }
    if (-not $pathFull.StartsWith($prefix, $comparison)) {
        throw 'A soak bundle path resolved outside the campaign root.'
    }
    $relative = $pathFull.Substring($prefix.Length).Replace('\', '/')
    if ([string]::IsNullOrEmpty($relative) -or [IO.Path]::IsPathRooted($relative)) {
        throw 'A soak bundle path could not be normalized safely.'
    }
    return $relative
}

function Get-AverageMetric([object[]]$Samples, [string]$Name) {
    if ($Samples.Count -eq 0) { return 0.0 }
    $sum = 0.0
    foreach ($sample in $Samples) { $sum += [double]$sample.$Name }
    return $sum / $Samples.Count
}

$campaign = [IO.Path]::GetFullPath($CampaignDirectory)
if (-not [IO.Directory]::Exists($campaign)) { throw 'The campaign directory does not exist.' }
$campaignItem = Get-Item -LiteralPath $campaign -Force
if (($campaignItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'The campaign directory cannot be a link or reparse point.'
}
if (-not $AllowStaging.IsPresent -and $campaign.TrimEnd('\', '/').EndsWith('.partial')) {
    throw 'Partial soak evidence cannot satisfy verification.'
}
$runnerScript = Join-Path $PSScriptRoot 'run-wall-clock-soak.ps1'
$verifierScript = [IO.Path]::GetFullPath($PSCommandPath)
if (-not [IO.File]::Exists($runnerScript) -or -not [IO.File]::Exists($verifierScript)) {
    throw 'Current release-source wall-clock scripts are unavailable.'
}
$currentRunnerHash = (Get-FileHash -LiteralPath $runnerScript -Algorithm SHA256).Hash.ToLowerInvariant()
$currentVerifierHash = (Get-FileHash -LiteralPath $verifierScript -Algorithm SHA256).Hash.ToLowerInvariant()

$expectedFiles = @(
    'app-report.ini',
    'campaign.ini',
    'checkpoint.ini',
    'data/incidents.sqlite3',
    'data/product-settings.ini',
    'data/recorder-settings.ini',
    'manifest.sha256.ini',
    'operator-events.tsv',
    'process-samples.tsv',
    'summary.ini'
)
$actualFiles = @(Get-ChildItem -LiteralPath $campaign -Recurse -Force -File)
if ($actualFiles.Count -ne $expectedFiles.Count) {
    throw 'The soak bundle does not contain the exact direct-v1 file set.'
}
$actualRelative = @()
foreach ($file in $actualFiles) {
    if (($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Soak bundle files cannot be links: $($file.FullName)"
    }
    $relative = Get-BundleRelativePath $campaign $file.FullName
    $actualRelative += $relative
    $limit = switch ($relative) {
        'data/incidents.sqlite3' { 1GB }
        'process-samples.tsv' { 32MB }
        'operator-events.tsv' { 64KB }
        default { 1MB }
    }
    if ($file.Length -le 0 -or $file.Length -gt $limit) {
        throw "Soak bundle file violates its size bound: $relative"
    }
}
if (Compare-Object ($expectedFiles | Sort-Object) ($actualRelative | Sort-Object)) {
    throw 'The soak bundle contains an unexpected or missing path.'
}
$directories = @(Get-ChildItem -LiteralPath $campaign -Recurse -Force -Directory)
$directoryRelative = @()
foreach ($directory in $directories) {
    if (($directory.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'Soak bundle directories cannot be links or reparse points.'
    }
    $directoryRelative += Get-BundleRelativePath $campaign $directory.FullName
}
if (Compare-Object @('data', 'data/crashes') ($directoryRelative | Sort-Object)) {
    throw 'The soak bundle must contain only its data and empty crash-staging directories.'
}
if (@(Get-ChildItem -LiteralPath (Join-Path $campaign 'data/crashes') -Force).Count -ne 0) {
    throw 'A passed soak cannot contain crash staging or completed dump evidence.'
}

$manifest = Read-DirectV1 (Join-Path $campaign 'manifest.sha256.ini')
if ((Require-Field $manifest 'algorithm') -cne 'sha256' -or
    (Read-UInt $manifest 'file_count') -ne ($expectedFiles.Count - 1) -or
    $manifest.Count -ne ($expectedFiles.Count + 2)) {
    throw 'The soak manifest header or field count is invalid.'
}
foreach ($relative in $expectedFiles | Where-Object { $_ -ne 'manifest.sha256.ini' }) {
    $expectedHash = Require-Field $manifest $relative
    if ($expectedHash -notmatch '^[0-9a-f]{64}$') {
        throw "Invalid manifest SHA-256: $relative"
    }
    $actualHash = (Get-FileHash -LiteralPath (Join-Path $campaign $relative) `
                               -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -cne $expectedHash) { throw "Changed soak evidence: $relative" }
}

$campaignFields = Read-DirectV1 (Join-Path $campaign 'campaign.ini')
$summary = Read-DirectV1 (Join-Path $campaign 'summary.ini')
$checkpoint = Read-DirectV1 (Join-Path $campaign 'checkpoint.ini')
$report = Read-DirectV1 (Join-Path $campaign 'app-report.ini')
$mode = Require-Field $campaignFields 'mode'
if ($mode -notin @('smoke', 'overnight', '72-hour') -or
    (Require-Field $campaignFields 'state') -cne 'passed' -or
    (Require-Field $summary 'state') -cne 'passed' -or
    (Require-Field $summary 'mode') -cne $mode -or
    (Require-Field $checkpoint 'state') -cne 'completed') {
    throw 'Campaign, summary, and checkpoint state do not describe one passed campaign.'
}
$sourceRevision = Require-Field $campaignFields 'source_revision'
if ($sourceRevision -notmatch '^(local-uncommitted|[0-9a-f]{40}|[0-9a-f]{64})$') {
    throw 'The campaign source revision is malformed.'
}
if ((Require-Field $report 'source_revision') -cne $sourceRevision) {
    throw 'The app report source revision does not match the campaign.'
}
$applicationHash = Require-Field $campaignFields 'application_sha256'
if ($applicationHash -notmatch '^[0-9a-f]{64}$' -or
    (Require-Field $summary 'application_sha256') -cne $applicationHash) {
    throw 'Campaign application provenance is malformed or inconsistent.'
}
foreach ($pair in @(@('runner_sha256', $currentRunnerHash),
                     @('verifier_sha256', $currentVerifierHash))) {
    if ((Require-Field $campaignFields $pair[0]) -cne $pair[1] -or
        (Require-Field $summary $pair[0]) -cne $pair[1]) {
        throw "Campaign harness does not match current release source: $($pair[0])"
    }
}
$faultProbeHash = Require-Field $campaignFields 'archive_fault_probe_sha256'
if (($faultProbeHash -cne 'none' -and $faultProbeHash -notmatch '^[0-9a-f]{64}$') -or
    (Require-Field $summary 'archive_fault_probe_sha256') -cne $faultProbeHash -or
    (Require-Field $summary 'source_revision') -cne $sourceRevision) {
    throw 'Campaign source/fault-probe provenance is malformed or inconsistent.'
}
$requested = Read-UInt $campaignFields 'requested_runtime_seconds'
if (($mode -eq 'overnight' -and $requested -ne 28800) -or
    ($mode -eq '72-hour' -and $requested -ne 259200) -or
    ($mode -eq 'smoke' -and ($requested -lt 10 -or $requested -gt 604800))) {
    throw 'The campaign duration does not satisfy its named mode.'
}
$captureInterval = Read-UInt $campaignFields 'capture_interval_seconds'
$checkpointInterval = Read-UInt $campaignFields 'checkpoint_interval_seconds'
$minimumProcessSamples = Read-UInt $campaignFields 'minimum_process_samples'
$minimumCollections = Read-UInt $campaignFields 'minimum_collections'
$minimumScheduledCaptures = Read-UInt $campaignFields 'minimum_scheduled_captures'
$logicalProcessors = Read-UInt $campaignFields 'logical_processor_count'
if ($captureInterval -lt 1 -or $captureInterval -gt 86400 -or
    $checkpointInterval -lt 1 -or $checkpointInterval -gt 3600 -or
    $checkpointInterval -ge $requested -or $logicalProcessors -lt 1) {
    throw 'Campaign cadence or processor provenance is outside its allowed bound.'
}
if (($mode -eq 'overnight' -and
     ($captureInterval -ne 900 -or $checkpointInterval -ne 60)) -or
    ($mode -eq '72-hour' -and
     ($captureInterval -ne 1800 -or $checkpointInterval -ne 60))) {
    throw 'The named long campaign does not use its fixed cadence.'
}
$expectedCollections = [uint64][math]::Max(
    1, $requested - [math]::Max(5, [math]::Ceiling($requested * 0.05)))
$nominalProcessSamples = [uint64][math]::Max(
    1, [math]::Ceiling($requested / [double]$checkpointInterval) - 1)
$expectedProcessSamples = [uint64][math]::Max(
    1, $nominalProcessSamples - [math]::Max(
        2, [math]::Ceiling($nominalProcessSamples * 0.05)))
$expectedScheduledCaptures = [uint64][math]::Max(
    0, [math]::Ceiling(($requested - 2) / [double]$captureInterval) - 1)
if ($minimumCollections -ne $expectedCollections -or
    $minimumProcessSamples -ne $expectedProcessSamples -or
    $minimumScheduledCaptures -ne $expectedScheduledCaptures) {
    throw 'Published campaign coverage minimums do not match the direct-v1 contract.'
}
foreach ($name in @('requested_runtime_seconds', 'capture_interval_seconds',
                     'checkpoint_interval_seconds', 'minimum_process_samples',
                     'minimum_collections', 'minimum_scheduled_captures',
                     'logical_processor_count')) {
    if ((Require-Field $summary $name) -cne (Require-Field $campaignFields $name)) {
        throw "Campaign and summary qualification fields differ: $name"
    }
}
$observedRuntime = Read-Double $summary 'observed_runtime_seconds'
if ((Read-UInt $report 'requested_runtime_seconds') -ne $requested -or
    (Read-UInt $report 'capture_interval_seconds') -ne $captureInterval -or
    (Read-UInt $report 'collections') -lt $minimumCollections -or
    (Require-Field $report 'completed') -cne '1' -or
    $observedRuntime -lt [double]$requested) {
    throw 'Campaign duration, cadence, coverage, summary, and app report do not agree.'
}

$processLines = [IO.File]::ReadAllLines((Join-Path $campaign 'process-samples.tsv'))
if ($processLines.Count -lt 2 -or $processLines[0] -cne
    "elapsed_seconds`tutc`tworking_set_bytes`tprivate_bytes`thandles`ttotal_cpu_seconds" -or
    [uint64]($processLines.Count - 1) -ne (Read-UInt $summary 'process_samples') -or
    [uint64]($processLines.Count - 1) -lt $minimumProcessSamples) {
    throw 'The process journal header or row count is invalid.'
}
$processSamples = [Collections.Generic.List[object]]::new()
$previousElapsed = -1.0
$previousCpu = -1.0
$samplingGaps = [uint64]0
$maximumWorkingSet = [uint64]0
$maximumPrivateBytes = [uint64]0
$maximumHandles = [uint64]0
foreach ($line in $processLines | Select-Object -Skip 1) {
    $columns = $line.Split("`t")
    [double]$elapsed = 0.0
    [double]$cpu = 0.0
    [uint64]$workingSet = 0
    [uint64]$privateBytes = 0
    [uint64]$handles = 0
    [DateTimeOffset]$utc = [DateTimeOffset]::MinValue
    if ($columns.Count -ne 6 -or
        -not [double]::TryParse($columns[0], [Globalization.NumberStyles]::AllowDecimalPoint,
                                $invariant, [ref]$elapsed) -or $elapsed -lt 0 -or
        -not [DateTimeOffset]::TryParse($columns[1], $invariant,
                                        [Globalization.DateTimeStyles]::RoundtripKind, [ref]$utc) -or
        -not [uint64]::TryParse($columns[2], [ref]$workingSet) -or
        -not [uint64]::TryParse($columns[3], [ref]$privateBytes) -or
        -not [uint64]::TryParse($columns[4], [ref]$handles) -or
        -not [double]::TryParse($columns[5], [Globalization.NumberStyles]::AllowDecimalPoint,
                                $invariant, [ref]$cpu) -or $cpu -lt 0) {
        throw 'The process journal contains a malformed row.'
    }
    if ($previousElapsed -ge 0 -and
        ($elapsed -le $previousElapsed -or $cpu -lt $previousCpu)) {
        throw 'The process journal elapsed or CPU values are not monotonic.'
    }
    if ($previousElapsed -ge 0 -and
        ($elapsed - $previousElapsed) -gt ($checkpointInterval * 3)) {
        $samplingGaps++
    }
    if ($workingSet -gt $maximumWorkingSet) { $maximumWorkingSet = $workingSet }
    if ($privateBytes -gt $maximumPrivateBytes) { $maximumPrivateBytes = $privateBytes }
    if ($handles -gt $maximumHandles) { $maximumHandles = $handles }
    $processSamples.Add([pscustomobject]@{
        Elapsed = $elapsed
        WorkingSet = $workingSet
        PrivateBytes = $privateBytes
        Handles = $handles
        Cpu = $cpu
    })
    $previousElapsed = $elapsed
    $previousCpu = $cpu
}
$sampleCount = [uint64]$processSamples.Count
$checkpointElapsed = Read-UInt $checkpoint 'elapsed_seconds'
if ((Read-UInt $checkpoint 'process_samples') -ne $sampleCount -or
    (Read-UInt $checkpoint 'sampling_gaps') -ne $samplingGaps -or
    (Read-UInt $summary 'sampling_gaps') -ne $samplingGaps -or
    $checkpointElapsed -gt ($observedRuntime + 0.01) -or
    ($observedRuntime - $checkpointElapsed) -gt 1.01 -or
    $processSamples[$processSamples.Count - 1].Elapsed -gt ($observedRuntime + 0.01)) {
    throw 'The checkpoint and summary do not match the recomputed journal timing.'
}
$steadyWindowSize = [math]::Min(10, $processSamples.Count)
$firstSteady = @($processSamples | Select-Object -First $steadyWindowSize)
$lastSteady = @($processSamples | Select-Object -Last $steadyWindowSize)
$workingSetGrowth = [int64][math]::Round(
    (Get-AverageMetric $lastSteady 'WorkingSet') -
    (Get-AverageMetric $firstSteady 'WorkingSet'))
$privateGrowth = [int64][math]::Round(
    (Get-AverageMetric $lastSteady 'PrivateBytes') -
    (Get-AverageMetric $firstSteady 'PrivateBytes'))
$handleGrowth = [int64][math]::Round(
    (Get-AverageMetric $lastSteady 'Handles') -
    (Get-AverageMetric $firstSteady 'Handles'))
$cpuPercent = if ($previousElapsed -gt 0) {
    ($previousCpu / ($previousElapsed * $logicalProcessors)) * 100.0
} else { 0.0 }
if ((Read-UInt $summary 'maximum_working_set_bytes') -ne $maximumWorkingSet -or
    (Read-UInt $summary 'maximum_private_bytes') -ne $maximumPrivateBytes -or
    (Read-UInt $summary 'maximum_handles') -ne $maximumHandles -or
    (Require-Field $summary 'average_total_machine_cpu_percent') -cne
        $cpuPercent.ToString('F6', $invariant) -or
    (Read-UInt $summary 'steady_state_window_samples') -ne [uint64]$steadyWindowSize -or
    (Read-Int $summary 'steady_state_working_set_growth_bytes') -ne $workingSetGrowth -or
    (Read-Int $summary 'steady_state_private_bytes_growth') -ne $privateGrowth -or
    (Read-Int $summary 'steady_state_handle_growth') -ne $handleGrowth) {
    throw 'The summary does not match independently recomputed process metrics.'
}

$eventLines = [IO.File]::ReadAllLines((Join-Path $campaign 'operator-events.tsv'))
if ($eventLines.Count -lt 1 -or $eventLines.Count -gt 1025 -or
    $eventLines[0] -cne "utc`tevent") {
    throw 'The operator journal header or bound is invalid.'
}
$events = @()
foreach ($line in $eventLines | Select-Object -Skip 1) {
    $columns = $line.Split("`t")
    [DateTimeOffset]$utc = [DateTimeOffset]::MinValue
    if ($columns.Count -ne 2 -or
        -not [DateTimeOffset]::TryParse($columns[0], $invariant,
                                        [Globalization.DateTimeStyles]::RoundtripKind, [ref]$utc) -or
        $columns[1] -notin @('sleep_resume', 'lock_unlock', 'device_churn',
                             'archive_fault_started', 'archive_recovered')) {
        throw 'The operator journal contains a malformed or unknown event.'
    }
    $events += $columns[1]
}

if ((Require-Field $report 'archive_healthy') -cne '1' -or
    (Require-Field $report 'archive_schema_version') -cne '1') {
    throw 'The app report does not prove a healthy direct-v1 archive.'
}
if ((Require-Field $report 'sampling_thread_prepared') -cne '1') {
    throw 'The app report does not prove sampling-thread preparation.'
}
Assert-SchedulingDropEvidence $report
Require-Zero $report @('failed_samples', 'dropped_samples', 'deadline_misses',
                        'collector_worker_failures', 'snapshot_failures',
                        'capture_queue_rejections', 'event_worker_failures',
                        'native_events_dropped', 'writer_cancelled',
                        'automatic_detection_enabled',
                        'automatic_detector_triggers',
                        'automatic_captures_started',
                        'automatic_event_requests')
$eventCount = (Read-UInt $report 'power_events_recorded') +
              (Read-UInt $report 'device_events_recorded') +
              (Read-UInt $report 'audio_events_recorded') +
              (Read-UInt $report 'service_events_recorded') +
              (Read-UInt $report 'security_events_recorded') +
              (Read-UInt $report 'update_events_recorded') +
              (Read-UInt $report 'application_events_recorded') +
              (Read-UInt $report 'network_events_recorded') +
              (Read-UInt $report 'graphics_events_recorded') +
              (Read-UInt $report 'storage_events_recorded')
if ($eventCount -ne (Read-UInt $report 'system_events_recorded') -or
    (Read-UInt $report 'writer_succeeded') + (Read-UInt $report 'writer_failed') -ne
        (Read-UInt $report 'incidents_completed') -or
    (Read-UInt $report 'archive_incidents') -ne (Read-UInt $report 'writer_succeeded') -or
    (Read-UInt $report 'incidents_completed') -lt $minimumScheduledCaptures) {
    throw 'Event, capture, writer, and archive accounting is inconsistent.'
}

$faultExercised = Read-UInt $summary 'archive_fault_exercised'
if ($faultExercised -gt 1) { throw 'archive_fault_exercised must be zero or one.' }
if ($faultExercised -eq 0) {
    if ($faultProbeHash -cne 'none') { throw 'An unexercised fault probe must not claim a hash.' }
    Require-Zero $report @('writer_retry_exhausted', 'writer_failed')
} elseif ($faultProbeHash -eq 'none' -or
          (Read-UInt $report 'writer_retry_attempts') -lt 1 -or
          (Read-UInt $report 'writer_retry_exhausted') -lt 1 -or
          (Read-UInt $report 'writer_failed') -lt 1 -or
          (Read-UInt $report 'writer_recoveries') -lt 1 -or
          (Require-Field $report 'recoverable_incident_available') -cne '1' -or
          'archive_fault_started' -notin $events -or 'archive_recovered' -notin $events) {
    throw 'Archive-fault evidence is incomplete or uncorroborated.'
}

if ($mode -ne 'smoke') {
    if ($maximumWorkingSet -gt 80MB -or $cpuPercent -gt 1.0 -or
        $steadyWindowSize -ne 10 -or $workingSetGrowth -gt 16MB -or
        $privateGrowth -gt 16MB -or $handleGrowth -gt 32) {
        throw 'The long-campaign resource or steady-state growth gate failed.'
    }
}
if ($mode -eq '72-hour') {
    foreach ($requiredEvent in @('sleep_resume', 'lock_unlock', 'device_churn',
                                  'archive_fault_started', 'archive_recovered')) {
        if ($requiredEvent -notin $events) { throw "Missing required event: $requiredEvent" }
    }
    if ((Read-UInt $report 'resume_events') -lt 1 -or
        (Require-Field $report 'session_notifications_available') -cne '1' -or
        (Read-UInt $report 'session_locks') -lt 1 -or
        (Read-UInt $report 'session_unlocks') -lt 1 -or
        (Read-UInt $report 'device_events_recorded') +
            (Read-UInt $report 'audio_events_recorded') -lt 1) {
        throw 'The 72-hour operator attestations lack independent native corroboration.'
    }
}

$archivePath = Join-Path $campaign 'data/incidents.sqlite3'
$archiveStream = [IO.File]::OpenRead($archivePath)
try {
    $header = [byte[]]::new(16)
    if ($archiveStream.Read($header, 0, $header.Length) -ne $header.Length -or
        [Text.Encoding]::ASCII.GetString($header) -cne "SQLite format 3`0") {
        throw 'The bound archive does not have a SQLite format-3 header.'
    }
} finally {
    $archiveStream.Dispose()
}

Write-Output "Verified passed wall-clock $mode campaign: $campaign"
