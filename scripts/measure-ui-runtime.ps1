[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ApplicationPath,
    [Parameter(Mandatory = $true)]
    [ValidateSet('Visible', 'Minimized', 'Hidden', 'Background')]
    [string]$Mode,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [ValidateRange(10, 3600)]
    [int]$DurationSeconds = 30,
    [ValidateRange(1, 300)]
    [int]$WarmupSeconds = 5
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$application = [IO.Path]::GetFullPath($ApplicationPath)
$output = [IO.Path]::GetFullPath($OutputPath)
if (-not [IO.Path]::IsPathFullyQualified($ApplicationPath) -or
    -not (Test-Path -LiteralPath $application -PathType Leaf)) {
    throw 'ApplicationPath must be an absolute existing executable.'
}
if (-not [IO.Path]::IsPathFullyQualified($OutputPath) -or
    [IO.Path]::GetExtension($output) -ne '.ini') {
    throw 'OutputPath must be an absolute .ini path.'
}
if ($WarmupSeconds -ge $DurationSeconds) {
    throw 'WarmupSeconds must be shorter than DurationSeconds.'
}

$parent = [IO.Path]::GetDirectoryName($output)
[IO.Directory]::CreateDirectory($parent) | Out-Null
$staging = "$output.partial"
$diagnostic = [IO.Path]::Combine($parent, ([IO.Path]::GetFileNameWithoutExtension($output) + '.diagnostic.ini'))
if ((Test-Path -LiteralPath $output) -or (Test-Path -LiteralPath $staging) -or
    (Test-Path -LiteralPath $diagnostic)) {
    throw 'Output, staging, or diagnostic path already exists.'
}

if ($Mode -in @('Minimized', 'Hidden')) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class BlackBoxWindowMeasurement {
    [DllImport("user32.dll")]
    public static extern bool ShowWindowAsync(IntPtr window, int command);
}
'@
}

$durationArgument = if ($Mode -eq 'Background') {
    "--background-diagnostic-seconds=$DurationSeconds"
} else {
    "--visible-diagnostic-seconds=$DurationSeconds"
}
$arguments = @($durationArgument, ('--diagnostic-report="{0}"' -f $diagnostic))
$process = Start-Process -FilePath $application -ArgumentList $arguments -PassThru
$started = [Diagnostics.Stopwatch]::StartNew()
$samples = [Collections.Generic.List[double]]::new()
$workingSets = [Collections.Generic.List[long]]::new()
$privateBytes = [Collections.Generic.List[long]]::new()
$previousCpu = [TimeSpan]::Zero
$previousElapsed = [TimeSpan]::Zero
$visibilityAttempted = $false

try {
    while (-not $process.HasExited) {
        Start-Sleep -Milliseconds 250
        $process.Refresh()
        if ($Mode -in @('Minimized', 'Hidden') -and -not $visibilityAttempted -and
            $started.Elapsed.TotalSeconds -ge 1.0) {
            $visibilityAttempted = $true
            $windowCommand = if ($Mode -eq 'Minimized') { 6 } else { 0 }
            if ($process.MainWindowHandle -eq [IntPtr]::Zero -or
                -not [BlackBoxWindowMeasurement]::ShowWindowAsync(
                    $process.MainWindowHandle, $windowCommand)) {
                throw "The BlackBox window could not enter $($Mode.ToLowerInvariant()) mode."
            }
        }
        $elapsed = $started.Elapsed
        $cpu = $process.TotalProcessorTime
        if ($elapsed.TotalSeconds -ge $WarmupSeconds -and
            $elapsed -gt $previousElapsed) {
            $capacity = ($cpu - $previousCpu).TotalSeconds /
                ($elapsed - $previousElapsed).TotalSeconds /
                [Environment]::ProcessorCount * 100.0
            $samples.Add($capacity)
            $workingSets.Add($process.WorkingSet64)
            $privateBytes.Add($process.PrivateMemorySize64)
        }
        $previousCpu = $cpu
        $previousElapsed = $elapsed
    }
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        throw "BlackBox exited with code $($process.ExitCode)."
    }
    if ($samples.Count -eq 0 -or -not (Test-Path -LiteralPath $diagnostic -PathType Leaf)) {
        throw 'The run produced no post-warm-up samples or diagnostic report.'
    }

    $averageCpu = ($samples | Measure-Object -Average).Average
    $maximumCpu = ($samples | Measure-Object -Maximum).Maximum
    $averageWorkingSet = ($workingSets | Measure-Object -Average).Average
    $maximumWorkingSet = ($workingSets | Measure-Object -Maximum).Maximum
    $averagePrivate = ($privateBytes | Measure-Object -Average).Average
    $maximumPrivate = ($privateBytes | Measure-Object -Maximum).Maximum
    $lines = @(
        'format=1',
        'artifact=blackbox-ui-runtime-measurement',
        "application_sha256=$((Get-FileHash -LiteralPath $application -Algorithm SHA256).Hash.ToLowerInvariant())",
        "operating_system=$([Environment]::OSVersion.VersionString)",
        "logical_processors=$([Environment]::ProcessorCount)",
        "mode=$($Mode.ToLowerInvariant())",
        "requested_seconds=$DurationSeconds",
        ('observed_seconds={0:R}' -f [double]$started.Elapsed.TotalSeconds),
        "warmup_seconds=$WarmupSeconds",
        "samples=$($samples.Count)",
        ('average_total_machine_cpu_percent={0:R}' -f [double]$averageCpu),
        ('maximum_total_machine_cpu_percent={0:R}' -f [double]$maximumCpu),
        ('average_working_set_bytes={0}' -f [long]$averageWorkingSet),
        "maximum_working_set_bytes=$maximumWorkingSet",
        ('average_private_bytes={0}' -f [long]$averagePrivate),
        "maximum_private_bytes=$maximumPrivate"
    )
    [IO.File]::WriteAllLines($staging, $lines, [Text.UTF8Encoding]::new($false))
    [IO.File]::Move($staging, $output)
} finally {
    if (-not $process.HasExited) {
        $process.Kill($true)
        $process.WaitForExit()
    }
}
