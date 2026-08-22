[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9A-Fa-f]{40}$')]
    [string]$CertificateThumbprint,
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')]
    [string]$ExpectedVersion = '1.0.0',
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{40}$')]
    [string]$ExpectedSourceRevision,
    [string]$SourceRoot = (Split-Path -Parent $PSScriptRoot)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$build = (Resolve-Path -LiteralPath $BuildDirectory -ErrorAction Stop).Path
& (Join-Path $PSScriptRoot 'verify-release-source.ps1') `
    -SourceRoot $SourceRoot -ExpectedSourceRevision $ExpectedSourceRevision | Out-Null
& (Join-Path $PSScriptRoot 'verify-release-build-binaries.ps1') `
    -BinaryDirectory (Join-Path $build 'src\Release') `
    -ExpectedVersion $ExpectedVersion `
    -ExpectedSourceRevision $ExpectedSourceRevision | Out-Null
$targets = @(
    (Join-Path $build 'src\Release\blackbox.exe'),
    (Join-Path $build 'src\Release\blackbox_dataset_tool.exe'),
    (Join-Path $build 'src\Release\blackbox_dogfood_tool.exe')
)
$signtool = Get-Command signtool.exe -ErrorAction SilentlyContinue
if ($null -eq $signtool) {
    $kits = Get-ChildItem -LiteralPath 'C:\Program Files (x86)\Windows Kits\10\bin' `
        -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object FullName -Match '\\x64\\signtool\.exe$' |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if ($null -eq $kits) { throw 'signtool.exe was not found' }
    $signtoolPath = $kits.FullName
} else {
    $signtoolPath = $signtool.Source
}

foreach ($target in $targets) {
    & $signtoolPath sign /sha1 $CertificateThumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 $target
    if ($LASTEXITCODE -ne 0) { throw "Authenticode signing failed: $target" }
    & $signtoolPath verify /pa /all $target
    if ($LASTEXITCODE -ne 0) { throw "Authenticode verification failed: $target" }
    $signature = Get-AuthenticodeSignature -LiteralPath $target
    if ($signature.Status -ne 'Valid' -or $null -eq $signature.TimeStamperCertificate) {
        throw "The signed binary is not valid and trusted-timestamped: $target"
    }
}
