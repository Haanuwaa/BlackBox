[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BinaryDirectory,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')]
    [string]$ExpectedVersion,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^(local-uncommitted|[0-9a-f]{40})$')]
    [string]$ExpectedSourceRevision
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$releaseDirectory = (Resolve-Path -LiteralPath $BinaryDirectory -ErrorAction Stop).Path
$descriptions = [ordered]@{
    'blackbox.exe' = 'BlackBox Computer Flight Recorder'
    'blackbox_dataset_tool.exe' = 'BlackBox Incident Dataset Tool'
    'blackbox_dogfood_tool.exe' = 'BlackBox Dogfood Evaluation Tool'
}
$parts = @($ExpectedVersion.Split('.') | ForEach-Object { [int]$_ })

foreach ($name in $descriptions.Keys) {
    $path = Join-Path $releaseDirectory $name
    if (-not [IO.File]::Exists($path)) {
        throw "Missing release binary: $path"
    }
    $item = Get-Item -LiteralPath $path -Force
    if ($item.PSIsContainer -or
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Release binary must be a regular non-link file: $path"
    }
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($path)
    if ($version.FileVersion -cne $ExpectedVersion -or
        $version.ProductVersion -cne $ExpectedVersion -or
        $version.FileMajorPart -ne $parts[0] -or
        $version.FileMinorPart -ne $parts[1] -or
        $version.FileBuildPart -ne $parts[2] -or
        $version.FilePrivatePart -ne 0 -or
        $version.ProductMajorPart -ne $parts[0] -or
        $version.ProductMinorPart -ne $parts[1] -or
        $version.ProductBuildPart -ne $parts[2] -or
        $version.ProductPrivatePart -ne 0 -or
        $version.CompanyName -cne 'BlackBox' -or
        $version.Comments -cne "source_revision=$ExpectedSourceRevision" -or
        $version.ProductName -cne 'BlackBox Computer Flight Recorder' -or
        $version.FileDescription -cne $descriptions[$name] -or
        $version.InternalName -cne [IO.Path]::GetFileNameWithoutExtension($name) -or
        $version.OriginalFilename -cne $name) {
        throw "Release binary version identity mismatch: $name"
    }
}

Write-Output "Verified release binary identity $ExpectedVersion at $ExpectedSourceRevision ($($descriptions.Count) files)."
