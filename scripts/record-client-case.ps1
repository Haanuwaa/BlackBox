[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CampaignDirectory,

    [Parameter(Mandatory = $true)]
    [ValidateSet(
        'package_launch_ordinary_user', 'tray_hide_restore', 'global_hotkey_capture',
        'first_run_onboarding_keyboard', 'focus_visibility_text_scaling',
        'incident_view', 'settings_diagnostics', 'keyboard_navigation',
        'high_contrast_live_toggle', 'hidden_high_contrast_catchup',
        'scale_100', 'scale_125', 'scale_150', 'scale_200',
        'mixed_scale_monitor_move', 'taskbar_work_area_change',
        'monitor_disconnect_reconnect', 'suspend_resume',
        'low_end_responsiveness', 'low_end_resource_bounds',
        'battery_operation', 'battery_saver', 'balanced_power',
        'performance_power', 'suspend_resume_battery')]
    [string]$Case,

    [Parameter(Mandatory = $true)]
    [ValidateSet('pass', 'fail')]
    [string]$Result
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$campaign = [IO.Path]::GetFullPath($CampaignDirectory)
$marker = Join-Path $campaign 'campaign.ini'
$requiredPath = Join-Path $campaign 'required-cases.tsv'
$resultsPath = Join-Path $campaign 'operator-results.tsv'
if (-not [IO.File]::Exists($marker) -or -not [IO.File]::Exists($requiredPath) -or
    -not [IO.File]::Exists($resultsPath)) {
    throw 'The client qualification campaign is incomplete.'
}
$markerFields = @{}
foreach ($line in [IO.File]::ReadAllLines($marker)) {
    $separator = $line.IndexOf('=')
    if ($separator -lt 1) { throw 'The campaign marker is malformed.' }
    $name = $line.Substring(0, $separator)
    if ($markerFields.ContainsKey($name)) { throw 'The campaign marker has duplicate fields.' }
    $markerFields[$name] = $line.Substring($separator + 1)
}
if ($markerFields['format'] -ne '1' -or $markerFields['state'] -ne 'running' -or
    $markerFields['mode'] -ne 'interactive') {
    throw 'Cases can only be recorded for a running direct-v1 interactive campaign.'
}
$targetProcessId = [int]$markerFields['process_id']
if ($targetProcessId -le 0 -or $markerFields['application_sha256'] -notmatch '^[0-9a-f]{64}$') {
    throw 'The interactive packaged application provenance is malformed.'
}
$targetProcess = Get-Process -Id $targetProcessId -ErrorAction SilentlyContinue
if ($null -eq $targetProcess) {
    throw 'The interactive packaged application is not running.'
}
try {
    $targetPath = $targetProcess.Path
} catch {
    throw 'The interactive packaged application path could not be verified.'
}
if ([string]::IsNullOrWhiteSpace($targetPath) -or
    (Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne
        $markerFields['application_sha256']) {
    throw 'The interactive process does not match the package application hash.'
}

$requiredRows = @([IO.File]::ReadAllLines($requiredPath))
if ($requiredRows.Count -lt 2 -or $requiredRows[0] -cne "format`tcase") {
    throw 'The required-case artifact is malformed.'
}
$requiredCases = @($requiredRows | Select-Object -Skip 1 | ForEach-Object {
    $parts = $_.Split("`t")
    if ($parts.Count -ne 2 -or $parts[0] -cne '1' -or
        [string]::IsNullOrWhiteSpace($parts[1])) {
        throw 'A required-case row is malformed.'
    }
    $parts[1]
})
if ($requiredCases -cnotcontains $Case) {
    throw "The case is not required by this campaign profile: $Case"
}

$stream = [IO.File]::Open($resultsPath, [IO.FileMode]::Open,
                          [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
try {
    $reader = [IO.StreamReader]::new($stream, [Text.UTF8Encoding]::new($false),
                                     $true, 1024, $true)
    $existing = $reader.ReadToEnd()
    $reader.Dispose()
    if (-not $existing.StartsWith("utc`tcase`tresult`r`n")) {
        throw 'The operator-result artifact is malformed.'
    }
    if ($existing -match "(?m)^.*`t$([regex]::Escape($Case))`t(?:pass|fail)`r?$") {
        throw "The case already has a recorded result: $Case"
    }
    $line = "{0}`t{1}`t{2}`r`n" -f [DateTimeOffset]::UtcNow.ToString('O'), $Case, $Result
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($line)
    $stream.Seek(0, [IO.SeekOrigin]::End) | Out-Null
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush($true)
} finally {
    $stream.Dispose()
}

Write-Host "Recorded $Result for $Case."
