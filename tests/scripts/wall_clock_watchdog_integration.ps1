[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ApplicationPath,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

# Opt-in Windows integration test. Suspends only the isolated application copy
# launched by this test, then expects the runner to kill it at its deadline.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $root) { throw 'Use a new integration output directory.' }
[IO.Directory]::CreateDirectory($root) | Out-Null
$runner = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../scripts/run-wall-clock-soak.ps1'))
$wrong = Join-Path $root 'wrong-revision'
$rejected = $false
try {
    & $runner -ApplicationPath $ApplicationPath -OutputDirectory $wrong `
        -DurationSeconds 12 -CaptureIntervalSeconds 3 -CheckpointSeconds 2 `
        -SourceRevision ('0' * 40)
} catch {
    if ($_.Exception.Message -notmatch 'identity|revision') { throw }
    $rejected = $true
}
if (-not $rejected -or (Test-Path -LiteralPath $wrong) -or
    (Test-Path -LiteralPath "$wrong.partial")) {
    throw 'Wrong revision was not rejected before staging and launch.'
}

$destination = Join-Path $root 'hung-run'
$partial = "$destination.partial"
$ownedPath = Join-Path $partial 'runtime/blackbox.exe'
$shell = (Get-Process -Id $PID).Path
$arguments = @('-NoProfile', '-File', ('"{0}"' -f $runner),
    '-ApplicationPath', ('"{0}"' -f [IO.Path]::GetFullPath($ApplicationPath)),
    '-OutputDirectory', ('"{0}"' -f $destination),
    '-DurationSeconds', '12', '-CaptureIntervalSeconds', '3', '-CheckpointSeconds', '2',
    '-SourceRevision', 'local-uncommitted')
$hostProcess = $null
$owned = $null
try {
    $hostProcess = Start-Process -FilePath $shell -ArgumentList $arguments -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $root 'runner.stdout.log') `
        -RedirectStandardError (Join-Path $root 'runner.stderr.log') -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(35)
    do {
        if ($hostProcess.HasExited) { throw 'Runner exited before the hang injection.' }
        $progress = Join-Path $partial 'app-progress.ini'
        if ([IO.File]::Exists($progress)) {
            $ownedCandidates = @(Get-Process -Name blackbox -ErrorAction SilentlyContinue |
                Where-Object { $_.Path -eq $ownedPath })
            if ($ownedCandidates.Count -eq 1) { $owned = $ownedCandidates[0]; break }
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($null -eq $owned) { throw 'Isolated application did not produce a heartbeat.' }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class BlackBoxWatchdogInjection {
    [DllImport("kernel32.dll", SetLastError=true)] private static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);
    [DllImport("ntdll.dll")] private static extern int NtSuspendProcess(IntPtr handle);
    public static void Suspend(uint pid) {
        IntPtr handle = OpenProcess(0x0800, false, pid);
        if (handle == IntPtr.Zero) throw new InvalidOperationException("Cannot open test process for suspension");
        try { if (NtSuspendProcess(handle) != 0) throw new InvalidOperationException("Cannot suspend test process"); }
        finally { CloseHandle(handle); }
    }
}
'@
    $owned.Refresh()
    if ($owned.HasExited -or $owned.Path -ne $ownedPath) { throw 'Test process identity changed.' }
    [BlackBoxWatchdogInjection]::Suspend([uint32]$owned.Id)
    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    while (-not $hostProcess.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 500
    }
    if (-not $hostProcess.HasExited) { throw 'Runner itself exceeded the integration timeout.' }
    $hostProcess.WaitForExit()
    if ($hostProcess.ExitCode -eq 0) { throw 'Hung application was incorrectly accepted.' }
    $failure = [IO.File]::ReadAllText((Join-Path $partial 'failure.txt'))
    if ($failure -notmatch 'runtime plus 60-second drain watchdog') { throw $failure }
    if ([IO.File]::ReadAllText((Join-Path $partial 'campaign.ini')) -notmatch 'state=failed') {
        throw 'Failed campaign state was not retained.'
    }
    $owned.Refresh()
    if (-not $owned.HasExited -or (Test-Path -LiteralPath $destination)) {
        throw 'Hung process survived or incomplete evidence was published.'
    }
    Write-Output 'PASS: wrong revision rejected before launch; hung process killed; failed partial evidence retained.'
} finally {
    if ($null -ne $owned -and -not $owned.HasExited -and $owned.Path -eq $ownedPath) {
        Stop-Process -Id $owned.Id -Force -ErrorAction SilentlyContinue
    }
    if ($null -ne $hostProcess -and -not $hostProcess.HasExited) {
        Stop-Process -Id $hostProcess.Id -Force -ErrorAction SilentlyContinue
    }
}
