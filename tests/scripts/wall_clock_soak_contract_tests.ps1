[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,

    [string]$ApplicationPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Expect-Failure([scriptblock]$Action, [string]$Name) {
    try {
        & $Action
    } catch {
        Write-Output "Expected rejection: $Name"
        return
    }
    throw "Expected failure did not occur: $Name"
}

function Expect-FailureMessage([scriptblock]$Action, [string]$Pattern, [string]$Name) {
    try {
        & $Action
    } catch {
        if ($_.Exception.Message -notmatch $Pattern) {
            throw "Unexpected rejection for ${Name}: $($_.Exception.Message)"
        }
        Write-Output "Expected rejection: $Name"
        return
    }
    throw "Expected failure did not occur: $Name"
}

function Write-Text([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Get-HereStringTemplate([string]$Path, [string]$VariableName) {
    $lines = [IO.File]::ReadAllLines($Path)
    $marker = '$' + $VariableName + ' = @"'
    $start = [Array]::IndexOf($lines, $marker)
    if ($start -lt 0) { throw "Runner template is missing: $VariableName" }
    $contents = [Collections.Generic.List[string]]::new()
    for ($index = $start + 1; $index -lt $lines.Count; ++$index) {
        if ($lines[$index] -ceq '"@') {
            return (($contents -join "`n") + "`n")
        }
        $contents.Add($lines[$index])
    }
    throw "Runner template is unterminated: $VariableName"
}

function Invoke-Application([string]$Path, [string[]]$Arguments) {
    $process = Start-Process -FilePath $Path -ArgumentList $Arguments `
                             -WindowStyle Hidden -Wait -PassThru
    return $process.ExitCode
}

function Set-Field([string]$Path, [string]$Name, [string]$Value) {
    $lines = [IO.File]::ReadAllLines($Path)
    $matches = 0
    for ($index = 0; $index -lt $lines.Count; ++$index) {
        if ($lines[$index].StartsWith("$Name=", [StringComparison]::Ordinal)) {
            $lines[$index] = "$Name=$Value"
            $matches++
        }
    }
    if ($matches -ne 1) { throw "Fixture field is missing or duplicated: $Name" }
    Write-Text $Path (($lines -join "`n") + "`n")
}

function Write-Manifest([string]$Directory) {
    $files = @(
        'app-report.ini', 'campaign.ini', 'checkpoint.ini', 'operator-events.tsv',
        'process-samples.tsv', 'summary.ini', 'data/product-settings.ini',
        'data/recorder-settings.ini', 'data/incidents.sqlite3'
    )
    $files += @('runtime-inventory.ini', 'app-progress.ini')
    $files += @(Get-ChildItem -LiteralPath (Join-Path $Directory 'runtime') -File | ForEach-Object { 'runtime/' + $_.Name })
    if ([IO.File]::Exists((Join-Path $Directory 'recovered-incident.sqlite3'))) { $files += 'recovered-incident.sqlite3' }
    $manifest = @('format=1', 'algorithm=sha256', "file_count=$($files.Count)")
    foreach ($relative in $files) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $Directory $relative) `
                             -Algorithm SHA256).Hash.ToLowerInvariant()
        $manifest += "$relative=$hash"
    }
    Write-Text (Join-Path $Directory 'manifest.sha256.ini') (($manifest -join "`n") + "`n")
}

function New-SoakFixture([string]$Directory) {
    $runnerHash = (Get-FileHash -LiteralPath (Join-Path $SourceRoot 'scripts\run-wall-clock-soak.ps1') `
                               -Algorithm SHA256).Hash.ToLowerInvariant()
    $verifierHash = (Get-FileHash -LiteralPath (Join-Path $SourceRoot 'scripts\verify-wall-clock-soak.ps1') `
                                 -Algorithm SHA256).Hash.ToLowerInvariant()
    $logicalProcessors = [uint64][Environment]::ProcessorCount
    $cpuPercent = (0.03 / (6.0 * $logicalProcessors) * 100.0).ToString(
        'F6', [Globalization.CultureInfo]::InvariantCulture)
    [IO.Directory]::CreateDirectory((Join-Path $Directory 'data')) | Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $Directory 'data\crashes')) | Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $Directory 'runtime')) | Out-Null
    Write-Text (Join-Path $Directory 'runtime/blackbox.exe') 'contract fixture, never executable evidence'
    $applicationHash = (Get-FileHash -LiteralPath (Join-Path $Directory 'runtime/blackbox.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Text (Join-Path $Directory 'runtime-inventory.ini') "format=1`nfile_count=1`nruntime/blackbox.exe=$applicationHash`n"
    Write-Text (Join-Path $Directory 'app-progress.ini') "format=1`nsource_revision=local-uncommitted`ncomplete=1`ncollections=1`n"
    Write-Text (Join-Path $Directory 'campaign.ini') @"
format=1
state=passed
mode=smoke
profile=isolated
requested_runtime_seconds=10
capture_interval_seconds=5
checkpoint_interval_seconds=2
minimum_process_samples=2
minimum_collections=5
minimum_scheduled_captures=1
logical_processor_count=$logicalProcessors
source_revision=local-uncommitted
application_sha256=$applicationHash
runner_sha256=$runnerHash
verifier_sha256=$verifierHash
archive_fault_probe_sha256=none
"@
    Write-Text (Join-Path $Directory 'checkpoint.ini') @"
format=1
state=completed
elapsed_seconds=10
process_samples=3
sampling_gaps=0
"@
    Write-Text (Join-Path $Directory 'summary.ini') @"
format=1
state=passed
mode=smoke
profile=isolated
requested_runtime_seconds=10
capture_interval_seconds=5
checkpoint_interval_seconds=2
minimum_process_samples=2
minimum_collections=5
minimum_scheduled_captures=1
logical_processor_count=$logicalProcessors
source_revision=local-uncommitted
application_sha256=$applicationHash
runner_sha256=$runnerHash
verifier_sha256=$verifierHash
archive_fault_probe_sha256=none
observed_runtime_seconds=10.250
process_samples=3
sampling_gaps=0
maximum_working_set_bytes=1024
maximum_private_bytes=2048
maximum_handles=10
average_total_machine_cpu_percent=$cpuPercent
archive_fault_exercised=0
steady_state_window_samples=3
steady_state_working_set_growth_bytes=0
steady_state_private_bytes_growth=0
steady_state_handle_growth=0
"@
    Write-Text (Join-Path $Directory 'app-report.ini') @"
format=1
source_revision=local-uncommitted
completed=1
requested_runtime_seconds=10
capture_interval_seconds=5
incident_pre_window_seconds=1
incident_post_window_seconds=1
history_duration_seconds=60
collect_process_paths=0
collections=10
sampling_thread_prepared=1
failed_samples=0
dropped_samples=0
deadline_misses=0
scheduling_drop_event_count=0
scheduling_drop_event_overflow=0
scheduling_drop_events=none
collector_worker_failures=0
snapshot_failures=0
capture_queue_rejections=0
event_worker_failures=0
native_events_dropped=0
writer_cancelled=0
unclassified_long_gaps=0
unclassified_skipped_samples=0
writer_failed_incidents_not_retained=0
writer_explicit_recoveries=0
writer_last_failed_capture_sequence=0
writer_last_failure_utc_nanoseconds=0
writer_retry_attempts=0
writer_recoveries=0
recoverable_incident_available=0
automatic_detection_enabled=0
automatic_detector_triggers=0
automatic_captures_started=0
automatic_event_requests=0
archive_healthy=1
archive_schema_version=1
power_events_recorded=0
device_events_recorded=0
audio_events_recorded=0
service_events_recorded=0
security_events_recorded=0
update_events_recorded=0
application_events_recorded=0
network_events_recorded=0
graphics_events_recorded=0
storage_events_recorded=0
system_events_recorded=0
writer_succeeded=1
writer_failed=0
incidents_completed=1
archive_incidents=1
writer_retry_exhausted=0
"@
    Write-Text (Join-Path $Directory 'operator-events.tsv') "utc`tevent`r`n"
    Write-Text (Join-Path $Directory 'process-samples.tsv') (
        "elapsed_seconds`tutc`tworking_set_bytes`tprivate_bytes`thandles`ttotal_cpu_seconds`r`n" +
        "2.000`t2026-01-01T00:00:02.0000000+00:00`t1024`t2048`t10`t0.010000`r`n" +
        "4.000`t2026-01-01T00:00:04.0000000+00:00`t1024`t2048`t10`t0.020000`r`n" +
        "6.000`t2026-01-01T00:00:06.0000000+00:00`t1024`t2048`t10`t0.030000`r`n")
    Write-Text (Join-Path $Directory 'data\product-settings.ini') "format=1`nfixture=1`n"
    Write-Text (Join-Path $Directory 'data\recorder-settings.ini') "format=1`nfixture=1`n"
    $archive = [byte[]]::new(512)
    [Text.Encoding]::ASCII.GetBytes("SQLite format 3`0").CopyTo($archive, 0)
    [IO.File]::WriteAllBytes((Join-Path $Directory 'data\incidents.sqlite3'), $archive)

    Write-Manifest $Directory
}

function New-OvernightFixture([string]$Directory) {
    New-SoakFixture $Directory
    foreach ($file in @('campaign.ini', 'summary.ini')) {
        $path = Join-Path $Directory $file
        Set-Field $path 'mode' 'overnight'
        Set-Field $path 'requested_runtime_seconds' '28800'
        Set-Field $path 'capture_interval_seconds' '900'
        Set-Field $path 'checkpoint_interval_seconds' '60'
        Set-Field $path 'minimum_process_samples' '455'
        Set-Field $path 'minimum_collections' '27360'
        Set-Field $path 'minimum_scheduled_captures' '31'
    }
    $summaryPath = Join-Path $Directory 'summary.ini'
    Set-Field $summaryPath 'observed_runtime_seconds' '28800.500'
    Set-Field $summaryPath 'process_samples' '455'
    Set-Field $summaryPath 'average_total_machine_cpu_percent' '0.000000'
    Set-Field $summaryPath 'steady_state_window_samples' '10'
    $checkpointPath = Join-Path $Directory 'checkpoint.ini'
    Set-Field $checkpointPath 'elapsed_seconds' '28800'
    Set-Field $checkpointPath 'process_samples' '455'
    $reportPath = Join-Path $Directory 'app-report.ini'
    Set-Field $reportPath 'requested_runtime_seconds' '28800'
    Set-Field $reportPath 'capture_interval_seconds' '900'
    Set-Field $reportPath 'collections' '27360'
    Set-Field $reportPath 'writer_succeeded' '31'
    Set-Field $reportPath 'incidents_completed' '31'
    Set-Field $reportPath 'archive_incidents' '31'
    $journal = [Text.StringBuilder]::new()
    [void]$journal.Append(
        "elapsed_seconds`tutc`tworking_set_bytes`tprivate_bytes`thandles`ttotal_cpu_seconds`r`n")
    $started = [DateTimeOffset]::Parse('2026-01-01T00:00:00.0000000+00:00')
    for ($index = 1; $index -le 455; ++$index) {
        $elapsed = $index * 60
        [void]$journal.AppendFormat(
            [Globalization.CultureInfo]::InvariantCulture,
            "{0:F3}`t{1}`t1024`t2048`t10`t0.000000`r`n",
            $elapsed, $started.AddSeconds($elapsed).ToString('O'))
    }
    Write-Text (Join-Path $Directory 'process-samples.tsv') $journal.ToString()
    Write-Manifest $Directory
}

$root = Join-Path ([IO.Path]::GetTempPath()) ("blackbox-soak-contract-" + [guid]::NewGuid())
[IO.Directory]::CreateDirectory($root) | Out-Null
try {
    $run = Join-Path $SourceRoot 'scripts\run-wall-clock-soak.ps1'
    $record = Join-Path $SourceRoot 'scripts\record-soak-event.ps1'
    $verify = Join-Path $SourceRoot 'scripts\verify-wall-clock-soak.ps1'
    foreach ($script in @($run, $record, $verify)) {
        $tokens = $null
        $errors = $null
        [void][Management.Automation.Language.Parser]::ParseFile(
            $script, [ref]$tokens, [ref]$errors)
        if ($errors.Count -ne 0) { throw "PowerShell parser rejected $script" }
    }

    if (-not [string]::IsNullOrWhiteSpace($ApplicationPath)) {
        $application = (Resolve-Path -LiteralPath $ApplicationPath -ErrorAction Stop).Path
        $settingsDirectory = Join-Path $root 'settings-preflight'
        [IO.Directory]::CreateDirectory($settingsDirectory) | Out-Null
        $productPath = Join-Path $settingsDirectory 'product-settings.ini'
        $recorderPath = Join-Path $settingsDirectory 'recorder-settings.ini'
        $archiveField = (Join-Path $settingsDirectory 'incidents.sqlite3').Replace('\', '/')
        $productTemplate = Get-HereStringTemplate $run 'productText'
        $recorderTemplate = Get-HereStringTemplate $run 'recorderText'
        $validProduct = $productTemplate.Replace('$archiveField', $archiveField)
        Write-Text $productPath $validProduct
        Write-Text $recorderPath $recorderTemplate
        $oldProduct = [Environment]::GetEnvironmentVariable(
            'BLACKBOX_PRODUCT_SETTINGS_PATH', 'Process')
        $oldRecorder = [Environment]::GetEnvironmentVariable(
            'BLACKBOX_SETTINGS_PATH', 'Process')
        try {
            [Environment]::SetEnvironmentVariable(
                'BLACKBOX_PRODUCT_SETTINGS_PATH', $productPath, 'Process')
            [Environment]::SetEnvironmentVariable(
                'BLACKBOX_SETTINGS_PATH', $recorderPath, 'Process')
            if ((Invoke-Application $application @('--validate-settings-only')) -ne 0) {
                throw 'The assembled app rejected the runner settings templates.'
            }

            $missingLifecycle = $validProduct.Replace(
                "record_process_lifecycle=0`n", '')
            if ($missingLifecycle -ceq $validProduct) {
                throw 'The runner template does not pin process lifecycle collection.'
            }
            Write-Text $productPath $missingLifecycle
            if ((Invoke-Application $application @('--validate-settings-only')) -eq 0) {
                throw 'The assembled app accepted an incomplete product settings template.'
            }

            Write-Text $productPath $validProduct
            if ((Invoke-Application $application @(
                    '--validate-settings-only', '--background')) -eq 0) {
                throw 'Settings-only validation accepted an unrelated app argument.'
            }
        } finally {
            [Environment]::SetEnvironmentVariable(
                'BLACKBOX_PRODUCT_SETTINGS_PATH', $oldProduct, 'Process')
            [Environment]::SetEnvironmentVariable(
                'BLACKBOX_SETTINGS_PATH', $oldRecorder, 'Process')
        }
    }

    $valid = Join-Path $root 'valid'
    New-SoakFixture $valid
    & $verify -CampaignDirectory $valid | Out-Null
    $windowsPowerShell = Get-Command powershell.exe -ErrorAction SilentlyContinue
    if ($null -ne $windowsPowerShell) {
        & $windowsPowerShell.Source -NoProfile -NonInteractive -ExecutionPolicy Bypass `
            -File $verify -CampaignDirectory $valid | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw 'The soak verifier failed under Windows PowerShell 5.1.'
        }
    }

    # The formerly accepted extra-loss case must fail even after rehashing all evidence.
    $validFault = Join-Path $root 'valid-fault'
    Copy-Item -LiteralPath $valid -Destination $validFault -Recurse
    Write-Text (Join-Path $validFault 'runtime/blackbox_soak_archive_fault.exe') 'isolated contract probe fixture'
    $probeHash = (Get-FileHash -LiteralPath (Join-Path $validFault 'runtime/blackbox_soak_archive_fault.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
    $appHash = (Get-FileHash -LiteralPath (Join-Path $validFault 'runtime/blackbox.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Text (Join-Path $validFault 'runtime-inventory.ini') "format=1`nfile_count=2`nruntime/blackbox.exe=$appHash`nruntime/blackbox_soak_archive_fault.exe=$probeHash`n"
    Copy-Item -LiteralPath (Join-Path $validFault 'data/incidents.sqlite3') -Destination (Join-Path $validFault 'recovered-incident.sqlite3')
    foreach ($file in @('campaign.ini', 'summary.ini')) { Set-Field (Join-Path $validFault $file) 'archive_fault_probe_sha256' $probeHash }
    Set-Field (Join-Path $validFault 'summary.ini') 'archive_fault_exercised' '1'
    $faultReport = Join-Path $validFault 'app-report.ini'
    foreach ($field in @('writer_failed', 'writer_explicit_recoveries', 'writer_retry_exhausted', 'writer_retry_attempts', 'writer_recoveries')) { Set-Field $faultReport $field '1' }
    Set-Field $faultReport 'writer_last_failed_capture_sequence' '2'
    Set-Field $faultReport 'writer_last_failure_utc_nanoseconds' '1767225605000000000'
    Write-Text (Join-Path $validFault 'operator-events.tsv') "utc`tevent`n2026-01-01T00:00:04Z`tarchive_fault_started`n2026-01-01T00:00:06Z`tarchive_recovered`n"
    Write-Manifest $validFault
    & $verify -CampaignDirectory $validFault | Out-Null
    foreach ($case in @('extra-loss', 'wrong-sequence', 'outside-fault', 'unretained-loss', 'unknown-gap', 'mutated-runtime')) {
        $destination = Join-Path $root $case
        Copy-Item -LiteralPath $validFault -Destination $destination -Recurse
        $caseReport = Join-Path $destination 'app-report.ini'
        switch ($case) {
            'extra-loss' { Set-Field $caseReport 'writer_failed' '2'; Set-Field $caseReport 'incidents_completed' '2' }
            'wrong-sequence' { Set-Field $caseReport 'writer_last_failed_capture_sequence' '3' }
            'outside-fault' { Set-Field $caseReport 'writer_last_failure_utc_nanoseconds' '1767225607000000000' }
            'unretained-loss' { Set-Field $caseReport 'writer_failed_incidents_not_retained' '1' }
            'unknown-gap' { Set-Field $caseReport 'unclassified_long_gaps' '1' }
            'mutated-runtime' { Write-Text (Join-Path $destination 'runtime/blackbox.exe') 'changed executable' }
        }
        Write-Manifest $destination
        Expect-Failure { & $verify -CampaignDirectory $destination | Out-Null } $case
    }

    $validOvernight = Join-Path $root 'valid-overnight'
    New-OvernightFixture $validOvernight
    & $verify -CampaignDirectory $validOvernight | Out-Null

    $wrongOvernightCadence = Join-Path $root 'wrong-overnight-cadence'
    Copy-Item -LiteralPath $validOvernight -Destination $wrongOvernightCadence -Recurse
    foreach ($file in @('campaign.ini', 'summary.ini', 'app-report.ini')) {
        Set-Field (Join-Path $wrongOvernightCadence $file) 'capture_interval_seconds' '901'
    }
    Write-Manifest $wrongOvernightCadence
    Expect-Failure { & $verify -CampaignDirectory $wrongOvernightCadence | Out-Null } `
        'hash-consistent overridden overnight cadence'

    $sparseOvernight = Join-Path $root 'sparse-overnight'
    Copy-Item -LiteralPath $validOvernight -Destination $sparseOvernight -Recurse
    $sparseJournal = Join-Path $sparseOvernight 'process-samples.tsv'
    $sparseLines = [IO.File]::ReadAllLines($sparseJournal)
    Write-Text $sparseJournal (($sparseLines[0..($sparseLines.Count - 2)] -join "`r`n") + "`r`n")
    Set-Field (Join-Path $sparseOvernight 'summary.ini') 'process_samples' '454'
    Set-Field (Join-Path $sparseOvernight 'checkpoint.ini') 'process_samples' '454'
    Write-Manifest $sparseOvernight
    Expect-Failure { & $verify -CampaignDirectory $sparseOvernight | Out-Null } `
        'hash-consistent sparse overnight journal'

    $partial = Join-Path $root 'valid.partial'
    Copy-Item -LiteralPath $valid -Destination $partial -Recurse
    Expect-Failure { & $verify -CampaignDirectory $partial | Out-Null } 'partial evidence'
    & $verify -CampaignDirectory $partial -AllowStaging | Out-Null

    $tampered = Join-Path $root 'tampered'
    Copy-Item -LiteralPath $valid -Destination $tampered -Recurse
    [IO.File]::AppendAllText((Join-Path $tampered 'summary.ini'), "changed=1`n")
    Expect-Failure { & $verify -CampaignDirectory $tampered | Out-Null } 'changed evidence'

    $staleHarness = Join-Path $root 'stale-harness'
    Copy-Item -LiteralPath $valid -Destination $staleHarness -Recurse
    Set-Field (Join-Path $staleHarness 'campaign.ini') 'runner_sha256' $('d' * 64)
    Set-Field (Join-Path $staleHarness 'summary.ini') 'runner_sha256' $('d' * 64)
    Write-Manifest $staleHarness
    Expect-Failure { & $verify -CampaignDirectory $staleHarness | Out-Null } `
        'hash-consistent stale runner identity'

    $staleVerifier = Join-Path $root 'stale-verifier'
    Copy-Item -LiteralPath $valid -Destination $staleVerifier -Recurse
    Set-Field (Join-Path $staleVerifier 'campaign.ini') 'verifier_sha256' $('e' * 64)
    Set-Field (Join-Path $staleVerifier 'summary.ini') 'verifier_sha256' $('e' * 64)
    Write-Manifest $staleVerifier
    Expect-Failure { & $verify -CampaignDirectory $staleVerifier | Out-Null } `
        'hash-consistent stale verifier identity'

    $wrongAppRevision = Join-Path $root 'wrong-app-revision'
    Copy-Item -LiteralPath $valid -Destination $wrongAppRevision -Recurse
    Set-Field (Join-Path $wrongAppRevision 'app-report.ini') `
        'source_revision' $('b' * 40)
    Write-Manifest $wrongAppRevision
    Expect-Failure { & $verify -CampaignDirectory $wrongAppRevision | Out-Null } `
        'hash-consistent app source revision mismatch'

    $falseMaximum = Join-Path $root 'false-maximum'
    Copy-Item -LiteralPath $valid -Destination $falseMaximum -Recurse
    Set-Field (Join-Path $falseMaximum 'summary.ini') 'maximum_private_bytes' '2049'
    Write-Manifest $falseMaximum
    Expect-Failure { & $verify -CampaignDirectory $falseMaximum | Out-Null } `
        'hash-consistent false resource maximum'

    $falseCpu = Join-Path $root 'false-cpu'
    Copy-Item -LiteralPath $valid -Destination $falseCpu -Recurse
    Set-Field (Join-Path $falseCpu 'summary.ini') `
        'average_total_machine_cpu_percent' '0.999999'
    Write-Manifest $falseCpu
    Expect-Failure { & $verify -CampaignDirectory $falseCpu | Out-Null } `
        'hash-consistent false CPU summary'

    $falseGrowth = Join-Path $root 'false-growth'
    Copy-Item -LiteralPath $valid -Destination $falseGrowth -Recurse
    Set-Field (Join-Path $falseGrowth 'summary.ini') 'steady_state_handle_growth' '1'
    Write-Manifest $falseGrowth
    Expect-Failure { & $verify -CampaignDirectory $falseGrowth | Out-Null } `
        'hash-consistent false steady-state growth'

    $falseGaps = Join-Path $root 'false-gaps'
    Copy-Item -LiteralPath $valid -Destination $falseGaps -Recurse
    Set-Field (Join-Path $falseGaps 'summary.ini') 'sampling_gaps' '1'
    Set-Field (Join-Path $falseGaps 'checkpoint.ini') 'sampling_gaps' '1'
    Write-Manifest $falseGaps
    Expect-Failure { & $verify -CampaignDirectory $falseGaps | Out-Null } `
        'hash-consistent false sampling gaps'

    $nonmonotonic = Join-Path $root 'nonmonotonic'
    Copy-Item -LiteralPath $valid -Destination $nonmonotonic -Recurse
    $journalPath = Join-Path $nonmonotonic 'process-samples.tsv'
    $journalLines = [IO.File]::ReadAllLines($journalPath)
    $columns = $journalLines[2].Split("`t")
    $columns[0] = '1.000'
    $journalLines[2] = $columns -join "`t"
    Write-Text $journalPath (($journalLines -join "`r`n") + "`r`n")
    Write-Manifest $nonmonotonic
    Expect-Failure { & $verify -CampaignDirectory $nonmonotonic | Out-Null } `
        'hash-consistent nonmonotonic journal'

    $falseMinimum = Join-Path $root 'false-minimum'
    Copy-Item -LiteralPath $valid -Destination $falseMinimum -Recurse
    Set-Field (Join-Path $falseMinimum 'campaign.ini') 'minimum_process_samples' '1'
    Set-Field (Join-Path $falseMinimum 'summary.ini') 'minimum_process_samples' '1'
    Write-Manifest $falseMinimum
    Expect-Failure { & $verify -CampaignDirectory $falseMinimum | Out-Null } `
        'hash-consistent false process minimum'

    $insufficientCollections = Join-Path $root 'insufficient-collections'
    Copy-Item -LiteralPath $valid -Destination $insufficientCollections -Recurse
    Set-Field (Join-Path $insufficientCollections 'app-report.ini') 'collections' '4'
    Write-Manifest $insufficientCollections
    Expect-Failure { & $verify -CampaignDirectory $insufficientCollections | Out-Null } `
        'hash-consistent insufficient collection coverage'

    $wrongReportCadence = Join-Path $root 'wrong-report-cadence'
    Copy-Item -LiteralPath $valid -Destination $wrongReportCadence -Recurse
    Set-Field (Join-Path $wrongReportCadence 'app-report.ini') `
        'capture_interval_seconds' '4'
    Write-Manifest $wrongReportCadence
    Expect-Failure { & $verify -CampaignDirectory $wrongReportCadence | Out-Null } `
        'hash-consistent app-report cadence mismatch'

    $insufficientCaptures = Join-Path $root 'insufficient-captures'
    Copy-Item -LiteralPath $valid -Destination $insufficientCaptures -Recurse
    foreach ($field in @('writer_succeeded', 'incidents_completed', 'archive_incidents')) {
        Set-Field (Join-Path $insufficientCaptures 'app-report.ini') $field '0'
    }
    Write-Manifest $insufficientCaptures
    Expect-Failure { & $verify -CampaignDirectory $insufficientCaptures | Out-Null } `
        'hash-consistent insufficient scheduled captures'

    $unexpectedAutomatic = Join-Path $root 'unexpected-automatic'
    Copy-Item -LiteralPath $valid -Destination $unexpectedAutomatic -Recurse
    Set-Field (Join-Path $unexpectedAutomatic 'app-report.ini') `
        'automatic_detection_enabled' '1'
    Write-Manifest $unexpectedAutomatic
    Expect-Failure { & $verify -CampaignDirectory $unexpectedAutomatic | Out-Null } `
        'hash-consistent automatic detection in isolated soak'

    $missingDropDetails = Join-Path $root 'missing-drop-details'
    Copy-Item -LiteralPath $valid -Destination $missingDropDetails -Recurse
    Set-Field (Join-Path $missingDropDetails 'app-report.ini') `
        'scheduling_drop_event_count' '1'
    Write-Manifest $missingDropDetails
    Expect-Failure { & $verify -CampaignDirectory $missingDropDetails | Out-Null } `
        'hash-consistent missing scheduling drop details'

    $contradictoryDropDetails = Join-Path $root 'contradictory-drop-details'
    Copy-Item -LiteralPath $valid -Destination $contradictoryDropDetails -Recurse
    $dropReport = Join-Path $contradictoryDropDetails 'app-report.ini'
    Set-Field $dropReport 'dropped_samples' '2'
    Set-Field $dropReport 'deadline_misses' '1'
    Set-Field $dropReport 'scheduling_drop_event_count' '1'
    Set-Field $dropReport 'scheduling_drop_events' `
        '10:1700000000000000000:200000000:1'
    Write-Manifest $contradictoryDropDetails
    Expect-Failure { & $verify -CampaignDirectory $contradictoryDropDetails | Out-Null } `
        'hash-consistent contradictory scheduling drop details'

    $extra = Join-Path $root 'extra'
    Copy-Item -LiteralPath $valid -Destination $extra -Recurse
    Write-Text (Join-Path $extra 'unexpected.txt') 'unexpected'
    Expect-Failure { & $verify -CampaignDirectory $extra | Out-Null } 'extra file'

    $running = Join-Path $root 'running.partial'
    [IO.Directory]::CreateDirectory($running) | Out-Null
    Write-Text (Join-Path $running 'campaign.ini') "format=1`nstate=running`nmode=72-hour`n"
    Write-Text (Join-Path $running 'operator-events.tsv') "utc`tevent`r`n"
    & $record -CampaignDirectory $running -Event device_churn | Out-Null
    $journal = [IO.File]::ReadAllText((Join-Path $running 'operator-events.tsv'))
    if ($journal -notmatch "`tdevice_churn\r?`n$") {
        throw 'The operator journal did not durably append the allowed event.'
    }
    Expect-Failure {
        & $record -CampaignDirectory $running -Event archive_fault_started | Out-Null
    } 'runner-owned archive event'

    Expect-FailureMessage {
        & $run -Mode overnight -CaptureIntervalSeconds 1 `
            -ApplicationPath (Join-Path $root 'missing.exe') `
            -OutputDirectory (Join-Path $root 'invalid-overnight-capture') | Out-Null
    } 'fixed capture and checkpoint cadences' 'overridden overnight capture cadence'
    Expect-FailureMessage {
        & $run -Mode '72-hour' -CheckpointSeconds 1 `
            -ApplicationPath (Join-Path $root 'missing.exe') `
            -OutputDirectory (Join-Path $root 'invalid-72-hour-checkpoint') | Out-Null
    } 'fixed capture and checkpoint cadences' 'overridden 72-hour checkpoint cadence'

    $runText = [IO.File]::ReadAllText($run)
    foreach ($required in @('data/incidents.sqlite3', 'state=completed',
                            'verify-wall-clock-soak.ps1', 'WaitForExit',
                            'steady_state_handle_growth', 'hotkey_key=10',
                            'hotkey_alt=1', 'application_sha256',
                            'runner_sha256', 'verifier_sha256',
                            'minimum_scheduled_captures', 'minimum_process_samples',
                            'logical_processor_count', 'record_process_lifecycle=0',
                            '--validate-settings-only',
                            'automatic_detection_enabled',
                            'automatic_detector_triggers',
                            'automatic_captures_started', 'automatic_event_requests')) {
        if (-not $runText.Contains($required)) {
            throw "The runner is missing its required contract: $required"
        }
    }
    Write-Output 'Wall-clock soak script contracts passed.'
} finally {
    if ([IO.Directory]::Exists($root)) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
