[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ApplicationPath,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class BlackBoxAutomationSmoke {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr window, uint message, UIntPtr wparam, IntPtr lparam);
}
'@
$sqlite = (Join-Path (Split-Path -Parent $ApplicationPath) 'sqlite3.dll').Replace('"', '""')
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class BlackBoxArchiveSmoke {
    [DllImport(@"$sqlite", CallingConvention=CallingConvention.Cdecl)]
    static extern int sqlite3_open_v2([MarshalAs(UnmanagedType.LPUTF8Str)] string path, out IntPtr db, int flags, IntPtr vfs);
    [DllImport(@"$sqlite", CallingConvention=CallingConvention.Cdecl)]
    static extern int sqlite3_prepare_v2(IntPtr db, [MarshalAs(UnmanagedType.LPUTF8Str)] string sql, int bytes, out IntPtr statement, IntPtr tail);
    [DllImport(@"$sqlite", CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_step(IntPtr statement);
    [DllImport(@"$sqlite", CallingConvention=CallingConvention.Cdecl)] static extern long sqlite3_column_int64(IntPtr statement, int column);
    [DllImport(@"$sqlite", CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_finalize(IntPtr statement);
    [DllImport(@"$sqlite", CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_close(IntPtr db);
    public static long Count(string path) {
        IntPtr db = IntPtr.Zero, statement = IntPtr.Zero;
        try {
            if (sqlite3_open_v2(path, out db, 1, IntPtr.Zero) != 0 ||
                sqlite3_prepare_v2(db, "SELECT count(*) FROM incidents", -1, out statement, IntPtr.Zero) != 0 ||
                sqlite3_step(statement) != 100) throw new Exception("Cannot inspect the isolated archive.");
            return sqlite3_column_int64(statement, 0);
        } finally { if (statement != IntPtr.Zero) sqlite3_finalize(statement); if (db != IntPtr.Zero) sqlite3_close(db); }
    }
}
"@
$root = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $root) { throw 'Use a new isolated output directory.' }
[IO.Directory]::CreateDirectory($root) | Out-Null
$product = Join-Path $root 'product.ini'
$recorder = Join-Path $root 'recorder.ini'
$archive = (Join-Path $root 'archive.sqlite3').Replace('\', '/')
[IO.File]::WriteAllText($product, @"
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
record_power_and_device_events=0
record_audio_device_events=0
record_system_event_evidence=0
archive_path=$archive
archive_maximum_bytes=1073741824
onboarding_completed=1
"@ + "`n", [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText($recorder, @"
format=1
sample_interval_ms=1000
history_duration_ms=300000
late_tolerance_ms=250
metadata_interval_ms=30000
incident_pre_window_ms=1000
incident_post_window_ms=5000
resume_gap_threshold_ms=5000
collect_process_paths=0
"@ + "`n", [Text.UTF8Encoding]::new($false))
$previousProduct = $env:BLACKBOX_PRODUCT_SETTINGS_PATH
$previousRecorder = $env:BLACKBOX_SETTINGS_PATH
$process = $null
try {
    $env:BLACKBOX_PRODUCT_SETTINGS_PATH = $product
    $env:BLACKBOX_SETTINGS_PATH = $recorder
    $process = Start-Process -FilePath $ApplicationPath -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $root "stderr.log")
    $null = $process.Handle # Retain a handle so Windows PowerShell can report the exit code.
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 100
        $process.Refresh()
        if ($process.HasExited) { throw 'Application exited during startup.' }
    } while ($process.MainWindowHandle -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline)
    if ($process.MainWindowHandle -eq [IntPtr]::Zero) { throw 'No application window.' }
    $element = [Windows.Automation.AutomationElement]::FromHandle($process.MainWindowHandle)
    function Find-Control([string]$Name) {
        $limit = [DateTime]::UtcNow.AddSeconds(10)
        do {
            $condition = [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::NameProperty, $Name)
            $found = $element.FindFirst([Windows.Automation.TreeScope]::Descendants, $condition)
            if ($null -ne $found) { return $found }
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $limit)
        $element.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition) | ForEach-Object { Write-Output $_.Current.Name } | Out-File (Join-Path $root "controls.txt")
        throw "Missing accessible control: $Name"
    }
    function Invoke-Control([string]$Name) {
        $control = Find-Control $Name
        $pattern = $control.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern)
        $pattern.Invoke()
        Start-Sleep -Milliseconds 150
    }
    Invoke-Control 'Capture what just happened   Ctrl+Enter'
    Invoke-Control 'Settings'
    $toggle = (Find-Control 'Show quiet desktop notifications').GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern)
    $toggle.Toggle()
    Start-Sleep -Milliseconds 200
    if ($toggle.Current.ToggleState -ne [Windows.Automation.ToggleState]::On) { throw 'Toggle action did not reach the widget.' }
    # Exercise the same normal-exit command consumed by the native shell. Verify
    # ownership before messaging, so another running BlackBox is never affected.
    $shell = [BlackBoxAutomationSmoke]::FindWindow('BlackBox.BackgroundShell.Window.V1', $null)
    [uint32]$owner = 0
    [void][BlackBoxAutomationSmoke]::GetWindowThreadProcessId($shell, [ref]$owner)
    if ($owner -ne $process.Id) { throw 'Isolated process does not own the shell window.' }
    [void][BlackBoxAutomationSmoke]::PostMessage($shell, 0x8045, [UIntPtr]::new([uint32]6), [IntPtr]::Zero)
    [void](Find-Control 'Keep working')
    Invoke-Control 'Save and exit'
    if (-not $process.WaitForExit(15000)) { throw 'Save-and-exit did not finish.' }
    if ($process.ExitCode -ne 0) { throw "Application failed with exit code $($process.ExitCode)." }
    if ([IO.File]::ReadAllText($product) -notmatch '(?m)^notifications=1\r?$') { throw 'Exit did not persist the settings draft.' }
    if ([BlackBoxArchiveSmoke]::Count($archive) -ne 1) { throw 'Exit lost the accepted capture.' }
    'UI Automation native smoke passed: navigation, toggle, draft confirmation, capture drain and save-and-exit.'
} finally {
    $env:BLACKBOX_PRODUCT_SETTINGS_PATH = $previousProduct
    $env:BLACKBOX_SETTINGS_PATH = $previousRecorder
    if ($null -ne $process -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
}
