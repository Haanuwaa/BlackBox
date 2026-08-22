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
foreach ($workflow in $workflowFiles) {
    $text = Get-Content -LiteralPath $workflow.FullName -Raw
    foreach ($match in [regex]::Matches($text, 'uses:\s*([^\s@]+)@([^\s#]+)')) {
        ++$actionCount
        $reference = $match.Groups[2].Value
        if ($reference -notmatch '^[0-9a-f]{40}$') {
            Fail "$($workflow.Name) uses a mutable action reference '$reference'"
        }
    }
    foreach ($match in [regex]::Matches(
        $text, 'fetch\s+--depth\s+1\s+origin\s+([0-9a-f]{40})')) {
        ++$bootstrapCount
        if ($match.Groups[1].Value -ne $baseline) {
            Fail "$($workflow.Name) bootstraps a vcpkg commit different from the manifest"
        }
    }
}
if ($actionCount -eq 0) {
    Fail 'workflow action pin inspection found no actions'
}
if ($bootstrapCount -eq 0) {
    Fail 'workflow vcpkg bootstrap inspection found no immutable commit'
}

Write-Output "Dependency policy verified: version=$($manifest.'version-semver') baseline=$baseline direct=$($expected.Count) pinned_actions=$actionCount bootstraps=$bootstrapCount"
