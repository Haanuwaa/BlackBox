[CmdletBinding()]
param(
    [string]$SourceRoot = (Split-Path -Parent $PSScriptRoot)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    throw "Dependency policy failed: $Message"
}

$root = (Resolve-Path -LiteralPath $SourceRoot).Path
$manifestPath = Join-Path $root 'vcpkg.json'
$cmakePath = Join-Path $root 'CMakeLists.txt'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    Fail 'vcpkg.json is missing'
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.'$schema' -ne
    'https://raw.githubusercontent.com/microsoft/vcpkg-tool/main/docs/vcpkg.schema.json') {
    Fail 'vcpkg manifest schema must use the official HTTPS schema'
}
if ($manifest.'builtin-baseline' -notmatch '^[0-9a-f]{40}$') {
    Fail 'builtin-baseline must be one lowercase immutable Git object ID'
}
$baseline = [string]$manifest.'builtin-baseline'

$cmake = Get-Content -LiteralPath $cmakePath -Raw
$projectMatch = [regex]::Match(
    $cmake, 'project\s*\(\s*BlackBox\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)',
    [Text.RegularExpressions.RegexOptions]::IgnoreCase)
if (-not $projectMatch.Success) {
    Fail 'CMake project version cannot be identified'
}
if ([string]$manifest.'version-semver' -ne $projectMatch.Groups[1].Value) {
    Fail 'vcpkg version-semver must equal the CMake project version'
}

$expected = @('catch2', 'imgui', 'implot', 'sdl3', 'sqlite3')
$observed = @()
foreach ($dependency in @($manifest.dependencies)) {
    if ($dependency -is [string]) {
        $observed += [string]$dependency
        continue
    }
    $properties = @($dependency.PSObject.Properties.Name | Sort-Object)
    if ((Compare-Object $properties @('features', 'name') -SyncWindow 0)) {
        Fail "dependency '$($dependency.name)' contains an unapproved field"
    }
    if ($dependency.name -ne 'imgui') {
        Fail "only imgui may declare manifest features; found '$($dependency.name)'"
    }
    $features = @($dependency.features | Sort-Object)
    if ((Compare-Object $features @('sdl3-binding', 'sdl3-renderer-binding') -SyncWindow 0)) {
        Fail 'imgui feature allowlist changed'
    }
    $observed += [string]$dependency.name
}
$observed = @($observed | Sort-Object)
if ((Compare-Object $observed $expected -SyncWindow 0)) {
    Fail 'direct dependency allowlist changed'
}
if ($manifest.PSObject.Properties.Name -contains 'overrides') {
    Fail 'dependency overrides are prohibited'
}

$workflowRoot = Join-Path $root '.github/workflows'
$workflowFiles = @(Get-ChildItem -LiteralPath $workflowRoot -File -Filter '*.yml')
if ($workflowFiles.Count -eq 0) {
    Fail 'no workflow files were found'
}
$actionCount = 0
$bootstrapCount = 0
$actionPins = @{}
$bootstrap = Get-Content -LiteralPath (Join-Path $root 'scripts/bootstrap-vcpkg.cmake') -Raw
if ($bootstrap -notmatch 'string\(JSON baseline GET "\$\{manifest\}" builtin-baseline\)' -or
    $bootstrap -notmatch 'fetch --depth 1 origin "\$\{baseline\}"' -or
    $bootstrap -notmatch 'https://github\.com/microsoft/vcpkg\.git') {
    Fail 'shared bootstrap must fetch the immutable baseline read from vcpkg.json'
}
foreach ($workflow in $workflowFiles) {
    $text = Get-Content -LiteralPath $workflow.FullName -Raw
    foreach ($match in [regex]::Matches($text, 'uses:\s*([^\s@]+)@([^\s#]+)')) {
        ++$actionCount
        $actionName = $match.Groups[1].Value
        $reference = $match.Groups[2].Value
        if ($reference -notmatch '^[0-9a-f]{40}$') {
            Fail "$($workflow.Name) uses a mutable action reference '$reference'"
        }
        $repository = ($actionName -split '/')[0..1] -join '/'
        if ($actionPins.ContainsKey($repository) -and $actionPins[$repository] -ne $reference) {
            Fail "$repository must use one consistent commit across workflows and sub-actions"
        }
        $actionPins[$repository] = $reference
    }
    if ($text -match 'fetch\s+--depth\s+1\s+origin') {
        Fail "$($workflow.Name) duplicates vcpkg bootstrap logic instead of using the shared helper"
    }
    $calls = [regex]::Matches($text, 'cmake -P scripts/bootstrap-vcpkg\.cmake').Count
    $roots = [regex]::Matches($text, '(?m)^\s+VCPKG_ROOT:').Count
    if ($calls -ne $roots) {
        Fail "$($workflow.Name) must bootstrap each isolated vcpkg job through the shared helper"
    }
    $bootstrapCount += $calls
}
if ($actionCount -eq 0) {
    Fail 'workflow action pin inspection found no actions'
}
if ($bootstrapCount -eq 0) {
    Fail 'workflow vcpkg bootstrap inspection found no shared bootstrap calls'
}

Write-Output "Dependency policy verified: version=$($manifest.'version-semver') baseline=$baseline direct=$($expected.Count) pinned_actions=$actionCount bootstraps=$bootstrapCount"
