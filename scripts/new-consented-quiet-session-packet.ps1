[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$DogfoodTool,
    [Parameter(Mandatory = $true)] [string]$BaseCorpusDirectory,
    [Parameter(Mandatory = $true)] [string]$OutputPacketDirectory,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]{1,64}$')] [string]$ProfileId,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]{1,64}$')] [string]$OsBuildBucket,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]{1,64}$')] [string]$CpuFamily,
    [Parameter(Mandatory = $true)] [ValidateRange(1, 4096)] [int]$LogicalProcessors,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]{1,64}$')] [string]$MemoryGibBucket,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]{1,64}$')] [string]$GpuFamily,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]{1,64}$')] [string]$PowerMode,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]{1,64}$')] [string]$SessionId,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]{1,64}$')] [string]$OperatorId,
    [Parameter(Mandatory = $true)]
    [ValidateSet('calibration', 'held_out')] [string]$Split,
    [Parameter(Mandatory = $true)]
    [ValidateRange(3600, 604800)] [int]$DurationSeconds,
    [Parameter(Mandatory = $true)]
    [ValidateSet('PARTICIPANT-CONSENT-CONFIRMED')] [string]$ConsentAttestation,
    [Parameter(Mandatory = $true)]
    [ValidateSet('QUIET-EXPOSURE-COMPLETED')] [string]$ExposureAttestation,
    [Parameter(Mandatory = $true)]
    [ValidateSet('NO-AUTOMATIC-CAPTURES')] [string]$NoCaptureAttestation
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Dogfood([string[]]$Arguments) {
    $output = @(& $script:ResolvedDogfoodTool @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "blackbox_dogfood_tool failed: $($output -join ' ')"
    }
}

function Append-Utf8([string]$Path, [string]$Line) {
    [IO.File]::AppendAllText($Path, $Line + "`n", [Text.UTF8Encoding]::new($false))
}

$toolItem = Get-Item -LiteralPath $DogfoodTool -Force
if ($toolItem.PSIsContainer -or
    (($toolItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
    throw 'DogfoodTool must be a regular, non-link file.'
}
$script:ResolvedDogfoodTool = $toolItem.FullName

$baseItem = Get-Item -LiteralPath $BaseCorpusDirectory -Force
if (-not $baseItem.PSIsContainer -or
    (($baseItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
    throw 'BaseCorpusDirectory must be a non-link directory.'
}

$finalPath = [IO.Path]::GetFullPath($OutputPacketDirectory)
$stagingPath = $finalPath + '.partial'
if (Test-Path -LiteralPath $finalPath) {
    throw 'OutputPacketDirectory already exists.'
}
if (Test-Path -LiteralPath $stagingPath) {
    throw 'The sibling partial packet directory already exists.'
}

Invoke-Dogfood @('init-session', $baseItem.FullName, $stagingPath)

Append-Utf8 (Join-Path $stagingPath 'hardware.tsv') (
    "$ProfileId`twindows`t$OsBuildBucket`t$CpuFamily`t$LogicalProcessors`t" +
    "$MemoryGibBucket`t$GpuFamily`t$PowerMode")
Append-Utf8 (Join-Path $stagingPath 'sessions.tsv') (
    "$SessionId`t$ProfileId`t$OperatorId`t$Split`tquiet`tquiet`t" +
    "$DurationSeconds`t0`t0`t1")

Invoke-Dogfood @('validate', $stagingPath)
[IO.Directory]::Move($stagingPath, $finalPath)
Invoke-Dogfood @('validate', $finalPath)

Write-Output 'format=1'
Write-Output 'packet_valid=1'
Write-Output 'prediction_free=1'
Write-Output 'kind=quiet'
Write-Output "split=$Split"
Write-Output "duration_seconds=$DurationSeconds"
Write-Output 'expected_incidents=0'
Write-Output 'automatic_captures=0'
Write-Output 'consent_attested=1'
