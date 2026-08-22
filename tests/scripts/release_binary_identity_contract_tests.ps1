[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$SourceRoot,
    [Parameter(Mandatory = $true)] [string]$BinaryDirectory,
    [Parameter(Mandatory = $true)] [string]$ExpectedVersion,
    [Parameter(Mandatory = $true)] [string]$ExpectedSourceRevision
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Expect-Failure([scriptblock]$Action, [string]$Name) {
    $failed = $false
    try { & $Action | Out-Null } catch { $failed = $true }
    if (-not $failed) { throw "Expected rejection: $Name" }
    Write-Output "Expected rejection: $Name"
}

$verifier = Join-Path $SourceRoot 'scripts\verify-release-build-binaries.ps1'
$sourceVerifier = Join-Path $SourceRoot 'scripts\verify-release-source.ps1'
$packageVerifier = Join-Path $SourceRoot 'scripts\verify-release.ps1'
$signer = Join-Path $SourceRoot 'scripts\sign-release.ps1'
foreach ($script in @($verifier, $sourceVerifier, $packageVerifier, $signer)) {
    $tokens = $null
    $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile(
        $script, [ref]$tokens, [ref]$errors)
    if ($errors.Count -ne 0) { throw "PowerShell parser rejected $script" }
}

$signerText = [IO.File]::ReadAllText($signer)
$sourcePreflight = $signerText.IndexOf("'verify-release-source.ps1'",
    [StringComparison]::Ordinal)
$preflight = $signerText.IndexOf("'verify-release-build-binaries.ps1'",
    [StringComparison]::Ordinal)
$sign = $signerText.IndexOf('& $signtoolPath sign', [StringComparison]::Ordinal)
if ($sourcePreflight -lt 0 -or $preflight -lt 0 -or $sign -lt 0 -or
    $sourcePreflight -ge $preflight -or $preflight -ge $sign -or
    -not $signerText.Contains("[string]`$ExpectedVersion = '1.0.0'") -or
    -not $signerText.Contains('[string]$ExpectedSourceRevision')) {
    throw 'Release signing does not fail closed on clean source and exact binary identity.'
}

& $verifier -BinaryDirectory $BinaryDirectory -ExpectedVersion $ExpectedVersion `
    -ExpectedSourceRevision $ExpectedSourceRevision | Out-Null
Expect-Failure {
    & $verifier -BinaryDirectory $BinaryDirectory -ExpectedVersion '1.0.0' `
        -ExpectedSourceRevision $ExpectedSourceRevision
} 'stale prerelease binaries presented as final 1.0.0'
Expect-Failure {
    & $verifier -BinaryDirectory $BinaryDirectory -ExpectedVersion $ExpectedVersion `
        -ExpectedSourceRevision ('f' * 40)
} 'binaries presented as a different source revision'

$scratch = Join-Path ([IO.Path]::GetTempPath()) (
    'blackbox-release-binary-identity-' + [guid]::NewGuid())
try {
    $git = @(
        (Get-Command git.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source),
        'C:\Program Files\Git\cmd\git.exe',
        'C:\Program Files\Git\bin\git.exe'
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) -and
                       [IO.File]::Exists($_) } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($git)) {
        throw 'Git is required by the release-source contract test.'
    }
    $repo = Join-Path $scratch 'source'
    [IO.Directory]::CreateDirectory($repo) | Out-Null
    [IO.File]::WriteAllText((Join-Path $repo 'fixture.txt'), "clean`n",
        [Text.UTF8Encoding]::new($false))
    & $git -C $repo init --initial-branch=main | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize release-source fixture.' }
    & $git -C $repo add -- fixture.txt
    if ($LASTEXITCODE -ne 0) { throw 'Cannot stage release-source fixture.' }
    & $git -C $repo -c user.name=BlackBoxContract `
        -c user.email=contract@example.invalid commit -m initial | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Cannot commit release-source fixture.' }
    $revision = (& $git -C $repo rev-parse HEAD).Trim().ToLowerInvariant()
    & $sourceVerifier -SourceRoot $repo -ExpectedSourceRevision $revision `
        -GitExecutable $git | Out-Null
    Expect-Failure {
        & $sourceVerifier -SourceRoot $repo -ExpectedSourceRevision ('f' * 40) `
            -GitExecutable $git
    } 'wrong release source revision'
    [IO.File]::WriteAllText((Join-Path $repo 'fixture.txt'), "dirty`n",
        [Text.UTF8Encoding]::new($false))
    Expect-Failure {
        & $sourceVerifier -SourceRoot $repo -ExpectedSourceRevision $revision `
            -GitExecutable $git
    } 'dirty tracked release source'
    [IO.File]::WriteAllText((Join-Path $repo 'fixture.txt'), "clean`n",
        [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $repo 'untracked.txt'), "untracked`n",
        [Text.UTF8Encoding]::new($false))
    Expect-Failure {
        & $sourceVerifier -SourceRoot $repo -ExpectedSourceRevision $revision `
            -GitExecutable $git
    } 'untracked release source'

    $release = Join-Path $scratch 'binaries'
    [IO.Directory]::CreateDirectory($release) | Out-Null
    $sourceRelease = (Resolve-Path $BinaryDirectory).Path
    foreach ($name in @('blackbox.exe', 'blackbox_dataset_tool.exe',
                        'blackbox_dogfood_tool.exe')) {
        Copy-Item -LiteralPath (Join-Path $sourceRelease $name) `
            -Destination (Join-Path $release $name)
    }
    $packageName = "BlackBox-$ExpectedVersion-windows-x64"
    $packageRoot = Join-Path $scratch $packageName
    [IO.Directory]::CreateDirectory((Join-Path $packageRoot 'docs')) | Out-Null
    foreach ($name in @('blackbox.exe', 'blackbox_dataset_tool.exe',
                        'blackbox_dogfood_tool.exe')) {
        Copy-Item -LiteralPath (Join-Path $sourceRelease $name) `
            -Destination (Join-Path $packageRoot $name)
    }
    [IO.File]::WriteAllText((Join-Path $packageRoot 'docs\RELEASE_READINESS.md'),
        "fixture`n", [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $packageRoot 'docs\USER_GUIDE.md'),
        "fixture`n", [Text.UTF8Encoding]::new($false))
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $package = Join-Path $scratch "$packageName.zip"
    [IO.Compression.ZipFile]::CreateFromDirectory(
        $packageRoot, $package, [IO.Compression.CompressionLevel]::Optimal, $true)
    $packageHash = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash
    [IO.File]::WriteAllText("$package.sha256",
        "$packageHash  $([IO.Path]::GetFileName($package))`n",
        [Text.UTF8Encoding]::new($false))
    & $packageVerifier -PackagePath $package -ExpectedVersion $ExpectedVersion `
        -ExpectedSourceRevision $ExpectedSourceRevision | Out-Null
    Expect-Failure {
        & $packageVerifier -PackagePath $package -ExpectedVersion $ExpectedVersion `
            -ExpectedSourceRevision ('f' * 40)
    } 'package executables presented as a different source revision'

    Copy-Item -LiteralPath (Join-Path $sourceRelease 'blackbox.exe') `
        -Destination (Join-Path $release 'blackbox_dataset_tool.exe') -Force
    Expect-Failure {
        & $verifier -BinaryDirectory $release -ExpectedVersion $ExpectedVersion `
            -ExpectedSourceRevision $ExpectedSourceRevision
    } 'renamed executable with mismatched embedded identity'
} finally {
    if ([IO.Directory]::Exists($scratch) -and
        $scratch.StartsWith([IO.Path]::GetTempPath(),
            [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $scratch -Recurse -Force
    }
}

Write-Output 'Release binary identity contracts passed.'
