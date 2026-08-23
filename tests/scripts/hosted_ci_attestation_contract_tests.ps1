[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Expect-Failure([scriptblock]$Action, [string]$Name) {
    $failed = $false
    try { & $Action } catch { $failed = $true }
    if (-not $failed) { throw "Expected rejection: $Name" }
    Write-Output "Expected rejection: $Name"
}

$writer = Join-Path $SourceRoot 'scripts\write-hosted-ci-attestation.ps1'
$verifier = Join-Path $SourceRoot 'scripts\verify-hosted-ci-attestation.ps1'
foreach ($script in @($writer, $verifier)) {
    $tokens = $null
    $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile(
        $script, [ref]$tokens, [ref]$errors)
    if ($errors.Count -ne 0) { throw "PowerShell parser rejected $script" }
}

$windowsWorkflow = [IO.File]::ReadAllText((Join-Path $SourceRoot '.github\workflows\windows.yml'))
$qualityWorkflow = [IO.File]::ReadAllText((Join-Path $SourceRoot '.github\workflows\quality.yml'))

foreach ($workflow in @($windowsWorkflow, $qualityWorkflow)) {
    if ($workflow -match '(?m)^\s*VCPKG_ROOT:\s*\$\{\{\s*github\.workspace\s*\}\}[/\\]vcpkg') {
        throw 'Hosted workflows must not bootstrap vcpkg inside the source checkout.'
    }
    $declaredRoots = [regex]::Matches($workflow, '(?m)^\s*VCPKG_ROOT:\s*.+$').Count
    $isolatedRoots = [regex]::Matches(
        $workflow, '(?m)^\s*VCPKG_ROOT:\s*\$\{\{\s*github\.workspace\s*\}\}-vcpkg\s*$').Count
    if ($declaredRoots -eq 0 -or $isolatedRoots -ne $declaredRoots) {
        throw 'Hosted workflows must bootstrap mutable dependencies outside the checkout.'
    }
}
foreach ($clause in @(
    'preset: windows-vs2026-release',
    'os: windows-2025',
    '- name: Install native Linux desktop dependencies',
    '- name: Configure portable headless graph',
    'SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=software',
    'needs: [build-test-package, headless-collection, linux-boundary]',
    '-WorkflowKey windows -OutputDirectory out/hosted-ci/windows',
    'BlackBox-hosted-ci-windows-${{ github.sha }}')) {
    if (-not $windowsWorkflow.Contains($clause)) {
        throw "Windows workflow lacks its hosted evidence contract: $clause"
    }
}
$linuxDependencies = $windowsWorkflow.IndexOf(
    '- name: Install native Linux desktop dependencies',
    [StringComparison]::Ordinal)
$linuxFirstConfigure = $windowsWorkflow.IndexOf(
    '- name: Configure portable headless graph',
    [StringComparison]::Ordinal)
if ($linuxDependencies -lt 0 -or $linuxFirstConfigure -lt 0 -or
    $linuxDependencies -gt $linuxFirstConfigure) {
    throw 'Linux desktop dependencies must be installed before vcpkg primes its SDL cache.'
}
foreach ($clause in @(
    '- dependency-policy-sbom', '- dependency-review', '- codeql', '- msvc-static-analysis',
    '- windows-address-sanitizer', '- linux-undefined-sanitizer',
    '- linux-native-fuzz', '- linux-coverage',
    'CC=gcc-14 CXX=g++-14', 'CC=clang-18 CXX=clang++-18',
    '-DCMAKE_CXX_FLAGS="-D__cpp_concepts=202002L"',
    'cmake --build out/build/windows-address-sanitizer --config RelWithDebInfo',
    'mkdir -p out/quality',
    '--gcov-ignore-parse-errors=negative_hits.warn_once_per_file',
    "base-ref: `${{ steps.dependency-range.outputs.base }}",
    "head-ref: `${{ steps.dependency-range.outputs.head }}",
    'Dependency review requires two distinct exact revisions.',
    '-WorkflowKey quality -OutputDirectory out/hosted-ci/quality',
    'BlackBox-hosted-ci-quality-${{ github.sha }}')) {
    if (-not $qualityWorkflow.Contains($clause)) {
        throw "Quality workflow lacks its hosted evidence contract: $clause"
    }
}
$asanJob = [regex]::Match(
    $qualityWorkflow,
    '(?ms)^  windows-address-sanitizer:\r?\n.*?(?=^  [a-z0-9-]+:\r?$)'
).Value
if ([string]::IsNullOrWhiteSpace($asanJob) -or
    $asanJob.Contains('--target blackbox blackbox_tests')) {
    throw 'Hosted ASan must build the complete graph required by its registered tests.'
}

$names = @(
    'GITHUB_ACTIONS', 'GITHUB_SHA', 'GITHUB_REPOSITORY', 'GITHUB_RUN_ID',
    'GITHUB_RUN_ATTEMPT', 'GITHUB_EVENT_NAME', 'GITHUB_REF', 'GITHUB_WORKFLOW',
    'GITHUB_WORKFLOW_REF'
)
$saved = @{}
foreach ($name in $names) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$root = Join-Path ([IO.Path]::GetTempPath()) ("blackbox-hosted-ci-contract-" + [guid]::NewGuid())
[IO.Directory]::CreateDirectory($root) | Out-Null
$revision = '0123456789abcdef0123456789abcdef01234567'
try {
    [Environment]::SetEnvironmentVariable('GITHUB_ACTIONS', $null, 'Process')
    Expect-Failure {
        & $writer -WorkflowKey windows -OutputDirectory (Join-Path $root 'local') | Out-Null
    } 'local hosted attestation'

    [Environment]::SetEnvironmentVariable('GITHUB_ACTIONS', 'true', 'Process')
    [Environment]::SetEnvironmentVariable('GITHUB_SHA', $revision, 'Process')
    [Environment]::SetEnvironmentVariable('GITHUB_REPOSITORY', 'flight/blackbox', 'Process')
    [Environment]::SetEnvironmentVariable('GITHUB_RUN_ID', '123456789', 'Process')
    [Environment]::SetEnvironmentVariable('GITHUB_RUN_ATTEMPT', '2', 'Process')
    [Environment]::SetEnvironmentVariable('GITHUB_EVENT_NAME', 'push', 'Process')
    [Environment]::SetEnvironmentVariable('GITHUB_REF', 'refs/heads/main', 'Process')

    $windows = Join-Path $root 'windows'
    [Environment]::SetEnvironmentVariable('GITHUB_WORKFLOW', 'Windows validation', 'Process')
    [Environment]::SetEnvironmentVariable('GITHUB_WORKFLOW_REF',
        'flight/blackbox/.github/workflows/windows.yml@refs/heads/main', 'Process')
    & $writer -WorkflowKey windows -OutputDirectory $windows | Out-Null
    & $verifier -AttestationDirectory $windows -WorkflowKey windows `
        -ExpectedSourceRevision $revision | Out-Null
    Expect-Failure {
        & $writer -WorkflowKey windows -OutputDirectory $windows | Out-Null
    } 'occupied attestation destination'
    Expect-Failure {
        & $verifier -AttestationDirectory $windows -WorkflowKey windows `
            -ExpectedSourceRevision ('f' * 40) | Out-Null
    } 'wrong attestation revision'

    $quality = Join-Path $root 'quality'
    [Environment]::SetEnvironmentVariable('GITHUB_WORKFLOW', 'Quality and security', 'Process')
    [Environment]::SetEnvironmentVariable('GITHUB_WORKFLOW_REF',
        'flight/blackbox/.github/workflows/quality.yml@refs/heads/main', 'Process')
    & $writer -WorkflowKey quality -OutputDirectory $quality | Out-Null
    & $verifier -AttestationDirectory $quality -WorkflowKey quality `
        -ExpectedSourceRevision $revision | Out-Null

    $partial = Join-Path $root 'partial.partial'
    Copy-Item -LiteralPath $windows -Destination $partial -Recurse
    Expect-Failure {
        & $verifier -AttestationDirectory $partial -WorkflowKey windows `
            -ExpectedSourceRevision $revision | Out-Null
    } 'partial hosted attestation'

    $tampered = Join-Path $root 'tampered'
    Copy-Item -LiteralPath $windows -Destination $tampered -Recurse
    [IO.File]::AppendAllText((Join-Path $tampered 'summary.ini'), "run_id=9`n")
    Expect-Failure {
        & $verifier -AttestationDirectory $tampered -WorkflowKey windows `
            -ExpectedSourceRevision $revision | Out-Null
    } 'tampered hosted attestation'

    $extra = Join-Path $root 'extra'
    Copy-Item -LiteralPath $windows -Destination $extra -Recurse
    [IO.File]::WriteAllText((Join-Path $extra 'extra.txt'), 'extra')
    Expect-Failure {
        & $verifier -AttestationDirectory $extra -WorkflowKey windows `
            -ExpectedSourceRevision $revision | Out-Null
    } 'extra hosted attestation file'

    $wrongWorkflow = Join-Path $root 'wrong-workflow'
    Expect-Failure {
        & $writer -WorkflowKey windows -OutputDirectory $wrongWorkflow | Out-Null
    } 'wrong hosted workflow identity'
    if (Test-Path -LiteralPath $wrongWorkflow) {
        throw 'Rejected workflow identity created output.'
    }
    Write-Output 'Hosted CI attestation contracts passed.'
} finally {
    foreach ($name in $names) {
        [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process')
    }
    if ([IO.Directory]::Exists($root)) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
