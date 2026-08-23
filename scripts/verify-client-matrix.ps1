[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string[]]$EvidenceDirectory,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [switch]$RequireAuthenticode
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

function Read-DirectV1([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { throw "Missing direct-v1 artifact: $Path" }
    $fields = @{}
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            throw 'Direct-v1 artifacts cannot contain blank lines.'
        }
        $separator = $line.IndexOf('=')
        if ($separator -lt 1) { throw "Malformed direct-v1 line in $Path" }
        $name = $line.Substring(0, $separator)
        if ($fields.ContainsKey($name)) { throw "Duplicate direct-v1 field: $name" }
        $fields[$name] = $line.Substring($separator + 1)
    }
    if ($fields['format'] -ne '1') { throw "Artifact is not direct format v1: $Path" }
    return $fields
}

function Require-Field($Fields, [string]$Name, [string]$Value) {
    if (-not $Fields.ContainsKey($Name) -or $Fields[$Name] -cne $Value) {
        throw "Required field mismatch: $Name=$Value"
    }
}

$expectedCases = @{
    standard = @(
        'package_launch_ordinary_user', 'tray_hide_restore', 'global_hotkey_capture',
        'first_run_onboarding_keyboard', 'focus_visibility_text_scaling',
        'incident_view', 'settings_diagnostics', 'keyboard_navigation',
        'high_contrast_live_toggle', 'hidden_high_contrast_catchup',
        'scale_100', 'scale_125', 'scale_150', 'scale_200'
    )
    multimonitor = @(
        'mixed_scale_monitor_move', 'taskbar_work_area_change',
        'monitor_disconnect_reconnect', 'suspend_resume'
    )
    'low-end' = @('low_end_responsiveness', 'low_end_resource_bounds')
    battery = @(
        'battery_operation', 'battery_saver', 'balanced_power',
        'performance_power', 'suspend_resume_battery'
    )
}

