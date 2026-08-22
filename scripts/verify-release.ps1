[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackagePath,
    [switch]$RequireAuthenticode,
    [string]$ExpectedVersion = '',
    [ValidatePattern('^(|local-uncommitted|[0-9a-f]{40})$')]
    [string]$ExpectedSourceRevision = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$package = (Resolve-Path -LiteralPath $PackagePath -ErrorAction Stop).Path
if (-not [IO.File]::Exists($package) -or
    [IO.Path]::GetExtension($package) -ine '.zip') {
    throw 'The release package must be an existing ZIP file.'
}
$checksumPath = "$package.sha256"
if (-not [IO.File]::Exists($checksumPath)) {
    throw "Missing checksum file: $checksumPath"
}

$checksumText = [IO.File]::ReadAllText($checksumPath)
$checksumPattern = '^(?<hash>[0-9A-Fa-f]{64})  (?<name>[^\r\n\\/]+)\r?\n?$'
$match = [regex]::Match($checksumText, $checksumPattern)
if (-not $match.Success -or
    $match.Groups['name'].Value -cne [IO.Path]::GetFileName($package)) {
    throw 'The checksum sidecar must contain exactly one SHA-256 and the package filename.'
}
$expected = $match.Groups['hash'].Value.ToUpperInvariant()
$actual = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash
if ($actual -ne $expected) {
    throw 'Package SHA-256 does not match the checksum file.'
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($package)
try {
    if ($archive.Entries.Count -eq 0 -or $archive.Entries.Count -gt 4096) {
        throw 'The package must contain between 1 and 4096 ZIP entries.'
    }
    $expectedRoot = [IO.Path]::GetFileNameWithoutExtension($package)
    $seen = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    [uint64]$totalUncompressed = 0
    foreach ($entry in $archive.Entries) {
        $name = $entry.FullName
        if ([string]::IsNullOrWhiteSpace($name) -or $name.Contains('\') -or
            $name.StartsWith('/') -or $name.Contains(':')) {
            throw "Unsafe ZIP entry name: $name"
        }
        $parts = $name.Split('/')
        $partCount = if ($name.EndsWith('/')) { $parts.Length - 1 } else { $parts.Length }
        if ($partCount -lt 1 -or $parts[0] -cne $expectedRoot) {
            throw 'Every package entry must be beneath the exact package-named root directory.'
        }
        for ($index = 0; $index -lt $partCount; ++$index) {
            if ([string]::IsNullOrWhiteSpace($parts[$index]) -or
                $parts[$index] -eq '.' -or $parts[$index] -eq '..') {
                throw "Unsafe ZIP entry path segment: $name"
            }
        }
        if (-not $seen.Add($name)) {
            throw "Duplicate case-insensitive ZIP entry: $name"
        }
        if ([uint64]$entry.Length -gt 512MB) {
            throw "ZIP entry exceeds the 512 MiB bound: $name"
        }
        $totalUncompressed += [uint64]$entry.Length
        if ($totalUncompressed -gt 1GB) {
            throw 'The uncompressed package exceeds the 1 GiB bound.'
        }
    }
} finally {
    $archive.Dispose()
}

$temporary = Join-Path ([IO.Path]::GetTempPath()) ("blackbox-verify-" + [guid]::NewGuid())
try {
    [IO.Compression.ZipFile]::ExtractToDirectory($package, $temporary)
    $root = Join-Path $temporary ([IO.Path]::GetFileNameWithoutExtension($package))
    if (-not [IO.Directory]::Exists($root)) {
        throw 'The verified package root was not extracted.'
    }
    $required = @(
        'blackbox.exe',
        'blackbox_dataset_tool.exe',
        'blackbox_dogfood_tool.exe',
        'docs\RELEASE_READINESS.md',
        'docs\USER_GUIDE.md'
    )
    foreach ($relative in $required) {
        $target = Join-Path $root $relative
        if (-not [IO.File]::Exists($target) -or
            ([IO.File]::GetAttributes($target) -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Package is missing a regular file: $relative"
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedVersion)) {
        if ($ExpectedVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
            throw 'ExpectedVersion must be an exact three-part semantic version.'
        }
        $descriptions = @{
            'blackbox.exe' = 'BlackBox Computer Flight Recorder'
            'blackbox_dataset_tool.exe' = 'BlackBox Incident Dataset Tool'
            'blackbox_dogfood_tool.exe' = 'BlackBox Dogfood Evaluation Tool'
        }
        $parts = @($ExpectedVersion.Split('.') | ForEach-Object { [int]$_ })
        foreach ($relative in $descriptions.Keys) {
            $version = [Diagnostics.FileVersionInfo]::GetVersionInfo(
                (Join-Path $root $relative))
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
                $version.ProductName -cne 'BlackBox Computer Flight Recorder' -or
                $version.FileDescription -cne $descriptions[$relative] -or
                $version.InternalName -cne
                    [IO.Path]::GetFileNameWithoutExtension($relative) -or
                $version.OriginalFilename -cne $relative) {
                throw "Package executable version identity mismatch: $relative"
            }
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedSourceRevision)) {
        foreach ($relative in @('blackbox.exe', 'blackbox_dataset_tool.exe',
                                'blackbox_dogfood_tool.exe')) {
            $version = [Diagnostics.FileVersionInfo]::GetVersionInfo(
                (Join-Path $root $relative))
            if ($version.Comments -cne "source_revision=$ExpectedSourceRevision") {
                throw "Package executable source revision mismatch: $relative"
            }
        }
    }
    if ($RequireAuthenticode) {
        foreach ($relative in @('blackbox.exe', 'blackbox_dataset_tool.exe',
                                'blackbox_dogfood_tool.exe')) {
            $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $root $relative)
            if ($signature.Status -ne 'Valid') {
                throw "Invalid Authenticode signature for ${relative}: $($signature.Status)"
            }
            if ($null -eq $signature.TimeStamperCertificate) {
                throw "Authenticode signature has no trusted timestamp for ${relative}."
            }
        }
    }
} finally {
    if ([IO.Directory]::Exists($temporary)) {
        Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Output "Verified $package ($actual)"
