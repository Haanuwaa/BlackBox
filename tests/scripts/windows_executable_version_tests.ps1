[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')]
    [string]$ExpectedVersion,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^(local-uncommitted|[0-9a-f]{40})$')]
    [string]$ExpectedSourceRevision,

    [Parameter(Mandatory = $true)]
    [string[]]$BinaryPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$descriptions = @{
    'blackbox.exe' = 'BlackBox Computer Flight Recorder'
    'blackbox_dataset_tool.exe' = 'BlackBox Incident Dataset Tool'
    'blackbox_dogfood_tool.exe' = 'BlackBox Dogfood Evaluation Tool'
    'blackbox_dogfood_capture.exe' = 'BlackBox Dogfood Capture Tool'
    'blackbox_soak_archive_fault.exe' = 'BlackBox Soak Archive Fault Probe'
}
$parts = @($ExpectedVersion.Split('.') | ForEach-Object { [int]$_ })

foreach ($path in $BinaryPath) {
    if (-not [IO.File]::Exists($path)) {
        throw "Versioned Windows executable is missing: $path"
    }
    $leaf = [IO.Path]::GetFileName($path)
    if (-not $descriptions.ContainsKey($leaf)) {
        throw "Unexpected executable in version-resource contract: $leaf"
    }
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($path)
    if ($version.FileVersion -cne $ExpectedVersion -or
        $version.ProductVersion -cne $ExpectedVersion) {
        throw "$leaf does not expose exact semantic version $ExpectedVersion."
    }
    if ($version.FileMajorPart -ne $parts[0] -or
        $version.FileMinorPart -ne $parts[1] -or
        $version.FileBuildPart -ne $parts[2] -or
        $version.FilePrivatePart -ne 0 -or
        $version.ProductMajorPart -ne $parts[0] -or
        $version.ProductMinorPart -ne $parts[1] -or
        $version.ProductBuildPart -ne $parts[2] -or
        $version.ProductPrivatePart -ne 0) {
        throw "$leaf has inconsistent fixed numeric version fields."
    }
    if ($version.CompanyName -cne 'BlackBox' -or
        $version.Comments -cne "source_revision=$ExpectedSourceRevision" -or
        $version.ProductName -cne 'BlackBox Computer Flight Recorder' -or
        $version.FileDescription -cne $descriptions[$leaf] -or
        $version.InternalName -cne [IO.Path]::GetFileNameWithoutExtension($leaf) -or
        $version.OriginalFilename -cne $leaf) {
        throw "$leaf has incomplete or inconsistent Windows identity metadata."
    }
}

Write-Output "Verified exact Windows version resources for $($BinaryPath.Count) executables."