if ($EvidenceDirectory.Count -lt 5) {
    throw 'The matrix requires at least five independent profile bundles.'
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
$staging = "$output.partial"
if ([IO.Directory]::Exists($output) -or [IO.File]::Exists($output) -or
    [IO.Directory]::Exists($staging) -or [IO.File]::Exists($staging)) {
    throw 'The matrix output and staging destinations must not already exist.'
}

$packageHash = $null
$packageName = $null
$sourceRevision = $null
$seenManifests = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
$rows = @()
$standardWindows10 = 0
$standardWindows11 = 0
$profileCounts = @{ standard = 0; multimonitor = 0; 'low-end' = 0; battery = 0 }
$allSigned = 1
$allTimestamped = 1

foreach ($directoryInput in $EvidenceDirectory) {
    $directory = (Resolve-Path -LiteralPath $directoryInput -ErrorAction Stop).Path
    if (-not [IO.Directory]::Exists($directory) -or
        ([IO.File]::GetAttributes($directory) -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Every evidence input must be a non-link directory.'
    }
    & (Join-Path $PSScriptRoot 'verify-client-evidence.ps1') `
        -CampaignDirectory $directory -RequireInteractive `
        -RequireAuthenticode:$RequireAuthenticode.IsPresent | Out-Null
    $summary = Read-DirectV1 (Join-Path $directory 'summary.ini')
    if (-not $summary.ContainsKey('package_name') -or
        $summary['package_name'] -notmatch '^[^\\/:]+\.zip$') {
        throw 'Client evidence contains an invalid package filename.'
    }
    $expectedFiles = @(
        'app-report.ini', $summary['package_name'], "$($summary['package_name']).sha256",
        'campaign.ini', 'data/incidents.sqlite3', 'data/product-settings.ini',
        'data/recorder-settings.ini', 'host.ini', 'operator-results.tsv',
        'process-samples.tsv', 'required-cases.tsv', 'summary.ini'
    )
    $manifestPath = Join-Path $directory 'manifest.sha256.ini'
    $manifest = Read-DirectV1 $manifestPath
    Require-Field $manifest 'algorithm' 'sha256'
    $manifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if (-not $seenManifests.Add($manifestHash)) {
        throw 'The same client evidence bundle cannot satisfy more than one matrix slot.'
    }

    $actualFiles = @(Get-ChildItem -LiteralPath $directory -Recurse -File |
        Where-Object { $_.Name -ne 'manifest.sha256.ini' })
    $actualRelative = @($actualFiles | ForEach-Object {
        $_.FullName.Substring($directory.Length + 1).Replace('\', '/')
    } | Sort-Object)
    $expectedSorted = @($expectedFiles | Sort-Object)
    $fileDifferences = @(Compare-Object -ReferenceObject $expectedSorted -DifferenceObject $actualRelative -CaseSensitive)
    if ($actualRelative.Count -ne $expectedSorted.Count -or $fileDifferences.Count -ne 0) {
        throw 'Client evidence does not contain the exact direct-v1 file set.'
    }
    if ([uint64]$manifest['file_count'] -ne [uint64]$actualFiles.Count) {
        throw 'Client evidence file count does not match its manifest.'
    }
    $manifestFileKeys = @($manifest.Keys | Where-Object {
        $_ -notin @('format', 'algorithm', 'file_count')
    })
    if ($manifestFileKeys.Count -ne $actualFiles.Count) {
        throw 'Client evidence manifest contains an unexpected field or omits a file.'
    }
    foreach ($file in $actualFiles) {
        if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Client evidence cannot contain a reparse-point file.'
        }
        $relative = $file.FullName.Substring($directory.Length + 1).Replace('\', '/')
        if (-not $manifest.ContainsKey($relative)) {
            throw "Client evidence manifest omits $relative"
        }
        $actualHash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -cne $manifest[$relative]) {
            throw "Client evidence hash mismatch: $relative"
        }
    }

    $hostFacts = Read-DirectV1 (Join-Path $directory 'host.ini')
    Require-Field $summary 'state' 'passed'
    Require-Field $summary 'mode' 'interactive'
    Require-Field $summary 'automated_package_smoke_satisfied' '1'
    Require-Field $summary 'single_host_profile_satisfied' '1'
    Require-Field $summary 'clean_client_matrix_satisfied' '0'
    Require-Field $summary 'physical_matrix_satisfied' '0'
    Require-Field $hostFacts 'supported_client' '1'
    if ($summary['source_revision'] -notmatch '^[0-9a-f]{40}$') {
        throw 'Client matrix evidence requires a lowercase 40-character source revision.'
    }

    $embeddedPackage = Join-Path $directory $summary['package_name']
    $embeddedHash = (Get-FileHash -LiteralPath $embeddedPackage -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($embeddedHash -cne $summary['package_sha256']) {
        throw 'The embedded release package does not match the qualification summary.'
    }
    & (Join-Path $PSScriptRoot 'verify-release.ps1') -PackagePath $embeddedPackage -RequireAuthenticode:$RequireAuthenticode.IsPresent | Out-Null

    $campaign = Read-DirectV1 (Join-Path $directory 'campaign.ini')
    Require-Field $campaign 'state' 'passed'
    Require-Field $campaign 'mode' 'interactive'
    Require-Field $campaign 'profile' $summary['profile']
    Require-Field $campaign 'source_revision' $summary['source_revision']
    Require-Field $campaign 'process_id' '0'
    Require-Field $campaign 'application_sha256' $summary['application_sha256']
    Require-Field $campaign 'runner_sha256' $summary['runner_sha256']
    Require-Field $campaign 'verifier_sha256' $summary['verifier_sha256']
    [void](Read-DirectV1 (Join-Path $directory 'data/product-settings.ini'))
    [void](Read-DirectV1 (Join-Path $directory 'data/recorder-settings.ini'))
    $archivePath = Join-Path $directory 'data/incidents.sqlite3'
    $sqliteHeader = [Text.Encoding]::ASCII.GetBytes("SQLite format 3`0")
    $archiveStream = [IO.File]::OpenRead($archivePath)
    try {
        $archiveBytes = [byte[]]::new($sqliteHeader.Length)
        if ($archiveStream.Length -lt 512 -or $archiveStream.Length -gt 1GB -or
            $archiveStream.Read($archiveBytes, 0, $archiveBytes.Length) -ne $archiveBytes.Length) {
            throw 'Client evidence archive violates its bounded SQLite shape.'
        }
        for ($index = 0; $index -lt $sqliteHeader.Length; ++$index) {
            if ($archiveBytes[$index] -ne $sqliteHeader[$index]) {
                throw 'Client evidence archive does not have a SQLite header.'
            }
        }
    } finally {
        $archiveStream.Dispose()
    }
    $appReport = Read-DirectV1 (Join-Path $directory 'app-report.ini')
    foreach ($pair in @(
        @('completed', '1'), @('archive_healthy', '1'), @('archive_schema_version', '1'),
        @('failed_samples', '0'), @('dropped_samples', '0'), @('deadline_misses', '0'),
        @('collector_worker_failures', '0'), @('snapshot_failures', '0'),
        @('event_worker_failures', '0'), @('writer_cancelled', '0'),
        @('writer_retry_exhausted', '0'), @('writer_failed', '0'))) {
        Require-Field $appReport $pair[0] $pair[1]
    }
    if ([uint64]$appReport['collections'] -lt 1) {
        throw 'Client evidence diagnostic report contains no collections.'
    }
    $processRows = @([IO.File]::ReadAllLines((Join-Path $directory 'process-samples.tsv')))
    if ($processRows.Count -lt 2 -or $processRows[0] -cne
        "elapsed_seconds`tutc`tphase`tworking_set_bytes`tprivate_bytes`thandles`ttotal_cpu_seconds") {
        throw 'Client evidence contains no valid process samples.'
    }
    $profile = $summary['profile']
    if (-not $expectedCases.ContainsKey($profile)) { throw "Unknown client profile: $profile" }
    if ($hostFacts['os_family'] -notin @('windows10_22h2', 'windows11')) {
        throw 'Client evidence is not from a supported Windows family.'
    }

    $requiredRows = @([IO.File]::ReadAllLines((Join-Path $directory 'required-cases.tsv')))
    $resultRows = @([IO.File]::ReadAllLines((Join-Path $directory 'operator-results.tsv')))
    if ($requiredRows.Count -ne ($expectedCases[$profile].Count + 1) -or
        $requiredRows[0] -cne "format`tcase" -or
        $resultRows.Count -ne ($expectedCases[$profile].Count + 1) -or
        $resultRows[0] -cne "utc`tcase`tresult") {
        throw 'Client profile case artifacts do not have the exact direct-v1 row count.'
    }
    foreach ($caseName in $expectedCases[$profile]) {
        if (@($requiredRows | Where-Object { $_ -ceq "1`t$caseName" }).Count -ne 1 -or
            @($resultRows | Where-Object { $_ -match "^[^`t]+`t$([regex]::Escape($caseName))`tpass$" }).Count -ne 1) {
            throw "Client profile case is missing or did not pass exactly once: $caseName"
        }
    }
    Require-Field $summary 'operator_cases_required' ([string]$expectedCases[$profile].Count)
    Require-Field $summary 'operator_cases_passed' ([string]$expectedCases[$profile].Count)

    if ($profile -eq 'multimonitor' -and [uint64]$hostFacts['display_count'] -lt 2) {
        throw 'Multimonitor evidence does not report at least two active displays.'
    }
    if ($profile -eq 'battery' -and $hostFacts['battery_present'] -ne '1') {
        throw 'Battery evidence does not report a detected battery.'
    }
    if ($profile -eq 'low-end' -and [uint64]$hostFacts['physical_memory_bytes'] -gt 8GB -and
        [uint64]$hostFacts['logical_processors'] -gt 4) {
        throw 'Low-end evidence does not meet the documented hardware bound.'
    }

    if ($null -eq $packageHash) {
        $packageHash = $summary['package_sha256']
        $packageName = $summary['package_name']
        $sourceRevision = $summary['source_revision']
    } elseif ($summary['package_sha256'] -cne $packageHash -or
              $summary['package_name'] -cne $packageName -or
              $summary['source_revision'] -cne $sourceRevision) {
        throw 'Every client matrix bundle must bind the same package and source revision.'
    }
    if ($summary['package_authenticode_valid'] -ne '1') { $allSigned = 0 }
    if ($summary['package_timestamped'] -ne '1') { $allTimestamped = 0 }
    if ($RequireAuthenticode -and
        ($summary['package_authenticode_valid'] -ne '1' -or
         $summary['package_timestamped'] -ne '1')) {
        throw 'The official matrix requires valid timestamped Authenticode on all shipped executables.'
    }

    $profileCounts[$profile]++
    if ($profile -eq 'standard' -and $hostFacts['os_family'] -eq 'windows10_22h2') {
        $standardWindows10++
    }
    if ($profile -eq 'standard' -and $hostFacts['os_family'] -eq 'windows11') {
        $standardWindows11++
    }
    $rows += "{0}`t{1}`t{2}`t{3}`t{4}" -f $manifestHash,
        $hostFacts['os_family'], $hostFacts['os_build'], $profile, $hostFacts['architecture']
}

if ($standardWindows10 -lt 1 -or $standardWindows11 -lt 1 -or
    $profileCounts['multimonitor'] -lt 1 -or $profileCounts['low-end'] -lt 1 -or
    $profileCounts['battery'] -lt 1) {
    throw 'Matrix coverage requires standard Windows 10 and Windows 11 plus multimonitor, low-end, and battery profiles.'
}

[IO.Directory]::CreateDirectory($staging) | Out-Null
try {
    $sourceLines = @("bundle_manifest_sha256`tos_family`tos_build`tprofile`tarchitecture") +
                   @($rows | Sort-Object)
    Write-AtomicText (Join-Path $staging 'sources.tsv') (($sourceLines -join "`r`n") + "`r`n")
    $summaryLines = @(
        'format=1', 'state=passed', "source_revision=$sourceRevision",
        "package_name=$packageName", "package_sha256=$packageHash",
        "evidence_bundle_count=$($rows.Count)",
        "standard_windows10_count=$standardWindows10",
        "standard_windows11_count=$standardWindows11",
        "multimonitor_count=$($profileCounts['multimonitor'])",
        "low_end_count=$($profileCounts['low-end'])",
        "battery_count=$($profileCounts['battery'])",
        "package_authenticode_valid=$allSigned", "package_timestamped=$allTimestamped",
        'clean_client_matrix_satisfied=1', 'physical_matrix_satisfied=1',
        "official_signed_matrix_satisfied=$(if ($RequireAuthenticode.IsPresent -and $allSigned -eq 1 -and $allTimestamped -eq 1) { 1 } else { 0 })"
    )
    Write-AtomicText (Join-Path $staging 'summary.ini') (($summaryLines -join "`n") + "`n")
    $manifestLines = @('format=1', 'algorithm=sha256', 'file_count=2')
    foreach ($relative in @('sources.tsv', 'summary.ini')) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $staging $relative) -Algorithm SHA256).Hash.ToLowerInvariant()
        $manifestLines += "$relative=$hash"
    }
    Write-AtomicText (Join-Path $staging 'manifest.sha256.ini') (($manifestLines -join "`n") + "`n")
    [IO.Directory]::Move($staging, $output)
    Write-Host "Client matrix verified: $output"
} catch {
    throw
}
