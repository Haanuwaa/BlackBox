[CmdletBinding()]
param(
    [ValidateSet('smoke', 'overnight', '72-hour')]
    [string]$Mode = 'smoke',
    [ValidateSet('isolated', 'defaults')]
    [string]$Profile = 'isolated',

    [string]$ApplicationPath =
        (Join-Path $PSScriptRoot '..\out\build\windows-vs2026-release\src\Release\blackbox.exe'),

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [int]$DurationSeconds = 0,
    [int]$CaptureIntervalSeconds = 0,
    [int]$CheckpointSeconds = 0,
    [string]$ArchiveFaultProbePath = '',
    [string]$SourceRevision = 'local-uncommitted',
    [switch]$ExerciseArchiveFault
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$invariant = [Globalization.CultureInfo]::InvariantCulture

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

function Add-JournalEvent([string]$Path, [string]$Event) {
    $line = "{0}`t{1}`r`n" -f [DateTimeOffset]::UtcNow.ToString('O'), $Event
    $bytes = [Text.Encoding]::UTF8.GetBytes($line)
    for ($attempt = 1; $attempt -le 20; ++$attempt) {
        try {
            $stream = [IO.File]::Open($Path, [IO.FileMode]::Append,
                                      [IO.FileAccess]::Write, [IO.FileShare]::Read)
            try {
                $stream.Write($bytes, 0, $bytes.Length)
                $stream.Flush($true)
                return
            } finally {
                $stream.Dispose()
            }
        } catch [IO.IOException] {
            if ($attempt -eq 20) { throw }
            Start-Sleep -Milliseconds 50
        }
    }
}

function Get-AverageMetric([object[]]$Samples, [string]$Name) {
    if ($Samples.Count -eq 0) { return 0.0 }
    $sum = 0.0
    foreach ($sample in $Samples) { $sum += [double]$sample.$Name }
    return $sum / $Samples.Count
}

function Read-DirectV1([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { throw "Missing direct-v1 artifact: $Path" }
    $fields = @{}
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ([string]::IsNullOrWhiteSpace($line)) { throw 'Direct-v1 artifacts cannot contain blank lines.' }
        $separator = $line.IndexOf('=')
        if ($separator -lt 1) { throw 'Direct-v1 artifact contains a malformed field.' }
        $name = $line.Substring(0, $separator)
        if ($fields.ContainsKey($name)) { throw "Duplicate direct-v1 field: $name" }
        $fields[$name] = $line.Substring($separator + 1)
    }
    if ($fields['format'] -ne '1') { throw 'Artifact is not direct format v1.' }
    return $fields
}

function Require-Zero($Fields, [string[]]$Names) {
    foreach ($name in $Names) {
        if (-not $Fields.ContainsKey($name) -or [uint64]$Fields[$name] -ne 0) {
            throw "Soak gate requires $name=0."
        }
    }
}

$defaults = @{
    smoke = @{ Duration = 60; Capture = 10; Checkpoint = 5 }
    overnight = @{ Duration = 28800; Capture = 900; Checkpoint = 60 }
    '72-hour' = @{ Duration = 259200; Capture = 1800; Checkpoint = 60 }
}
if ($Profile -eq 'defaults') {
    if ($Mode -ne 'smoke' -or $ExerciseArchiveFault) { throw 'Default-profile rehearsal is a separate smoke campaign without fault injection.' }
    if ($DurationSeconds -eq 0) { $DurationSeconds = 240 }
    if ($DurationSeconds -lt 220) { throw 'Default-profile rehearsal requires at least 220 seconds.' }
    if ($CaptureIntervalSeconds -eq 0) { $CaptureIntervalSeconds = 180 }
    if ($CaptureIntervalSeconds -lt 150) { throw 'Default-profile captures need at least 150 seconds between markers.' }
}
if ($DurationSeconds -eq 0) { $DurationSeconds = $defaults[$Mode].Duration }
if ($CaptureIntervalSeconds -eq 0) { $CaptureIntervalSeconds = $defaults[$Mode].Capture }
if ($CheckpointSeconds -eq 0) { $CheckpointSeconds = $defaults[$Mode].Checkpoint }
if ($DurationSeconds -lt 10 -or $DurationSeconds -gt 604800) {
    throw 'DurationSeconds must be between 10 and 604800.'
}
if ($Mode -ne 'smoke' -and $DurationSeconds -ne $defaults[$Mode].Duration) {
    throw 'Only smoke campaigns permit a shortened duration.'
}
if ($CaptureIntervalSeconds -lt 1 -or $CaptureIntervalSeconds -gt 86400) {
    throw 'CaptureIntervalSeconds must be between 1 and 86400.'
}
if ($CheckpointSeconds -lt 1 -or $CheckpointSeconds -gt 3600) {
    throw 'CheckpointSeconds must be between 1 and 3600.'
}
if ($CheckpointSeconds -ge $DurationSeconds) {
    throw 'CheckpointSeconds must be shorter than the campaign duration.'
}
if ($Mode -ne 'smoke' -and
    ($CaptureIntervalSeconds -ne $defaults[$Mode].Capture -or
     $CheckpointSeconds -ne $defaults[$Mode].Checkpoint)) {
    throw 'Named long campaigns require their fixed capture and checkpoint cadences.'
}
if ($SourceRevision -notmatch '^(local-uncommitted|[0-9A-Fa-f]{40}|[0-9A-Fa-f]{64})$') {
    throw 'SourceRevision must be local-uncommitted or a 40/64-digit hexadecimal revision.'
}
$SourceRevision = $SourceRevision.ToLowerInvariant()
$minimumCollections = [uint64][math]::Max(
    1, $DurationSeconds - [math]::Max(5, [math]::Ceiling($DurationSeconds * 0.05)))
$nominalProcessSamples = [uint64][math]::Max(
    1, [math]::Ceiling($DurationSeconds / [double]$CheckpointSeconds) - 1)
$minimumProcessSamples = [uint64][math]::Max(
    1, $nominalProcessSamples - [math]::Max(
        2, [math]::Ceiling($nominalProcessSamples * 0.05)))
# Production requires the configured one-second post-window plus one safety second to
# complete before diagnostic exit. Count only interval boundaries that satisfy that rule.
$minimumScheduledCaptures = [uint64][math]::Max(
    0, [math]::Ceiling(($DurationSeconds - 2) / [double]$CaptureIntervalSeconds) - 1)
if ($Profile -eq 'defaults') { $minimumScheduledCaptures = [uint64]1 }
$logicalProcessors = [uint64][Environment]::ProcessorCount
if ($logicalProcessors -lt 1) { throw 'The logical processor count is unavailable.' }

$application = [IO.Path]::GetFullPath($ApplicationPath)
$runnerScript = [IO.Path]::GetFullPath($PSCommandPath)
$verifierScript = Join-Path $PSScriptRoot 'verify-wall-clock-soak.ps1'
$faultProbe = if ([string]::IsNullOrWhiteSpace($ArchiveFaultProbePath)) {
    Join-Path ([IO.Path]::GetDirectoryName($application)) 'blackbox_soak_archive_fault.exe'
} else {
    [IO.Path]::GetFullPath($ArchiveFaultProbePath)
}
$faultRequired = $Mode -eq '72-hour' -or $ExerciseArchiveFault.IsPresent
$output = [IO.Path]::GetFullPath($OutputDirectory)
$staging = "$output.partial"
if (-not [IO.File]::Exists($application)) { throw 'The assembled application does not exist.' }
if ($faultRequired -and -not [IO.File]::Exists($faultProbe)) {
    throw 'This campaign requires the isolated archive-fault probe.'
}
if ($faultRequired) {
    $probeIdentity = [Diagnostics.FileVersionInfo]::GetVersionInfo($faultProbe)
    if ($probeIdentity.Comments -cne "source_revision=$SourceRevision" -or
        $probeIdentity.OriginalFilename -cne 'blackbox_soak_archive_fault.exe') {
        throw 'Preflight fault-probe source revision or binary identity mismatch.'
    }
}
if (-not [IO.File]::Exists($runnerScript) -or -not [IO.File]::Exists($verifierScript)) {
    throw 'The wall-clock runner and verifier scripts must both exist.'
}
if ([IO.Directory]::Exists($output) -or [IO.File]::Exists($output) -or
    [IO.Directory]::Exists($staging) -or [IO.File]::Exists($staging)) {
    throw 'The campaign output and staging destinations must not already exist.'
}
# Reject a stale executable before creating evidence or starting any diagnostic clock.
$identity = [Diagnostics.FileVersionInfo]::GetVersionInfo($application)
if ($identity.Comments -cne "source_revision=$SourceRevision" -or
    $identity.OriginalFilename -cne 'blackbox.exe') {
    throw 'Preflight application source revision or binary identity mismatch.'
}
if ($faultRequired -and ($CaptureIntervalSeconds -lt 10 -or
    $CheckpointSeconds -gt [math]::Floor($CaptureIntervalSeconds / 2) -or
    $DurationSeconds -lt ($CaptureIntervalSeconds * 4 + 3))) {
    throw 'Fault rehearsal needs at least four captures, a ten-second capture interval, and checkpoints at most half that interval.'
}
$applicationHash = (Get-FileHash -LiteralPath $application -Algorithm SHA256).Hash.ToLowerInvariant()
$runnerHash = (Get-FileHash -LiteralPath $runnerScript -Algorithm SHA256).Hash.ToLowerInvariant()
$verifierHash = (Get-FileHash -LiteralPath $verifierScript -Algorithm SHA256).Hash.ToLowerInvariant()
$faultProbeHash = if ($faultRequired) {
    (Get-FileHash -LiteralPath $faultProbe -Algorithm SHA256).Hash.ToLowerInvariant()
} else { 'none' }
$provenance = "source_revision=$SourceRevision`napplication_sha256=$applicationHash`n" +
              "runner_sha256=$runnerHash`nverifier_sha256=$verifierHash`n" +
              "archive_fault_probe_sha256=$faultProbeHash`n"
$campaignContract = "profile=$Profile`n" + "capture_interval_seconds=$CaptureIntervalSeconds`n" +
                    "checkpoint_interval_seconds=$CheckpointSeconds`n" +
                    "minimum_process_samples=$minimumProcessSamples`n" +
                    "minimum_collections=$minimumCollections`n" +
                    "minimum_scheduled_captures=$minimumScheduledCaptures`n" +
                    "logical_processor_count=$logicalProcessors`n"

[IO.Directory]::CreateDirectory($staging) | Out-Null
# Run an isolated immutable copy, including every adjacent runtime DLL.
$runtime = Join-Path $staging 'runtime'
[IO.Directory]::CreateDirectory($runtime) | Out-Null
$runtimeSources = @($application) + @(Get-ChildItem -LiteralPath ([IO.Path]::GetDirectoryName($application)) -Filter '*.dll' -File | ForEach-Object { $_.FullName })
if ($faultRequired) { $runtimeSources += $faultProbe }
$workloadProbe = Join-Path ([IO.Path]::GetDirectoryName($application)) 'blackbox_default_profile_workload.exe'
if ($Profile -eq 'defaults') {
    $workloadIdentity = [Diagnostics.FileVersionInfo]::GetVersionInfo($workloadProbe)
    if ($workloadIdentity.Comments -cne "source_revision=$SourceRevision") { throw 'Default workload identity mismatch.' }
    $runtimeSources += $workloadProbe
}
if ($runtimeSources.Count -gt 32) { throw 'Runtime inventory exceeds 32 files.' }
$runtimeHashes = [ordered]@{}
foreach ($source in $runtimeSources) {
    $item = Get-Item -LiteralPath $source -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        $item.Length -le 0 -or $item.Length -gt 200MB -or
        $item.Name -notmatch '^[A-Za-z0-9_.-]+\.(exe|dll)$') {
        throw 'Runtime files must be bounded regular executable or DLL files.'
    }
    $relative = 'runtime/' + $item.Name
    if ($runtimeHashes.Contains($relative)) { throw 'Duplicate runtime filename.' }
    [IO.File]::Copy($source, (Join-Path $staging $relative), $false)
    $runtimeHashes[$relative] = (Get-FileHash -LiteralPath (Join-Path $staging $relative) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$inventory = @('format=1', "file_count=$($runtimeHashes.Count)")
foreach ($relative in $runtimeHashes.Keys) { $inventory += "$relative=$($runtimeHashes[$relative])" }
Write-AtomicText (Join-Path $staging 'runtime-inventory.ini') (($inventory -join "`n") + "`n")
$application = Join-Path $runtime ([IO.Path]::GetFileName($application))
if ($faultRequired) { $faultProbe = Join-Path $runtime ([IO.Path]::GetFileName($faultProbe)) }
$workloadProbe = Join-Path $runtime 'blackbox_default_profile_workload.exe'
$data = Join-Path $staging 'data'
[IO.Directory]::CreateDirectory($data) | Out-Null
$productSettings = Join-Path $data 'product-settings.ini'
$recorderSettings = Join-Path $data 'recorder-settings.ini'
$archive = Join-Path $data 'incidents.sqlite3'
$report = Join-Path $staging 'app-report.ini'
$samples = Join-Path $staging 'process-samples.tsv'
$events = Join-Path $staging 'operator-events.tsv'
$campaign = Join-Path $staging 'campaign.ini'

$archiveField = $archive.Replace('\', '/')
$productText = @"
format=1
hotkey_key=10
hotkey_control=1
hotkey_shift=1
hotkey_alt=1
hotkey_system_modifier=0
automatic_detection=0
detector_sensitivity=1
detect_cpu=1
detect_memory=1
detect_disk=1
detect_network=1
detector_cooldown_seconds=120
notifications=0
record_foreground_application=0
record_process_lifecycle=0
record_power_and_device_events=1
record_audio_device_events=1
record_system_event_evidence=0
archive_path=$archiveField
archive_maximum_bytes=1073741824
onboarding_completed=1
"@
$recorderText = @"
format=1
sample_interval_ms=1000
history_duration_ms=60000
late_tolerance_ms=250
metadata_interval_ms=30000
incident_pre_window_ms=1000
incident_post_window_ms=1000
resume_gap_threshold_ms=5000
collect_process_paths=0
"@
if ($Profile -eq 'defaults') {
    $productText = $productText.Replace('automatic_detection=0', 'automatic_detection=1').Replace('notifications=0', 'notifications=1').Replace('record_power_and_device_events=1', 'record_power_and_device_events=0').Replace('record_audio_device_events=1', 'record_audio_device_events=0')
    $recorderText = $recorderText.Replace('history_duration_ms=60000', 'history_duration_ms=300000').Replace('incident_pre_window_ms=1000', 'incident_pre_window_ms=120000').Replace('incident_post_window_ms=1000', 'incident_post_window_ms=30000').Replace('collect_process_paths=0', 'collect_process_paths=1')
}
Write-AtomicText $productSettings $productText
Write-AtomicText $recorderSettings $recorderText
Write-AtomicText $campaign (
    "format=1`nstate=running`nmode=$Mode`nrequested_runtime_seconds=$DurationSeconds`n" +
    $campaignContract + $provenance)
[IO.File]::WriteAllText($samples,
    "elapsed_seconds`tutc`tworking_set_bytes`tprivate_bytes`thandles`ttotal_cpu_seconds`r`n",
    [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText($events, "utc`tevent`r`n", [Text.UTF8Encoding]::new($false))

$oldProduct = [Environment]::GetEnvironmentVariable('BLACKBOX_PRODUCT_SETTINGS_PATH', 'Process')
$oldRecorder = [Environment]::GetEnvironmentVariable('BLACKBOX_SETTINGS_PATH', 'Process')
$process = $null
$faultProcess = $null
$faultStarted = $false
$faultRecovered = $false
$settingsProbe = $null
$workloadProcess = $null
$visibilityStep = 0
$ownedWindow = [IntPtr]::Zero
if ($Profile -eq 'defaults' -and -not ('BlackBoxRehearsalWindow' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class BlackBoxRehearsalWindow {
    private delegate bool WindowCallback(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] private static extern bool EnumWindows(WindowCallback callback, IntPtr data);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetWindowText(IntPtr window, System.Text.StringBuilder text, int count);
    public static IntPtr Find(uint process) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((window, data) => {
            uint owner; GetWindowThreadProcessId(window, out owner);
            var title = new System.Text.StringBuilder(256); GetWindowText(window, title, 256);
            if (owner == process && title.ToString().StartsWith("BlackBox")) { result = window; return false; }
            return true;
        }, IntPtr.Zero);
        return result;
    }
}
'@
}
$lastProgressAt = 0.0
$lastCollections = [uint64]0
$nextSampleAt = [double]$CheckpointSeconds
$nextStatusAt = 0.0
$stopwatch = [Diagnostics.Stopwatch]::new()
$lastCpu = 0.0
$maximumWorkingSet = [uint64]0
$maximumPrivateBytes = [uint64]0
$maximumHandles = [uint64]0
$sampleCount = [uint64]0
$samplingGaps = [uint64]0
$previousSampleAt = 0.0
$steadyWindowSize = 10
$firstSteadySamples = [Collections.Generic.List[object]]::new()
$lastSteadySamples = [Collections.Generic.Queue[object]]::new()

try {
    [Environment]::SetEnvironmentVariable('BLACKBOX_PRODUCT_SETTINGS_PATH', $productSettings, 'Process')
    [Environment]::SetEnvironmentVariable('BLACKBOX_SETTINGS_PATH', $recorderSettings, 'Process')
    $settingsProbe = Start-Process -FilePath $application `
                                   -ArgumentList @('--validate-settings-only') `
                                   -WindowStyle Hidden -PassThru
    if (-not $settingsProbe.WaitForExit(30000)) {
        Stop-Process -Id $settingsProbe.Id -Force -ErrorAction SilentlyContinue
        throw 'Settings preflight exceeded its 30-second watchdog.'
    }
    $settingsProbe.Refresh()
    if ($settingsProbe.ExitCode -ne 0) {
        throw 'The assembled application rejected the generated direct-v1 settings.'
    }
    $arguments = @(
        "--background-diagnostic-seconds=$DurationSeconds",
        ('"--diagnostic-report={0}"' -f $report),
        "--diagnostic-capture-interval-seconds=$CaptureIntervalSeconds"
    )
    if ($faultRequired) { $arguments += '--diagnostic-recover-failed' }
    if ($Profile -eq 'defaults') { $arguments += '--diagnostic-overlap-automatic' }
    $process = Start-Process -FilePath $application -ArgumentList $arguments -WindowStyle Hidden -PassThru
    $stopwatch.Start()
    [Environment]::SetEnvironmentVariable('BLACKBOX_PRODUCT_SETTINGS_PATH', $oldProduct, 'Process')
    [Environment]::SetEnvironmentVariable('BLACKBOX_SETTINGS_PATH', $oldRecorder, 'Process')

    while (-not $process.HasExited) {
        if ($process.WaitForExit([math]::Min(5, $CheckpointSeconds) * 1000)) { break }
        $process.Refresh()
        if ($process.HasExited) { break }
        $elapsed = $stopwatch.Elapsed.TotalSeconds
        if ($elapsed -gt ($DurationSeconds + 60)) {
            throw 'Application exceeded the runtime plus 60-second drain watchdog.'
        }
        $progressPath = Join-Path $staging 'app-progress.ini'
        $progress = $null
        try {
            if ((Get-Item -LiteralPath $progressPath -ErrorAction Stop).Length -le 1MB) {
                $candidate = Read-DirectV1 $progressPath
                if ($candidate['complete'] -eq '1') { $progress = $candidate }
            }
        } catch { } # A writer may be replacing a snapshot; retry at the next tick.
        if ($null -ne $progress) {
            if ($progress['source_revision'] -cne $SourceRevision) { throw 'Live progress has the wrong binary identity.' }
            $collections = [uint64]$progress['collections']
            if ($collections -gt $lastCollections) { $lastProgressAt = $elapsed; $lastCollections = $collections }
            if ([uint64]$progress['collector_worker_failures'] -ne 0 -or
                [uint64]$progress['dropped_samples'] -ne 0 -or
                [uint64]$progress['writer_failed'] -gt $(if ($faultRequired) { 1 } else { 0 })) {
                throw 'Live progress recorded a terminal collector or unexpected writer failure.'
            }
        }
        if (($elapsed - $lastProgressAt) -gt 120) { throw 'Collection progress stalled for more than 120 seconds.' }
        if ($elapsed -ge $nextStatusAt) {
            Write-Host ("Elapsed {0:N0}/{1}s; collections {2}; evidence {3}" -f $elapsed, $DurationSeconds, $lastCollections, $staging)
            if ($Mode -eq '72-hour') {
                $journal = [IO.File]::ReadAllText($events)
                $missing = @('sleep_resume', 'lock_unlock', 'device_churn') | Where-Object { $journal -notmatch "`t$_(?:`r?`n|$)" }
                if ($missing) { Write-Host ('Operator events still needed: ' + ($missing -join ', ')) }
            }
            $nextStatusAt = $elapsed + 300
        }
        if ($Profile -eq 'defaults' -and $elapsed -ge 130 -and $null -eq $workloadProcess) {
            Write-Host 'Starting the bounded 20-second CPU workload after the full pre-window warmup.'
            $workloadProcess = Start-Process -FilePath $workloadProbe -WindowStyle Hidden -PassThru
        }
        if ($Profile -eq 'defaults' -and $visibilityStep -lt 3 -and $elapsed -ge (15 + $visibilityStep * 20)) {
            if ($ownedWindow -eq [IntPtr]::Zero) { $ownedWindow = [BlackBoxRehearsalWindow]::Find([uint32]$process.Id) }
            if ($ownedWindow -eq [IntPtr]::Zero) { throw 'Rehearsal could not find its own application window.' }
            $visible = [BlackBoxRehearsalWindow]::IsWindowVisible($ownedWindow)
            $minimized = [BlackBoxRehearsalWindow]::IsIconic($ownedWindow)
            $matched = ($visibilityStep -eq 0 -and $visible -and -not $minimized) -or
                       ($visibilityStep -eq 1 -and $minimized) -or
                       ($visibilityStep -eq 2 -and -not $visible)
            if ($matched) {
                Add-JournalEvent $events @('ui_visible', 'ui_minimized', 'ui_hidden')[$visibilityStep]
                $visibilityStep++
            } else {
                [void][BlackBoxRehearsalWindow]::ShowWindowAsync($ownedWindow, @(4, 6, 0)[$visibilityStep])
            }
        }
        if ($elapsed -lt $nextSampleAt) { continue }
        $nextSampleAt = $elapsed + $CheckpointSeconds
        $journalElapsed = [double]::Parse(
            $elapsed.ToString('F3', $invariant), $invariant)
        $cpu = $process.TotalProcessorTime.TotalSeconds
        $journalCpu = [double]::Parse($cpu.ToString('F6', $invariant), $invariant)
        if ($previousSampleAt -gt 0 -and
            ($journalElapsed - $previousSampleAt) -gt ($CheckpointSeconds * 3)) {
            $samplingGaps++
        }
        $previousSampleAt = $journalElapsed
        $lastCpu = $journalCpu
        $workingSet = [uint64]$process.WorkingSet64
        $privateBytes = [uint64]$process.PrivateMemorySize64
        $handles = [uint64]$process.HandleCount
        if ($workingSet -gt $maximumWorkingSet) { $maximumWorkingSet = $workingSet }
        if ($privateBytes -gt $maximumPrivateBytes) { $maximumPrivateBytes = $privateBytes }
        if ($handles -gt $maximumHandles) { $maximumHandles = $handles }
        $steadySample = [pscustomobject]@{
            WorkingSet = $workingSet
            PrivateBytes = $privateBytes
            Handles = $handles
        }
        if ($firstSteadySamples.Count -lt $steadyWindowSize) {
            $firstSteadySamples.Add($steadySample)
        }
        $lastSteadySamples.Enqueue($steadySample)
        if ($lastSteadySamples.Count -gt $steadyWindowSize) {
            [void]$lastSteadySamples.Dequeue()
        }
        $sampleCount++
        $line = [string]::Format($invariant,
            "{0:F3}`t{1}`t{2}`t{3}`t{4}`t{5:F6}`r`n",
            [object[]]@($journalElapsed, [DateTimeOffset]::UtcNow.ToString('O'), $workingSet,
                       $privateBytes, $handles, $journalCpu))
        [IO.File]::AppendAllText($samples, $line, [Text.UTF8Encoding]::new($false))
        Write-AtomicText (Join-Path $staging 'checkpoint.ini') (
            "format=1`nstate=running`nelapsed_seconds=$([math]::Floor($elapsed))`n" +
            "process_samples=$sampleCount`nsampling_gaps=$samplingGaps`n")
        if ($faultRequired -and -not $faultStarted -and $null -ne $progress -and
            [uint64]$progress['writer_succeeded'] -eq 1 -and
            [uint64]$progress['elapsed_seconds'] -ge
                (($CaptureIntervalSeconds * 2) - [math]::Max(5, $CheckpointSeconds) - 5)) {
            $holdSeconds = if ($Mode -eq '72-hour') {
                [math]::Max(20, $CheckpointSeconds * 2)
            } else {
                $CaptureIntervalSeconds
            }
            $faultProcess = Start-Process -FilePath $faultProbe `
                                          -ArgumentList @(('"{0}"' -f $archive), $holdSeconds) `
                                          -WindowStyle Hidden -PassThru
            $faultStarted = $true
            Add-JournalEvent $events 'archive_fault_started'
        }
        if ($faultProcess -ne $null -and -not $faultRecovered -and $faultProcess.HasExited) {
            if ($faultProcess.ExitCode -ne 0) { throw 'The isolated archive-fault probe failed.' }
            $faultRecovered = $true
            Add-JournalEvent $events 'archive_recovered'
        }
    }
    $process.WaitForExit()
    $process.Refresh()
    $stopwatch.Stop()
    if ($process.ExitCode -ne 0) { throw "Application exited with status $($process.ExitCode)." }
    if ($sampleCount -lt $minimumProcessSamples) {
        throw 'The process journal did not retain its minimum checkpoint coverage.'
    }

    $fields = Read-DirectV1 $report
    if ($fields['completed'] -ne '1' -or
        $fields['source_revision'] -cne $SourceRevision -or
        [uint64]$fields['requested_runtime_seconds'] -ne [uint64]$DurationSeconds -or
        [uint64]$fields['capture_interval_seconds'] -ne [uint64]$CaptureIntervalSeconds) {
        throw 'Application report does not match the completed campaign.'
    }
    if ([uint64]$fields['collections'] -lt [uint64]$minimumCollections) {
        throw 'The app did not collect the minimum expected wall-clock samples.'
    }
    if ($fields['sampling_thread_prepared'] -ne '1') {
        throw 'The application did not prepare its sampling thread.'
    }
    if ([uint64]$fields['incidents_completed'] -lt $minimumScheduledCaptures) {
        throw 'The app did not complete the minimum scheduled diagnostic captures.'
    }
    Require-Zero $fields @('failed_samples', 'dropped_samples', 'deadline_misses',
                            'collector_worker_failures', 'snapshot_failures',
                            'capture_queue_rejections', 'event_worker_failures',
                            'native_events_dropped', 'writer_cancelled',
                            'unclassified_long_gaps', 'unclassified_skipped_samples',
                            'writer_failed_incidents_not_retained')
    if ($Profile -eq 'isolated') {
        Require-Zero $fields @('automatic_detection_enabled', 'automatic_detector_triggers', 'automatic_captures_started', 'automatic_event_requests')
    } elseif ($fields['automatic_detection_enabled'] -ne '1' -or
              [uint64]$fields['automatic_detector_triggers'] -lt 1 -or
              [uint64]$fields['automatic_captures_started'] -lt 1) {
        throw 'The default-profile workload did not produce an actual automatic capture.'
    }
    if (-not $faultRequired) {
        Require-Zero $fields @('writer_retry_exhausted', 'writer_failed', 'writer_explicit_recoveries')
    }
    if ($fields['archive_healthy'] -ne '1' -or $fields['archive_schema_version'] -ne '1') {
        throw 'The isolated direct-v1 archive did not finish healthy.'
    }
    $categorizedEvents = [uint64]$fields['power_events_recorded'] +
                         [uint64]$fields['device_events_recorded'] +
                         [uint64]$fields['audio_events_recorded'] +
                         [uint64]$fields['service_events_recorded'] +
                         [uint64]$fields['security_events_recorded'] +
                         [uint64]$fields['update_events_recorded'] +
                         [uint64]$fields['application_events_recorded'] +
                         [uint64]$fields['network_events_recorded'] +
                         [uint64]$fields['graphics_events_recorded'] +
                         [uint64]$fields['storage_events_recorded']
    if ($categorizedEvents -ne [uint64]$fields['system_events_recorded']) {
        throw 'Event-source counters do not account for every recorded event.'
    }
    $accountedIncidents = [uint64]$fields['writer_succeeded'] + [uint64]$fields['writer_failed'] - [uint64]$fields['writer_explicit_recoveries']
    if ($accountedIncidents -ne [uint64]$fields['incidents_completed'] -or
        [uint64]$fields['archive_incidents'] -ne [uint64]$fields['writer_succeeded']) {
        throw 'Capture, writer, and archive counts do not agree.'
    }

    $cpuPercent = if ($sampleCount -gt 0 -and $previousSampleAt -gt 0) {
        ($lastCpu / ($previousSampleAt * $logicalProcessors)) * 100.0
    } else { 0.0 }
    if ($Mode -ne 'smoke') {
        if ($maximumWorkingSet -gt 80MB) { throw 'Working-set gate exceeded 80 MiB.' }
        if ($cpuPercent -gt 1.0) { throw 'Hidden wall-clock CPU gate exceeded 1% total-machine capacity.' }
        if ($firstSteadySamples.Count -ne $steadyWindowSize -or
            $lastSteadySamples.Count -ne $steadyWindowSize) {
            throw 'Long campaigns require complete first/last steady-state windows.'
        }
    }

    $firstSteady = @($firstSteadySamples)
    $lastSteady = @($lastSteadySamples)
    $firstWorkingSetAverage = Get-AverageMetric $firstSteady 'WorkingSet'
    $lastWorkingSetAverage = Get-AverageMetric $lastSteady 'WorkingSet'
    $firstPrivateAverage = Get-AverageMetric $firstSteady 'PrivateBytes'
    $lastPrivateAverage = Get-AverageMetric $lastSteady 'PrivateBytes'
    $firstHandleAverage = Get-AverageMetric $firstSteady 'Handles'
    $lastHandleAverage = Get-AverageMetric $lastSteady 'Handles'
    $workingSetGrowth = [int64][math]::Round($lastWorkingSetAverage - $firstWorkingSetAverage)
    $privateGrowth = [int64][math]::Round($lastPrivateAverage - $firstPrivateAverage)
    $handleGrowth = [int64][math]::Round($lastHandleAverage - $firstHandleAverage)
    if ($Mode -ne 'smoke') {
        if ($workingSetGrowth -gt 16MB) {
            throw 'Steady-state working-set growth exceeded 16 MiB.'
        }
        if ($privateGrowth -gt 16MB) {
            throw 'Steady-state private-memory growth exceeded 16 MiB.'
        }
        if ($handleGrowth -gt 32) {
            throw 'Steady-state handle growth exceeded 32 handles.'
        }
    }

    if ($faultRequired) {
        if ([uint64]$fields['writer_retry_attempts'] -lt 1 -or
            [uint64]$fields['writer_retry_exhausted'] -ne 1 -or
            [uint64]$fields['writer_failed'] -ne 1 -or
            [uint64]$fields['writer_recoveries'] -lt 1 -or
            $fields['recoverable_incident_available'] -ne '0' -or
            [uint64]$fields['writer_explicit_recoveries'] -ne 1 -or
            [uint64]$fields['writer_last_failed_capture_sequence'] -ne 2) {
            throw 'Archive fault/recovery was not independently reflected by the writer.'
        }
    }
    if ($Mode -eq '72-hour') {
        $eventRows = @(Get-Content -LiteralPath $events | Select-Object -Skip 1)
        foreach ($required in @('sleep_resume', 'lock_unlock', 'device_churn',
                                'archive_fault_started', 'archive_recovered')) {
            if (-not ($eventRows -match "`t$required$")) { throw "Missing operator event: $required" }
        }
        if ([uint64]$fields['resume_events'] -lt 1) {
            throw 'The collector did not independently observe the attested sleep/resume.'
        }
        if ($fields['session_notifications_available'] -ne '1' -or
            [uint64]$fields['session_locks'] -lt 1 -or
            [uint64]$fields['session_unlocks'] -lt 1) {
            throw 'The shell did not independently observe the attested lock/unlock.'
        }
        $deviceEvidence = [uint64]$fields['device_events_recorded'] +
                          [uint64]$fields['audio_events_recorded']
        if ($deviceEvidence -lt 1) {
            throw 'The event provider did not independently observe the attested device churn.'
        }
    }

    $observedRuntimeText = $stopwatch.Elapsed.TotalSeconds.ToString('F3', $invariant)
    $cpuPercentText = $cpuPercent.ToString('F6', $invariant)
    if ((Get-FileHash -LiteralPath $application -Algorithm SHA256).Hash.ToLowerInvariant() -cne
            $applicationHash -or
        (Get-FileHash -LiteralPath $runnerScript -Algorithm SHA256).Hash.ToLowerInvariant() -cne
            $runnerHash -or
        (Get-FileHash -LiteralPath $verifierScript -Algorithm SHA256).Hash.ToLowerInvariant() -cne
            $verifierHash -or
        ($faultRequired -and
         (Get-FileHash -LiteralPath $faultProbe -Algorithm SHA256).Hash.ToLowerInvariant() -cne
            $faultProbeHash)) {
        throw 'A bound application or qualification tool changed during the campaign.'
    }
    $summary = @"
format=1
state=passed
mode=$Mode
profile=$Profile
requested_runtime_seconds=$DurationSeconds
capture_interval_seconds=$CaptureIntervalSeconds
checkpoint_interval_seconds=$CheckpointSeconds
minimum_process_samples=$minimumProcessSamples
minimum_collections=$minimumCollections
minimum_scheduled_captures=$minimumScheduledCaptures
logical_processor_count=$logicalProcessors
source_revision=$SourceRevision
application_sha256=$applicationHash
runner_sha256=$runnerHash
verifier_sha256=$verifierHash
archive_fault_probe_sha256=$faultProbeHash
observed_runtime_seconds=$observedRuntimeText
process_samples=$sampleCount
sampling_gaps=$samplingGaps
maximum_working_set_bytes=$maximumWorkingSet
maximum_private_bytes=$maximumPrivateBytes
maximum_handles=$maximumHandles
average_total_machine_cpu_percent=$cpuPercentText
archive_fault_exercised=$(if ($faultRequired) { 1 } else { 0 })
steady_state_window_samples=$([math]::Min($steadyWindowSize, $sampleCount))
steady_state_working_set_growth_bytes=$workingSetGrowth
steady_state_private_bytes_growth=$privateGrowth
steady_state_handle_growth=$handleGrowth
"@
    Write-AtomicText (Join-Path $staging 'summary.ini') $summary
    Write-AtomicText $campaign (
        "format=1`nstate=passed`nmode=$Mode`nrequested_runtime_seconds=$DurationSeconds`n" +
        $campaignContract + $provenance)
    Write-AtomicText (Join-Path $staging 'checkpoint.ini') (
        "format=1`nstate=completed`nelapsed_seconds=$([math]::Floor($stopwatch.Elapsed.TotalSeconds))`n" +
        "process_samples=$sampleCount`nsampling_gaps=$samplingGaps`n")
    $manifestFiles = @('app-report.ini', 'campaign.ini', 'checkpoint.ini', 'operator-events.tsv',
                       'process-samples.tsv', 'summary.ini',
                       'data/product-settings.ini', 'data/recorder-settings.ini',
                       'data/incidents.sqlite3')
    $manifestFiles += @('runtime-inventory.ini', 'app-progress.ini') + @($runtimeHashes.Keys)
    if ($faultRequired) { $manifestFiles += 'recovered-incident.sqlite3' }
    foreach ($relative in $runtimeHashes.Keys) {
        if ((Get-FileHash -LiteralPath (Join-Path $staging $relative) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $runtimeHashes[$relative]) {
            throw "Runtime file changed during campaign: $relative"
        }
    }
    $manifestLines = @('format=1', 'algorithm=sha256', "file_count=$($manifestFiles.Count)")
    foreach ($relative in $manifestFiles) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $staging $relative) -Algorithm SHA256).Hash.ToLowerInvariant()
        $manifestLines += "$relative=$hash"
    }
    Write-AtomicText (Join-Path $staging 'manifest.sha256.ini') (($manifestLines -join "`n") + "`n")
    & $verifierScript `
        -CampaignDirectory $staging -AllowStaging | Out-Null
    [IO.Directory]::Move($staging, $output)
    Write-Host "Wall-clock $Mode campaign passed: $output"
} catch {
    $failure = $_
    # Terminate owned children even when the output volume is full or unavailable.
    if ($workloadProcess -ne $null -and -not $workloadProcess.HasExited) { Stop-Process -Id $workloadProcess.Id -Force -ErrorAction SilentlyContinue }
    if ($settingsProbe -ne $null -and -not $settingsProbe.HasExited) {
        Stop-Process -Id $settingsProbe.Id -Force -ErrorAction SilentlyContinue
    }
    if ($process -ne $null -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    if ($faultProcess -ne $null -and -not $faultProcess.HasExited) {
        Stop-Process -Id $faultProcess.Id -Force -ErrorAction SilentlyContinue
    }
    try {
        Write-AtomicText (Join-Path $staging 'failure.txt') (
            [DateTimeOffset]::UtcNow.ToString('O') + "`n" + $failure.Exception.Message + "`n" + $failure.ScriptStackTrace + "`n")
        Write-AtomicText $campaign (
            "format=1`nstate=failed`nmode=$Mode`nrequested_runtime_seconds=$DurationSeconds`n" +
            $campaignContract + $provenance)
    } catch {
        Write-Warning ('Could not persist failure diagnostics: ' + $_.Exception.Message)
    }
    throw $failure
} finally {
    [Environment]::SetEnvironmentVariable('BLACKBOX_PRODUCT_SETTINGS_PATH', $oldProduct, 'Process')
    [Environment]::SetEnvironmentVariable('BLACKBOX_SETTINGS_PATH', $oldRecorder, 'Process')
}
