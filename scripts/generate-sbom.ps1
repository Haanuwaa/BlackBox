[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$StatusPath,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [string]$SourceRoot = (Split-Path -Parent $PSScriptRoot)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path -LiteralPath $SourceRoot).Path
& (Join-Path $PSScriptRoot 'verify-dependency-policy.ps1') -SourceRoot $root

$resolvedStatus = (Resolve-Path -LiteralPath $StatusPath).Path
$statusInfo = Get-Item -LiteralPath $resolvedStatus
if ($statusInfo.Length -le 0 -or $statusInfo.Length -gt 4MB) {
    throw 'vcpkg status file is empty or exceeds the 4 MiB SBOM input bound'
}
$manifest = Get-Content -LiteralPath (Join-Path $root 'vcpkg.json') -Raw |
    ConvertFrom-Json
$directNames = @{}
foreach ($dependency in @($manifest.dependencies)) {
    $name = if ($dependency -is [string]) { [string]$dependency } else { [string]$dependency.name }
    $directNames[$name] = $true
}

$records = @()
$paragraphs = [regex]::Split(
    (Get-Content -LiteralPath $resolvedStatus -Raw).Trim(), '\r?\n\r?\n+')
foreach ($paragraph in $paragraphs) {
    $fields = @{}
    foreach ($line in [regex]::Split($paragraph, '\r?\n')) {
        $separator = $line.IndexOf(':')
        if ($separator -le 0) { continue }
        $fields[$line.Substring(0, $separator)] = $line.Substring($separator + 1).Trim()
    }
    if (-not $fields.ContainsKey('Package') -or
        -not $fields.ContainsKey('Version') -or
        -not $fields.ContainsKey('Architecture') -or
        -not $fields.ContainsKey('Status') -or
        $fields['Status'] -ne 'install ok installed') {
        continue
    }
    foreach ($fieldName in @('Package', 'Version', 'Architecture')) {
        if ($fields[$fieldName] -notmatch '^[A-Za-z0-9._+#-]{1,128}$') {
            throw "unsafe vcpkg status field '$fieldName'"
        }
    }
    $records += [pscustomobject]@{
        Name = [string]$fields['Package']
        Version = [string]$fields['Version']
        Architecture = [string]$fields['Architecture']
        Depends = if ($fields.ContainsKey('Depends')) { [string]$fields['Depends'] } else { '' }
    }
}
if ($records.Count -eq 0 -or $records.Count -gt 1024) {
    throw 'installed dependency count is empty or exceeds the 1024-component bound'
}
$records = @($records | Sort-Object Name, Architecture)
$references = @{}
$components = @()
foreach ($record in $records) {
    $reference = "pkg:generic/vcpkg/$($record.Name)@$($record.Version)?triplet=$($record.Architecture)"
    $references[$record.Name] = $reference
    $components += [ordered]@{
        type = 'library'
        name = $record.Name
        version = $record.Version
        'bom-ref' = $reference
        purl = $reference
        properties = @(
            [ordered]@{ name = 'blackbox:vcpkg-triplet'; value = $record.Architecture },
            [ordered]@{ name = 'blackbox:direct-dependency'; value = if ($directNames.ContainsKey($record.Name)) { 'true' } else { 'false' } }
        )
    }
}

$relationships = @()
foreach ($record in $records) {
    $dependencies = @()
    if ($record.Depends) {
        foreach ($raw in $record.Depends.Split(',')) {
            $name = ($raw.Trim() -replace '\[.*$', '' -replace ':.*$', '' -replace '\s+.*$', '')
            if ($references.ContainsKey($name)) { $dependencies += $references[$name] }
        }
    }
    $relationships += [ordered]@{
        ref = $references[$record.Name]
        dependsOn = @($dependencies | Sort-Object -Unique)
    }
}

$bom = [ordered]@{
    bomFormat = 'CycloneDX'
    specVersion = '1.5'
    version = 1
    metadata = [ordered]@{
        component = [ordered]@{
            type = 'application'
            name = 'BlackBox'
            version = [string]$manifest.'version-semver'
        }
        properties = @(
            [ordered]@{ name = 'blackbox:vcpkg-builtin-baseline'; value = [string]$manifest.'builtin-baseline' }
        )
    }
    components = $components
    dependencies = $relationships
}

$destination = [IO.Path]::GetFullPath($OutputPath)
$parent = Split-Path -Parent $destination
if ($parent) { [IO.Directory]::CreateDirectory($parent) | Out-Null }
$json = $bom | ConvertTo-Json -Depth 12
[IO.File]::WriteAllText($destination, $json + [Environment]::NewLine,
                        [Text.UTF8Encoding]::new($false))
$verified = Get-Content -LiteralPath $destination -Raw | ConvertFrom-Json
if ($verified.bomFormat -ne 'CycloneDX' -or
    @($verified.components).Count -ne $records.Count) {
    throw 'generated SBOM failed read-back validation'
}
Write-Output "CycloneDX SBOM written: path=$destination components=$($records.Count) direct=$($directNames.Count)"
