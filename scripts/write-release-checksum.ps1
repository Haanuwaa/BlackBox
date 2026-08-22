[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackagePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$resolved = (Resolve-Path -LiteralPath $PackagePath -ErrorAction Stop).Path
if (-not [IO.File]::Exists($resolved) -or [IO.Path]::GetExtension($resolved) -ine '.zip' -or
    ([IO.File]::GetAttributes($resolved) -band [IO.FileAttributes]::ReparsePoint)) {
    throw 'The release package must be a non-link ZIP file.'
}
$hash = Get-FileHash -LiteralPath $resolved -Algorithm SHA256
$output = "$resolved.sha256"
$temporary = "$output.tmp"
$contents = "$($hash.Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($resolved))`n"
[IO.File]::WriteAllText($temporary, $contents, [Text.Encoding]::ASCII)
if ([IO.File]::Exists($output)) {
    $backup = "$output.previous"
    [IO.File]::Replace($temporary, $output, $backup)
    [IO.File]::Delete($backup)
} else {
    [IO.File]::Move($temporary, $output)
}
Write-Output $output
