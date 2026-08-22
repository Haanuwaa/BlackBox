[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackagePath,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [ValidateSet('smoke', 'interactive')]
    [string]$Mode = 'smoke',

    [ValidateSet('standard', 'multimonitor', 'low-end', 'battery')]
    [string]$Profile = 'standard',

    [string]$SourceRevision = 'local-uncommitted',
    [int]$DiagnosticSeconds = 15,
    [int]$InteractiveTimeoutSeconds = 7200,
    [switch]$RequireAuthenticode
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

function Read-DirectV1([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { throw "Missing direct-v1 artifact: $Path" }
    $fields = @{}
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            throw 'Direct-v1 artifacts cannot contain blank lines.'
        }
        $separator = $line.IndexOf('=')
        if ($separator -lt 1) { throw 'Direct-v1 artifact contains a malformed field.' }
        $name = $line.Substring(0, $separator)
        if ($fields.ContainsKey($name)) { throw "Duplicate direct-v1 field: $name" }
        $fields[$name] = $line.Substring($separator + 1)
    }
    if ($fields['format'] -ne '1') { throw 'Artifact is not direct format v1.' }
    return $fields
}

function Convert-SafeValue([object]$Value) {
    if ($null -eq $Value) { return 'unavailable' }
    $text = ([string]$Value).Trim() -replace '[\r\n\t=]', ' '
    $text = $text -replace '[^\x20-\x7e]', '?'
    if ([string]::IsNullOrWhiteSpace($text)) { return 'unavailable' }
    if ($text.Length -gt 240) { return $text.Substring(0, 240) }
    return $text
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

function Add-DisplayProbeType {
    if ('BlackBoxClientDisplayProbe' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class BlackBoxClientDisplayProbe {
    private delegate bool MonitorEnumProc(IntPtr monitor, IntPtr dc, ref RECT rect, IntPtr data);
    [StructLayout(LayoutKind.Sequential)] private struct RECT {
        public int Left, Top, Right, Bottom;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] private struct MONITORINFOEX {
        public int Size;
        public RECT Monitor;
        public RECT Work;
        public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Device;
    }
    [DllImport("user32.dll")] private static extern bool EnumDisplayMonitors(
        IntPtr dc, IntPtr clip, MonitorEnumProc callback, IntPtr data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool GetMonitorInfo(
        IntPtr monitor, ref MONITORINFOEX info);
    [DllImport("shcore.dll")] private static extern int GetDpiForMonitor(
        IntPtr monitor, int type, out uint dpiX, out uint dpiY);

    public static string[] Read() {
        var rows = new List<string>();
        MonitorEnumProc callback = delegate(IntPtr monitor, IntPtr dc, ref RECT ignored, IntPtr data) {
            var info = new MONITORINFOEX();
            info.Size = Marshal.SizeOf(info);
            if (!GetMonitorInfo(monitor, ref info)) return true;
            uint dpiX = 0, dpiY = 0;
            try {
                if (GetDpiForMonitor(monitor, 0, out dpiX, out dpiY) != 0) {
                    dpiX = 0; dpiY = 0;
                }
            } catch (DllNotFoundException) { dpiX = 0; dpiY = 0; }
            rows.Add(String.Format(
                "device:{0};bounds:{1},{2},{3},{4};work:{5},{6},{7},{8};primary:{9};dpi:{10},{11};scale_percent:{12}",
                info.Device, info.Monitor.Left, info.Monitor.Top,
                info.Monitor.Right - info.Monitor.Left, info.Monitor.Bottom - info.Monitor.Top,
                info.Work.Left, info.Work.Top, info.Work.Right - info.Work.Left,
                info.Work.Bottom - info.Work.Top, (info.Flags & 1) != 0 ? 1 : 0,
                dpiX, dpiY, dpiX == 0 ? 0 : (int)Math.Round(dpiX * 100.0 / 96.0)));
            return true;
        };
        if (!EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, callback, IntPtr.Zero)) {
            throw new InvalidOperationException("EnumDisplayMonitors failed.");
        }
        rows.Sort(StringComparer.Ordinal);
        return rows.ToArray();
    }
}
'@
}

function Get-HostFacts {
    $currentVersion = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    $build = [int]$currentVersion.CurrentBuildNumber
    $ubr = [int]$currentVersion.UBR
    $family = if ($build -ge 22000) { 'windows11' }
              elseif ($build -eq 19045) { 'windows10_22h2' }
              else { 'unsupported_windows' }
    $processor = Get-CimInstance -ClassName Win32_Processor | Select-Object -First 1
    $computer = Get-CimInstance -ClassName Win32_ComputerSystem
    $gpuNames = @(Get-CimInstance -ClassName Win32_VideoController |
                  ForEach-Object { Convert-SafeValue $_.Name } | Sort-Object -Unique)
    $battery = @(Get-CimInstance -ClassName Win32_Battery -ErrorAction SilentlyContinue)
    Add-Type -AssemblyName System.Windows.Forms
    Add-DisplayProbeType
    $displays = @([BlackBoxClientDisplayProbe]::Read())
    $powerStatus = [Windows.Forms.SystemInformation]::PowerStatus
    $powerScheme = Convert-SafeValue (& powercfg.exe /getactivescheme 2>$null)
    return @{
        OsFamily = $family
        OsProduct = Convert-SafeValue $currentVersion.ProductName
        OsEdition = Convert-SafeValue $currentVersion.EditionID
        OsDisplayVersion = Convert-SafeValue $currentVersion.DisplayVersion
        OsBuild = "$build.$ubr"
        Architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
        Cpu = Convert-SafeValue $processor.Name
        LogicalProcessors = [int][Environment]::ProcessorCount
        PhysicalMemoryBytes = [uint64]$computer.TotalPhysicalMemory
        Gpu = Convert-SafeValue ($gpuNames -join ' | ')
        ProcessCount = @(Get-Process).Count
        BatteryPresent = if ($battery.Count -gt 0) { 1 } else { 0 }
        PowerLineStatus = Convert-SafeValue $powerStatus.PowerLineStatus
        BatteryChargePercent = [math]::Round($powerStatus.BatteryLifePercent * 100.0, 1)
        ActivePowerScheme = $powerScheme
        HighContrast = if ([Windows.Forms.SystemInformation]::HighContrast) { 1 } else { 0 }
        MenuAnimation = if ([Windows.Forms.SystemInformation]::IsMenuAnimationEnabled) { 1 } else { 0 }
        KeyboardPreferred = if ([Windows.Forms.SystemInformation]::IsKeyboardPreferred) { 1 } else { 0 }
        MenuAccessKeysUnderlined = if ([Windows.Forms.SystemInformation]::MenuAccessKeysUnderlined) { 1 } else { 0 }
        DisplayCount = $displays.Count
        Displays = $displays
        Supported = if ($family -ne 'unsupported_windows' -and
            [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq
                [Runtime.InteropServices.Architecture]::X64) { 1 } else { 0 }
    }
}

function Add-ProcessSample([Diagnostics.Process]$Process, [string]$Phase,
                           [Diagnostics.Stopwatch]$Clock, [string]$Path) {
    $Process.Refresh()
    if ($Process.HasExited) { return }
    $line = [string]::Format($invariant,
        "{0:F3}`t{1}`t{2}`t{3}`t{4}`t{5}`t{6:F6}`r`n",
        [object[]]@($Clock.Elapsed.TotalSeconds, [DateTimeOffset]::UtcNow.ToString('O'), $Phase,
                   $Process.WorkingSet64, $Process.PrivateMemorySize64, $Process.HandleCount,
                   $Process.TotalProcessorTime.TotalSeconds))
    [IO.File]::AppendAllText($Path, $line, [Text.UTF8Encoding]::new($false))
}

if ($env:OS -ne 'Windows_NT') { throw 'Client qualification requires Windows.' }
if ($DiagnosticSeconds -lt 10 -or $DiagnosticSeconds -gt 300) {
    throw 'DiagnosticSeconds must be between 10 and 300.'
}
if ($InteractiveTimeoutSeconds -lt 60 -or $InteractiveTimeoutSeconds -gt 28800) {
    throw 'InteractiveTimeoutSeconds must be between 60 and 28800.'
}
if ($Mode -eq 'smoke' -and $Profile -ne 'standard') {
    throw 'Smoke mode uses only the standard profile.'
}
if ($Mode -eq 'interactive' -and $SourceRevision -notmatch '^[0-9A-Fa-f]{40}$') {
    throw 'Interactive evidence requires a 40-character source revision.'
}

$package = (Resolve-Path -LiteralPath $PackagePath -ErrorAction Stop).Path
$checksum = "$package.sha256"
$output = [IO.Path]::GetFullPath($OutputDirectory)
$staging = "$output.partial"
if ([IO.Directory]::Exists($output) -or [IO.File]::Exists($output) -or
    [IO.Directory]::Exists($staging) -or [IO.File]::Exists($staging)) {
    throw 'The client qualification output and staging destinations must not already exist.'
}

$existingBlackBox = @(Get-Process -Name 'blackbox' -ErrorAction SilentlyContinue)
if ($existingBlackBox.Count -ne 0) {
    $processIds = (($existingBlackBox | Select-Object -ExpandProperty Id | Sort-Object) -join ',')
    throw "Client qualification requires a clean single-instance host; stop the existing BlackBox process(es): $processIds"
}

$hostFacts = Get-HostFacts
if ($Mode -eq 'interactive' -and $hostFacts.Supported -ne 1) {
    throw 'Interactive qualification requires a supported x64 Windows client.'
}
if ($Mode -eq 'interactive' -and $Profile -eq 'multimonitor' -and
    $hostFacts.DisplayCount -lt 2) {
    throw 'The multimonitor profile requires at least two active displays.'
}
if ($Mode -eq 'interactive' -and $Profile -eq 'battery' -and
    $hostFacts.BatteryPresent -ne 1) {
    throw 'The battery profile requires a host with a detected battery.'
}
if ($Mode -eq 'interactive' -and $Profile -eq 'low-end' -and
    $hostFacts.PhysicalMemoryBytes -gt 8GB -and $hostFacts.LogicalProcessors -gt 4) {
    throw 'The low-end profile requires at most 8 GiB RAM or at most four logical processors.'
}

$requiredByProfile = @{
    standard = @(
        'package_launch_ordinary_user', 'tray_hide_restore', 'global_hotkey_capture',
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
$requiredCases = @()
if ($Mode -eq 'interactive') {
    $requiredCases = @($requiredByProfile[$Profile])
}

[IO.Directory]::CreateDirectory($staging) | Out-Null
$campaignPath = Join-Path $staging 'campaign.ini'
$samplesPath = Join-Path $staging 'process-samples.tsv'
$resultsPath = Join-Path $staging 'operator-results.tsv'
$requiredPath = Join-Path $staging 'required-cases.tsv'
$reportPath = Join-Path $staging 'app-report.ini'
$dataPath = Join-Path $staging 'data'
$extractPath = Join-Path $staging 'extracted'
[IO.Directory]::CreateDirectory($dataPath) | Out-Null
[IO.Directory]::CreateDirectory($extractPath) | Out-Null

$copiedPackage = Join-Path $staging ([IO.Path]::GetFileName($package))
$copiedChecksum = "$copiedPackage.sha256"
[IO.File]::Copy($package, $copiedPackage, $false)
[IO.File]::Copy($checksum, $copiedChecksum, $false)
& (Join-Path $PSScriptRoot 'verify-release.ps1') -PackagePath $copiedPackage `
    -RequireAuthenticode:$RequireAuthenticode.IsPresent `
    -ExpectedSourceRevision ($SourceRevision.ToLowerInvariant())
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::ExtractToDirectory($copiedPackage, $extractPath)
$packageRoot = Join-Path $extractPath ([IO.Path]::GetFileNameWithoutExtension($package))
$application = Join-Path $packageRoot 'blackbox.exe'
if (-not [IO.File]::Exists($application)) { throw 'The extracted package application is missing.' }
$runnerScript = [IO.Path]::GetFullPath($PSCommandPath)
$verifierScript = Join-Path $PSScriptRoot 'verify-client-evidence.ps1'
if (-not [IO.File]::Exists($runnerScript) -or -not [IO.File]::Exists($verifierScript)) {
    throw 'The client runner and evidence verifier scripts must both exist.'
}
$applicationHash = (Get-FileHash -LiteralPath $application -Algorithm SHA256).Hash.ToLowerInvariant()
$runnerHash = (Get-FileHash -LiteralPath $runnerScript -Algorithm SHA256).Hash.ToLowerInvariant()
$verifierHash = (Get-FileHash -LiteralPath $verifierScript -Algorithm SHA256).Hash.ToLowerInvariant()
$provenance = "application_sha256=$applicationHash`nrunner_sha256=$runnerHash`n" +
              "verifier_sha256=$verifierHash`n"

$archive = Join-Path $dataPath 'incidents.sqlite3'
$productSettings = Join-Path $dataPath 'product-settings.ini'
$recorderSettings = Join-Path $dataPath 'recorder-settings.ini'
$archiveField = $archive.Replace('\', '/')
Write-AtomicText $productSettings @"
format=1
hotkey_key=12
hotkey_control=1
hotkey_shift=1
hotkey_alt=0
hotkey_windows=0
automatic_detection=0
detector_sensitivity=1
detect_cpu=1
detect_memory=1
detect_disk=1
detect_network=1
detector_cooldown_seconds=120
notifications=0
record_foreground_application=0
record_power_and_device_events=1
record_audio_device_events=1
record_windows_event_log_evidence=0
archive_path=$archiveField
archive_maximum_bytes=1073741824
onboarding_completed=1
"@
Write-AtomicText $recorderSettings @"
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

$campaignText = "format=1`nstate=running`nmode=$Mode`nprofile=$Profile`n" +
                "source_revision=$($SourceRevision.ToLowerInvariant())`nprocess_id=0`n$provenance"
Write-AtomicText $campaignPath $campaignText
[IO.File]::WriteAllText($samplesPath,
    "elapsed_seconds`tutc`tphase`tworking_set_bytes`tprivate_bytes`thandles`ttotal_cpu_seconds`r`n",
    [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText($resultsPath, "utc`tcase`tresult`r`n",
    [Text.UTF8Encoding]::new($false))
$requiredLines = @("format`tcase") + @($requiredCases | ForEach-Object { "1`t$_" })
[IO.File]::WriteAllText($requiredPath, (($requiredLines -join "`r`n") + "`r`n"),
    [Text.UTF8Encoding]::new($false))

$hostLines = @(
    'format=1',
    "os_family=$($hostFacts.OsFamily)",
    "os_product=$($hostFacts.OsProduct)",
    "os_edition=$($hostFacts.OsEdition)",
    "os_display_version=$($hostFacts.OsDisplayVersion)",
    "os_build=$($hostFacts.OsBuild)",
    "architecture=$($hostFacts.Architecture)",
    "supported_client=$($hostFacts.Supported)",
    "cpu=$($hostFacts.Cpu)",
    "logical_processors=$($hostFacts.LogicalProcessors)",
    "physical_memory_bytes=$($hostFacts.PhysicalMemoryBytes)",
    "gpu=$($hostFacts.Gpu)",
    "process_count=$($hostFacts.ProcessCount)",
    "battery_present=$($hostFacts.BatteryPresent)",
    "power_line_status=$($hostFacts.PowerLineStatus)",
    "battery_charge_percent=$($hostFacts.BatteryChargePercent)",
    "active_power_scheme=$($hostFacts.ActivePowerScheme)",
    "high_contrast=$($hostFacts.HighContrast)",
    "menu_animation=$($hostFacts.MenuAnimation)",
    "keyboard_preferred=$($hostFacts.KeyboardPreferred)",
    "menu_access_keys_underlined=$($hostFacts.MenuAccessKeysUnderlined)",
    "display_count=$($hostFacts.DisplayCount)"
)
for ($index = 0; $index -lt $hostFacts.Displays.Count; ++$index) {
    $hostLines += "display_$index=$(Convert-SafeValue $hostFacts.Displays[$index])"
}
Write-AtomicText (Join-Path $staging 'host.ini') (($hostLines -join "`n") + "`n")

$oldProduct = [Environment]::GetEnvironmentVariable('BLACKBOX_PRODUCT_SETTINGS_PATH', 'Process')
$oldRecorder = [Environment]::GetEnvironmentVariable('BLACKBOX_SETTINGS_PATH', 'Process')
$process = $null
$clock = [Diagnostics.Stopwatch]::StartNew()
$maximumWorkingSet = [uint64]0
$maximumPrivateBytes = [uint64]0
$maximumHandles = [uint64]0

try {
    [Environment]::SetEnvironmentVariable('BLACKBOX_PRODUCT_SETTINGS_PATH', $productSettings, 'Process')
    [Environment]::SetEnvironmentVariable('BLACKBOX_SETTINGS_PATH', $recorderSettings, 'Process')

    if ($Mode -eq 'interactive') {
        $process = Start-Process -FilePath $application -WorkingDirectory $packageRoot -PassThru
        Write-AtomicText $campaignPath (
            "format=1`nstate=running`nmode=$Mode`nprofile=$Profile`n" +
            "source_revision=$($SourceRevision.ToLowerInvariant())`nprocess_id=$($process.Id)`n" +
            $provenance)
        Start-Sleep -Seconds 3
        $process.Refresh()
        if ($process.HasExited) {
            throw 'The packaged interactive application did not remain running.'
        }
        Write-Host "Interactive client qualification is running in $staging"
        Write-Host 'Record each observed case with scripts/record-client-case.ps1, then exit BlackBox from its tray menu.'
        while (-not $process.HasExited) {
            if ($clock.Elapsed.TotalSeconds -gt $InteractiveTimeoutSeconds) {
                throw 'The interactive qualification timed out.'
            }
            Add-ProcessSample $process 'interactive' $clock $samplesPath
            $process.Refresh()
            if (-not $process.HasExited) { Start-Sleep -Seconds 1 }
        }
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) {
            throw "The interactive application exited with status $($process.ExitCode)."
        }

        $rows = @(Get-Content -LiteralPath $resultsPath | Select-Object -Skip 1)
        if ($rows.Count -ne $requiredCases.Count) {
            throw 'The interactive profile does not contain exactly its required case results.'
        }
        foreach ($requiredCase in $requiredCases) {
            if (@($rows | Where-Object { $_ -match "`t$([regex]::Escape($requiredCase))`tpass$" }).Count -ne 1) {
                throw "Required interactive case did not pass exactly once: $requiredCase"
            }
        }
    }

    $arguments = @(
        "--background-diagnostic-seconds=$DiagnosticSeconds",
        ('"--diagnostic-report={0}"' -f $reportPath),
        '--diagnostic-capture-interval-seconds=5'
    )
    $process = Start-Process -FilePath $application -WorkingDirectory $packageRoot -ArgumentList $arguments -WindowStyle Hidden -PassThru
    while (-not $process.HasExited) {
        Add-ProcessSample $process 'diagnostic' $clock $samplesPath
        $process.Refresh()
        if (-not $process.HasExited) { Start-Sleep -Seconds 1 }
    }
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        throw "The packaged diagnostic application exited with status $($process.ExitCode)."
    }
    $fields = Read-DirectV1 $reportPath
    if ($fields['completed'] -ne '1' -or
        [uint64]$fields['requested_runtime_seconds'] -ne [uint64]$DiagnosticSeconds -or
        $fields['archive_healthy'] -ne '1' -or $fields['archive_schema_version'] -ne '1') {
        throw 'The packaged diagnostic report did not complete against a healthy schema-v1 archive.'
    }
    $minimumCollections = [math]::Max(1, $DiagnosticSeconds - 5)
    if ([uint64]$fields['collections'] -lt [uint64]$minimumCollections) {
        throw 'The packaged diagnostic did not collect the minimum expected samples.'
    }
    foreach ($name in @('failed_samples', 'dropped_samples', 'deadline_misses',
                        'collector_worker_failures', 'snapshot_failures',
                        'capture_queue_rejections', 'event_worker_failures',
                        'native_events_dropped', 'writer_cancelled',
                        'writer_retry_exhausted', 'writer_failed')) {
        if (-not $fields.ContainsKey($name) -or [uint64]$fields[$name] -ne 0) {
            throw "Packaged diagnostic requires $name=0."
        }
    }
    if ([uint64]$fields['archive_incidents'] -ne [uint64]$fields['writer_succeeded'] -or
        [uint64]$fields['incidents_completed'] -ne [uint64]$fields['writer_succeeded']) {
        throw 'Packaged diagnostic capture, writer, and archive counts do not agree.'
    }

    foreach ($row in Get-Content -LiteralPath $samplesPath | Select-Object -Skip 1) {
        $columns = $row -split "`t"
        if ($columns.Count -ne 7) { throw 'Process sample row is malformed.' }
        $workingSet = [uint64]$columns[3]
        $privateBytes = [uint64]$columns[4]
        $handles = [uint64]$columns[5]
        if ($workingSet -gt $maximumWorkingSet) { $maximumWorkingSet = $workingSet }
        if ($privateBytes -gt $maximumPrivateBytes) { $maximumPrivateBytes = $privateBytes }
        if ($handles -gt $maximumHandles) { $maximumHandles = $handles }
    }
    if ($maximumWorkingSet -gt 80MB) {
        throw 'The packaged application exceeded the 80 MiB working-set gate.'
    }

    $binaryNames = @('blackbox.exe', 'blackbox_dataset_tool.exe', 'blackbox_dogfood_tool.exe')
    $allValid = 1
    $allTimestamped = 1
    $binaryLines = @()
    foreach ($binaryName in $binaryNames) {
        $binaryPath = Join-Path $packageRoot $binaryName
        $signature = Get-SignatureFacts $binaryPath
        if ($signature.Valid -ne 1) { $allValid = 0 }
        if ($signature.Timestamped -ne 1) { $allTimestamped = 0 }
        $key = $binaryName.Replace('.', '_')
        $binaryLines += "$key.sha256=$((Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash.ToLowerInvariant())"
        $binaryLines += "$key.signature_status=$(Convert-SafeValue $signature.Status)"
        $binaryLines += "$key.signer_thumbprint=$($signature.SignerThumbprint)"
        $binaryLines += "$key.timestamp_thumbprint=$($signature.TimestampThumbprint)"
    }
    if ($RequireAuthenticode -and ($allValid -ne 1 -or $allTimestamped -ne 1)) {
        throw 'The qualification requires every shipped executable to be valid and timestamped.'
    }
    if ((Get-FileHash -LiteralPath $application -Algorithm SHA256).Hash.ToLowerInvariant() -cne
            $applicationHash -or
        (Get-FileHash -LiteralPath $runnerScript -Algorithm SHA256).Hash.ToLowerInvariant() -cne
            $runnerHash -or
        (Get-FileHash -LiteralPath $verifierScript -Algorithm SHA256).Hash.ToLowerInvariant() -cne
            $verifierHash) {
        throw 'The launched application or client qualification tools changed during the campaign.'
    }

    $packageHash = (Get-FileHash -LiteralPath $copiedPackage -Algorithm SHA256).Hash.ToLowerInvariant()
    $summaryLines = @(
        'format=1', 'state=passed', "mode=$Mode", "profile=$Profile",
        "source_revision=$($SourceRevision.ToLowerInvariant())",
        "application_sha256=$applicationHash", "runner_sha256=$runnerHash",
        "verifier_sha256=$verifierHash",
        "package_name=$([IO.Path]::GetFileName($package))", "package_sha256=$packageHash",
        "package_authenticode_valid=$allValid", "package_timestamped=$allTimestamped",
        'automated_package_smoke_satisfied=1',
        "operator_cases_required=$($requiredCases.Count)",
        "operator_cases_passed=$($requiredCases.Count)",
        "single_host_profile_satisfied=$(if ($Mode -eq 'interactive') { 1 } else { 0 })",
        'clean_client_matrix_satisfied=0', 'physical_matrix_satisfied=0',
        "diagnostic_seconds=$DiagnosticSeconds", "collections=$($fields['collections'])",
        "maximum_working_set_bytes=$maximumWorkingSet",
        "maximum_private_bytes=$maximumPrivateBytes", "maximum_handles=$maximumHandles"
    ) + $binaryLines
    Write-AtomicText (Join-Path $staging 'summary.ini') (($summaryLines -join "`n") + "`n")
    Write-AtomicText $campaignPath (
        "format=1`nstate=passed`nmode=$Mode`nprofile=$Profile`n" +
        "source_revision=$($SourceRevision.ToLowerInvariant())`nprocess_id=0`n$provenance")

    Remove-Item -LiteralPath $extractPath -Recurse -Force
    $manifestLines = @('format=1', 'algorithm=sha256')
    $manifestFiles = @(Get-ChildItem -LiteralPath $staging -Recurse -File |
        Where-Object { $_.Name -ne 'manifest.sha256.ini' } |
        Sort-Object { $_.FullName.Substring($staging.Length + 1) })
    $manifestLines += "file_count=$($manifestFiles.Count)"
    foreach ($file in $manifestFiles) {
        $relative = $file.FullName.Substring($staging.Length + 1).Replace('\', '/')
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $manifestLines += "$relative=$hash"
    }
    Write-AtomicText (Join-Path $staging 'manifest.sha256.ini') (($manifestLines -join "`n") + "`n")
    & $verifierScript -CampaignDirectory $staging -AllowStaging `
        -RequireInteractive:($Mode -eq 'interactive') `
        -RequireAuthenticode:$RequireAuthenticode.IsPresent | Out-Null
    [IO.Directory]::Move($staging, $output)
    Write-Host "Client qualification evidence generated: $output"
} catch {
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    Write-AtomicText $campaignPath (
        "format=1`nstate=failed`nmode=$Mode`nprofile=$Profile`n" +
        "source_revision=$($SourceRevision.ToLowerInvariant())`nprocess_id=0`n$provenance")
    throw
} finally {
    [Environment]::SetEnvironmentVariable('BLACKBOX_PRODUCT_SETTINGS_PATH', $oldProduct, 'Process')
    [Environment]::SetEnvironmentVariable('BLACKBOX_SETTINGS_PATH', $oldRecorder, 'Process')
}
