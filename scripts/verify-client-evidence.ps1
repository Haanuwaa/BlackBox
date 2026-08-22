[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CampaignDirectory,

    [switch]$AllowStaging,
    [switch]$RequireInteractive,
    [switch]$RequireAuthenticode
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$invariant = [Globalization.CultureInfo]::InvariantCulture

function Read-DirectV1([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { throw "Missing direct-v1 artifact: $Path" }
    $fields = @{}
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ([string]::IsNullOrWhiteSpace($line)) { throw "Blank direct-v1 line: $Path" }
        $separator = $line.IndexOf('=')
        if ($separator -lt 1) { throw "Malformed direct-v1 line: $Path" }
        $name = $line.Substring(0, $separator)
        if ($fields.ContainsKey($name)) { throw "Duplicate direct-v1 field: $name" }
        $fields[$name] = $line.Substring($separator + 1)
    }
    if ($fields['format'] -ne '1') { throw "Artifact is not direct format v1: $Path" }
    return $fields
}

function Require-Value($Fields, [string]$Name, [string]$Value) {
    if (-not $Fields.ContainsKey($Name) -or $Fields[$Name] -cne $Value) {
        throw "Required client evidence mismatch: $Name=$Value"
    }
}

function Read-UInt($Fields, [string]$Name) {
    if (-not $Fields.ContainsKey($Name)) { throw "Missing unsigned field: $Name" }
    [uint64]$value = 0
    if (-not [uint64]::TryParse([string]$Fields[$Name],
                                [Globalization.NumberStyles]::None,
                                $invariant, [ref]$value)) {
        throw "Malformed unsigned field: $Name"
    }
    return $value
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
        throw 'A client evidence path resolved outside the campaign root.'
    }
    $relative = $pathFull.Substring($prefix.Length).Replace('\', '/')
    if ([string]::IsNullOrEmpty($relative) -or [IO.Path]::IsPathRooted($relative)) {
        throw 'A client evidence path could not be normalized safely.'
    }
    return $relative
}

function Get-SignatureFacts([string]$Path) {
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    return @{
        Status = [string]$signature.Status
        Valid = if ($signature.Status -eq 'Valid') { 1 } else { 0 }
        Timestamped = if ($signature.Status -eq 'Valid' -and
            $null -ne $signature.TimeStamperCertificate) { 1 } else { 0 }
        SignerThumbprint = if ($null -ne $signature.SignerCertificate) {
            $signature.SignerCertificate.Thumbprint.ToLowerInvariant()
        } else { 'none' }
        TimestampThumbprint = if ($null -ne $signature.TimeStamperCertificate) {
            $signature.TimeStamperCertificate.Thumbprint.ToLowerInvariant()
        } else { 'none' }
    }
}

$expectedCases = @{
    standard = @(
        'package_launch_ordinary_user', 'tray_hide_restore', 'global_hotkey_capture',
        'incident_view', 'settings_diagnostics', 'keyboard_navigation',
        'high_contrast_live_toggle', 'hidden_high_contrast_catchup',
        'scale_100', 'scale_125', 'scale_150', 'scale_200')
    multimonitor = @('mixed_scale_monitor_move', 'taskbar_work_area_change',
                     'monitor_disconnect_reconnect', 'suspend_resume')
    'low-end' = @('low_end_responsiveness', 'low_end_resource_bounds')
    battery = @('battery_operation', 'battery_saver', 'balanced_power',
                'performance_power', 'suspend_resume_battery')
}

$directory = [IO.Path]::GetFullPath($CampaignDirectory)
if (-not [IO.Directory]::Exists($directory)) { throw 'Client evidence directory does not exist.' }
$directoryItem = Get-Item -LiteralPath $directory -Force
if (($directoryItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'Client evidence directory cannot be a link or reparse point.'
}
if (-not $AllowStaging.IsPresent -and $directory.TrimEnd('\', '/').EndsWith('.partial')) {
    throw 'Partial client evidence cannot satisfy verification.'
}

$summary = Read-DirectV1 (Join-Path $directory 'summary.ini')
if (-not $summary.ContainsKey('package_name') -or
    $summary['package_name'] -notmatch '^[^\\/:]+\.zip$') {
    throw 'Client evidence contains an unsafe package name.'
}
$packageName = $summary['package_name']
$expectedFiles = @(
    'app-report.ini', $packageName, "$packageName.sha256", 'campaign.ini',
    'data/incidents.sqlite3', 'data/product-settings.ini', 'data/recorder-settings.ini',
    'host.ini', 'manifest.sha256.ini', 'operator-results.tsv', 'process-samples.tsv',
    'required-cases.tsv', 'summary.ini'
)
$actualFiles = @(Get-ChildItem -LiteralPath $directory -Recurse -Force -File)
if ($actualFiles.Count -ne $expectedFiles.Count) {
    throw 'Client evidence does not contain the exact direct-v1 file count.'
}
$actualRelative = @()
foreach ($file in $actualFiles) {
    if (($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'Client evidence files cannot be links or reparse points.'
    }
    $relative = Get-BundleRelativePath $directory $file.FullName
    $actualRelative += $relative
    $limit = switch ($relative) {
        $packageName { 512MB }
        "$packageName.sha256" { 1KB }
        'data/incidents.sqlite3' { 1GB }
        { $_ -in @('operator-results.tsv', 'process-samples.tsv', 'required-cases.tsv') } { 32MB }
        default { 1MB }
    }
    if ($file.Length -le 0 -or $file.Length -gt $limit) {
        throw "Client evidence file violates its size bound: $relative"
    }
}
if (Compare-Object ($expectedFiles | Sort-Object) ($actualRelative | Sort-Object)) {
    throw 'Client evidence contains an unexpected or missing path.'
}
$directories = @(Get-ChildItem -LiteralPath $directory -Recurse -Force -Directory)
$directoryRelative = @($directories | ForEach-Object {
    if (($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'Client evidence directories cannot be links or reparse points.'
    }
    Get-BundleRelativePath $directory $_.FullName
} | Sort-Object)
if (Compare-Object @('data', 'data/crashes') $directoryRelative -CaseSensitive -SyncWindow 0 -ErrorAction Stop) {
    throw 'Client evidence must contain only data and empty crash directories.'
}
if (@(Get-ChildItem -LiteralPath (Join-Path $directory 'data/crashes') -Force).Count -ne 0) {
    throw 'Passed client evidence cannot contain crash dumps or staging files.'
}

$manifest = Read-DirectV1 (Join-Path $directory 'manifest.sha256.ini')
Require-Value $manifest 'algorithm' 'sha256'
if ((Read-UInt $manifest 'file_count') -ne ($expectedFiles.Count - 1) -or
    $manifest.Count -ne ($expectedFiles.Count + 2)) {
    throw 'Client evidence manifest field count is invalid.'
}
foreach ($relative in $expectedFiles | Where-Object { $_ -ne 'manifest.sha256.ini' }) {
    if (-not $manifest.ContainsKey($relative) -or
        $manifest[$relative] -notmatch '^[0-9a-f]{64}$') {
        throw "Client evidence manifest omits or malforms: $relative"
    }
    $hash = (Get-FileHash -LiteralPath (Join-Path $directory $relative) `
                         -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -cne $manifest[$relative]) { throw "Changed client evidence: $relative" }
}

Require-Value $summary 'state' 'passed'
$mode = $summary['mode']
$profile = $summary['profile']
if ($mode -notin @('smoke', 'interactive') -or -not $expectedCases.ContainsKey($profile)) {
    throw 'Client evidence mode or profile is invalid.'
}
if ($RequireInteractive.IsPresent -and $mode -cne 'interactive') {
    throw 'This operation requires interactive client evidence.'
}
$sourceRevision = $summary['source_revision']
if (($mode -eq 'interactive' -and $sourceRevision -notmatch '^[0-9a-f]{40}$') -or
    ($mode -eq 'smoke' -and $sourceRevision -notmatch '^(local-uncommitted|[0-9a-f]{40})$')) {
    throw 'Client evidence source revision is malformed.'
}
foreach ($name in @('application_sha256', 'runner_sha256', 'verifier_sha256')) {
    if (-not $summary.ContainsKey($name) -or $summary[$name] -notmatch '^[0-9a-f]{64}$') {
        throw "Client evidence provenance is malformed: $name"
    }
}

$campaign = Read-DirectV1 (Join-Path $directory 'campaign.ini')
foreach ($pair in @(@('state', 'passed'), @('mode', $mode), @('profile', $profile),
                     @('source_revision', $sourceRevision), @('process_id', '0'),
                     @('application_sha256', $summary['application_sha256']),
                     @('runner_sha256', $summary['runner_sha256']),
                     @('verifier_sha256', $summary['verifier_sha256']))) {
    Require-Value $campaign $pair[0] $pair[1]
}
[void](Read-DirectV1 (Join-Path $directory 'data/product-settings.ini'))
[void](Read-DirectV1 (Join-Path $directory 'data/recorder-settings.ini'))

$archive = Join-Path $directory 'data/incidents.sqlite3'
$archiveStream = [IO.File]::OpenRead($archive)
try {
    $header = [byte[]]::new(16)
    if ($archiveStream.Length -lt 512 -or $archiveStream.Length -gt 1GB -or
        $archiveStream.Read($header, 0, $header.Length) -ne $header.Length -or
        [Text.Encoding]::ASCII.GetString($header) -cne "SQLite format 3`0") {
        throw 'Client evidence archive is not a bounded SQLite format-3 file.'
    }
} finally { $archiveStream.Dispose() }

$report = Read-DirectV1 (Join-Path $directory 'app-report.ini')
foreach ($pair in @(
    @('completed', '1'), @('archive_healthy', '1'), @('archive_schema_version', '1'),
    @('failed_samples', '0'), @('dropped_samples', '0'), @('deadline_misses', '0'),
    @('collector_worker_failures', '0'), @('snapshot_failures', '0'),
    @('event_worker_failures', '0'), @('native_events_dropped', '0'),
    @('writer_cancelled', '0'), @('writer_retry_exhausted', '0'), @('writer_failed', '0'))) {
    Require-Value $report $pair[0] $pair[1]
}
if ((Read-UInt $report 'collections') -lt 1 -or
    (Read-UInt $report 'archive_incidents') -ne (Read-UInt $report 'writer_succeeded') -or
    (Read-UInt $report 'incidents_completed') -ne (Read-UInt $report 'writer_succeeded')) {
    throw 'Client report collection/capture/writer/archive accounting is invalid.'
}
$eventCount = (Read-UInt $report 'power_events_recorded') +
              (Read-UInt $report 'device_events_recorded') +
              (Read-UInt $report 'audio_events_recorded') +
              (Read-UInt $report 'service_events_recorded') +
              (Read-UInt $report 'defender_events_recorded') +
              (Read-UInt $report 'windows_update_events_recorded') +
              (Read-UInt $report 'application_events_recorded') +
              (Read-UInt $report 'network_events_recorded') +
              (Read-UInt $report 'graphics_events_recorded') +
              (Read-UInt $report 'storage_events_recorded')
if ($eventCount -ne (Read-UInt $report 'system_events_recorded')) {
    throw 'Client report event-source accounting is invalid.'
}
if ((Read-UInt $summary 'maximum_working_set_bytes') -gt 80MB) {
    throw 'Client evidence exceeded its 80 MiB working-set gate.'
}
[void](Read-UInt $summary 'maximum_private_bytes')
[void](Read-UInt $summary 'maximum_handles')

$processRows = [IO.File]::ReadAllLines((Join-Path $directory 'process-samples.tsv'))
if ($processRows.Count -lt 2 -or $processRows[0] -cne
    "elapsed_seconds`tutc`tphase`tworking_set_bytes`tprivate_bytes`thandles`ttotal_cpu_seconds") {
    throw 'Client evidence process journal is empty or malformed.'
}
foreach ($row in $processRows | Select-Object -Skip 1) {
    $columns = $row.Split("`t")
    [double]$number = 0.0
    [uint64]$unsigned = 0
    [DateTimeOffset]$utc = [DateTimeOffset]::MinValue
    if ($columns.Count -ne 7 -or
        -not [double]::TryParse($columns[0], [Globalization.NumberStyles]::AllowDecimalPoint,
                                $invariant, [ref]$number) -or $number -lt 0 -or
        -not [DateTimeOffset]::TryParse($columns[1], $invariant,
                                        [Globalization.DateTimeStyles]::RoundtripKind, [ref]$utc) -or
        $columns[2] -notin @('interactive', 'diagnostic') -or
        -not [uint64]::TryParse($columns[3], [ref]$unsigned) -or
        -not [uint64]::TryParse($columns[4], [ref]$unsigned) -or
        -not [uint64]::TryParse($columns[5], [ref]$unsigned) -or
        -not [double]::TryParse($columns[6], [Globalization.NumberStyles]::AllowDecimalPoint,
                                $invariant, [ref]$number) -or $number -lt 0) {
        throw 'Client evidence process journal contains a malformed row.'
    }
}

$requiredRows = [IO.File]::ReadAllLines((Join-Path $directory 'required-cases.tsv'))
$resultRows = [IO.File]::ReadAllLines((Join-Path $directory 'operator-results.tsv'))
if ($mode -eq 'interactive') {
    $cases = @($expectedCases[$profile])
} else {
    $cases = @()
}
if ($requiredRows.Count -ne ($cases.Count + 1) -or $requiredRows[0] -cne "format`tcase" -or
    $resultRows.Count -ne ($cases.Count + 1) -or $resultRows[0] -cne "utc`tcase`tresult") {
    throw 'Client case artifacts do not have the exact expected shape.'
}
foreach ($caseName in $cases) {
    if (@($requiredRows | Where-Object { $_ -ceq "1`t$caseName" }).Count -ne 1 -or
        @($resultRows | Where-Object {
            $_ -match "^[^`t]+`t$([regex]::Escape($caseName))`tpass$"
        }).Count -ne 1) {
        throw "Client case did not pass exactly once: $caseName"
    }
}
Require-Value $summary 'operator_cases_required' ([string]$cases.Count)
Require-Value $summary 'operator_cases_passed' ([string]$cases.Count)
Require-Value $summary 'automated_package_smoke_satisfied' '1'
Require-Value $summary 'clean_client_matrix_satisfied' '0'
Require-Value $summary 'physical_matrix_satisfied' '0'
Require-Value $summary 'single_host_profile_satisfied' $(if ($mode -eq 'interactive') { '1' } else { '0' })

$hostFields = Read-DirectV1 (Join-Path $directory 'host.ini')
if ($mode -eq 'interactive') {
    Require-Value $hostFields 'supported_client' '1'
    if ($hostFields['os_family'] -notin @('windows10_22h2', 'windows11')) {
        throw 'Interactive client evidence is not a supported Windows family.'
    }
    if ($profile -eq 'multimonitor' -and (Read-UInt $hostFields 'display_count') -lt 2) {
        throw 'Multimonitor evidence lacks two active displays.'
    }
    if ($profile -eq 'battery' -and $hostFields['battery_present'] -cne '1') {
        throw 'Battery evidence lacks a detected battery.'
    }
    if ($profile -eq 'low-end' -and (Read-UInt $hostFields 'physical_memory_bytes') -gt 8GB -and
        (Read-UInt $hostFields 'logical_processors') -gt 4) {
        throw 'Low-end evidence does not meet its hardware ceiling.'
    }
}

$package = Join-Path $directory $packageName
$packageHash = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant()
if ($summary['package_sha256'] -cne $packageHash) {
    throw 'Client evidence package hash does not match its summary.'
}
& (Join-Path $PSScriptRoot 'verify-release.ps1') -PackagePath $package `
    -RequireAuthenticode:$RequireAuthenticode.IsPresent | Out-Null

$temporary = Join-Path ([IO.Path]::GetTempPath()) ("blackbox-client-verify-" + [guid]::NewGuid())
try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($package, $temporary)
    $root = Join-Path $temporary ([IO.Path]::GetFileNameWithoutExtension($package))
    $allValid = 1
    $allTimestamped = 1
    foreach ($binary in @('blackbox.exe', 'blackbox_dataset_tool.exe', 'blackbox_dogfood_tool.exe')) {
        $path = Join-Path $root $binary
        $key = $binary.Replace('.', '_')
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        Require-Value $summary "$key.sha256" $hash
        $facts = Get-SignatureFacts $path
        Require-Value $summary "$key.signature_status" $facts.Status
        Require-Value $summary "$key.signer_thumbprint" $facts.SignerThumbprint
        Require-Value $summary "$key.timestamp_thumbprint" $facts.TimestampThumbprint
        if ($facts.Valid -ne 1) { $allValid = 0 }
        if ($facts.Timestamped -ne 1) { $allTimestamped = 0 }
        if ($binary -ceq 'blackbox.exe' -and $summary['application_sha256'] -cne $hash) {
            throw 'Operator/application provenance does not match packaged blackbox.exe.'
        }
    }
    Require-Value $summary 'package_authenticode_valid' ([string]$allValid)
    Require-Value $summary 'package_timestamped' ([string]$allTimestamped)
    if ($RequireAuthenticode.IsPresent -and ($allValid -ne 1 -or $allTimestamped -ne 1)) {
        throw 'Official client evidence requires valid timestamped executables.'
    }
} finally {
    if ([IO.Directory]::Exists($temporary)) {
        Remove-Item -LiteralPath $temporary -Recurse -Force
    }
}

Write-Output "Verified passed $mode client evidence: $directory"
