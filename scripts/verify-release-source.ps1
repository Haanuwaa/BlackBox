[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{40}$')]
    [string]$ExpectedSourceRevision,

    [string]$GitExecutable = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path -LiteralPath $SourceRoot -ErrorAction Stop).Path
$metadata = Join-Path $root '.git'
if (-not [IO.Directory]::Exists($metadata) -or
    ([IO.File]::GetAttributes($metadata) -band [IO.FileAttributes]::ReparsePoint)) {
    throw 'Release source must be a regular local Git worktree with directory metadata.'
}

if ([string]::IsNullOrWhiteSpace($GitExecutable)) {
    $command = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        $GitExecutable = $command.Source
    } else {
        foreach ($candidate in @(
            'C:\Program Files\Git\cmd\git.exe',
            'C:\Program Files\Git\bin\git.exe',
            'C:\Program Files (x86)\Git\cmd\git.exe')) {
            if ([IO.File]::Exists($candidate)) {
                $GitExecutable = $candidate
                break
            }
        }
    }
}
if ([string]::IsNullOrWhiteSpace($GitExecutable) -or
    -not [IO.File]::Exists($GitExecutable)) {
    throw 'Git executable was not found for release-source verification.'
}
$git = (Resolve-Path -LiteralPath $GitExecutable -ErrorAction Stop).Path

function Invoke-Git([string[]]$Arguments) {
    $output = @(& $git -C $root @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Git release-source query failed: $($output -join ' ')"
    }
    return ($output -join "`n").Trim()
}

$topLevel = Invoke-Git @('rev-parse', '--show-toplevel')
$expectedRoot = [IO.Path]::GetFullPath($root).TrimEnd('\', '/')
$actualRoot = [IO.Path]::GetFullPath($topLevel).TrimEnd('\', '/')
if (-not $actualRoot.Equals($expectedRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'SourceRoot is not the top level of the release Git worktree.'
}

$revision = (Invoke-Git @('rev-parse', '--verify', 'HEAD')).ToLowerInvariant()
if ($revision -notmatch '^[0-9a-f]{40}$' -or
    $revision -cne $ExpectedSourceRevision) {
    throw 'Release source HEAD does not match ExpectedSourceRevision.'
}

$status = Invoke-Git @('status', '--porcelain=v1', '--untracked-files=all', '--ignored=no')
if (-not [string]::IsNullOrEmpty($status)) {
    throw 'Release source has staged, unstaged, or untracked changes.'
}

$submodules = Invoke-Git @('submodule', 'status', '--recursive')
if ($submodules -match '(?m)^[+\-U]') {
    throw 'Release source has missing, modified, or conflicted submodules.'
}

Write-Output 'format=1'
Write-Output 'source_clean=1'
Write-Output "source_revision=$revision"
