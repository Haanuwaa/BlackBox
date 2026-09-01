[CmdletBinding()]
param(
    [string]$SourceRoot = (Split-Path -Parent $PSScriptRoot)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Require-Text([string]$Text, [string]$Pattern, [string]$Description) {
    if ($Text -notmatch $Pattern) {
        throw "Quality gate contract failed: missing $Description"
    }
}

function Reject-Text([string]$Text, [string]$Pattern, [string]$Description) {
    if ($Text -match $Pattern) {
        throw "Quality gate contract failed: found $Description"
    }
}

$root = (Resolve-Path -LiteralPath $SourceRoot).Path
$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
$tests = Get-Content -LiteralPath (Join-Path $root 'tests/CMakeLists.txt') -Raw
$workflow = Get-Content -LiteralPath (Join-Path $root '.github/workflows/quality.yml') -Raw
$presets = Get-Content -LiteralPath (Join-Path $root 'CMakePresets.json') -Raw
$asanTripletPath = Join-Path $root 'cmake/triplets/x64-windows-blackbox-asan.cmake'
$codeqlStart = $workflow.IndexOf("`n  codeql:", [StringComparison]::Ordinal)
$codeqlEnd = $workflow.IndexOf("`n  msvc-static-analysis:", [StringComparison]::Ordinal)
if ($codeqlStart -lt 0 -or $codeqlEnd -le $codeqlStart) {
    throw 'Quality gate contract failed: CodeQL workflow boundary is missing'
}
$codeqlWorkflow = $workflow.Substring($codeqlStart, $codeqlEnd - $codeqlStart)

foreach ($option in @(
    'BLACKBOX_ENABLE_ADDRESS_SANITIZER',
    'BLACKBOX_ENABLE_UNDEFINED_SANITIZER',
    'BLACKBOX_ENABLE_THREAD_SANITIZER',
    'BLACKBOX_ENABLE_MSVC_CODE_ANALYSIS',
    'BLACKBOX_ENABLE_COVERAGE',
    'BLACKBOX_BUILD_FUZZERS')) {
    Require-Text $cmake ("option\(" + $option) "CMake option $option"
}
Require-Text $cmake 'Coverage must run in its own build graph' 'incompatible-mode rejection'
Require-Text $cmake 'clang_rt\.asan_dynamic-x86_64\.dll' 'MSVC ASan runtime discovery'
Require-Text $cmake 'blackbox_copy_address_sanitizer_runtime' 'MSVC ASan runtime staging helper'
Require-Text $tests 'settings_native_fuzz_smoke' 'native fuzz smoke registration'
Require-Text $tests 'native_parser_fuzz_smoke' 'native parser fuzz smoke registration'
Require-Text $tests 'strict_v1_input_property_tests\.cpp' 'strict-v1 mutation property test'
Require-Text $tests 'corrupt_archive_property_tests\.cpp' 'corrupt archive property test'

foreach ($job in @(
    'dependency-policy-sbom',
    'dependency-review',
    'codeql',
    'msvc-static-analysis',
    'windows-address-sanitizer',
    'linux-undefined-sanitizer',
    'linux-thread-sanitizer',
    'linux-native-fuzz',
    'linux-coverage')) {
    Require-Text $workflow ("(?m)^  " + [regex]::Escape($job) + ':$') "workflow job $job"
}
Require-Text $workflow 'BLACKBOX_ENABLE_ADDRESS_SANITIZER=ON' 'AddressSanitizer activation'
Require-Text $workflow 'BLACKBOX_ENABLE_UNDEFINED_SANITIZER=ON' 'UndefinedBehaviorSanitizer activation'
Require-Text $workflow 'BLACKBOX_ENABLE_THREAD_SANITIZER=ON' 'ThreadSanitizer activation'
Require-Text $workflow 'BLACKBOX_ENABLE_MSVC_CODE_ANALYSIS=ON' 'MSVC analysis activation'
Require-Text $workflow 'BLACKBOX_BUILD_FUZZERS=ON' 'libFuzzer activation'
Require-Text $workflow 'BLACKBOX_ENABLE_COVERAGE=ON' 'coverage activation'
if (([regex]::Matches($workflow, '-max_total_time=30')).Count -ne 2) {
    throw 'Quality gate contract failed: native fuzz campaign must be split into two bounded 30-second targets'
}
$fuzzJobStart = $workflow.IndexOf("`n  linux-native-fuzz:", [System.StringComparison]::Ordinal)
$threadSanitizerJobStart = $workflow.IndexOf("`n  linux-thread-sanitizer:", [System.StringComparison]::Ordinal)
$coverageJobStart = $workflow.IndexOf("`n  linux-coverage:", [System.StringComparison]::Ordinal)
if ($fuzzJobStart -lt 0 -or $threadSanitizerJobStart -le $fuzzJobStart -or
    $coverageJobStart -le $threadSanitizerJobStart) {
    throw 'Quality gate contract failed: sanitizer and fuzz job boundaries are malformed'
}
$fuzzJob = $workflow.Substring($fuzzJobStart, $threadSanitizerJobStart - $fuzzJobStart)
$threadSanitizerJob = $workflow.Substring(
    $threadSanitizerJobStart, $coverageJobStart - $threadSanitizerJobStart)
Require-Text $fuzzJob 'blackbox_settings_fuzzer[\s\S]+-max_total_time=30' 'settings fuzz campaign placement'
Require-Text $fuzzJob 'blackbox_native_parser_fuzzer[\s\S]+-max_total_time=30' 'parser fuzz campaign placement'
if (([regex]::Matches($fuzzJob, '-max_total_time=30')).Count -ne 2 -or
    $threadSanitizerJob.Contains('out/build/linux-fuzz', [System.StringComparison]::Ordinal)) {
    throw 'Quality gate contract failed: both bounded fuzz campaigns must run only in linux-native-fuzz'
}
Require-Text $workflow '--fail-under-line 60' 'line coverage floor'
Require-Text $workflow '--fail-under-branch 45' 'branch coverage floor'
Require-Text $workflow '--fail-under-line 45' 'component line coverage floor'
Require-Text $workflow '--fail-under-branch 30' 'component branch coverage floor'
Require-Text $codeqlWorkflow 'actions/cache@[0-9a-f]{40}' 'immutable CodeQL dependency cache action'
Require-Text $workflow 'queries: security-extended' 'extended CodeQL security query suite'
Reject-Text $workflow 'queries:\s*[^\r\n]*security-and-quality' `
    'broad CodeQL quality suite in security alert output'
Require-Text $workflow 'group: \$\{\{ github\.workflow \}\}-\$\{\{ github\.ref \}\}' `
    'same-workflow same-ref concurrency group'
Require-Text $workflow 'cancel-in-progress: true' 'obsolete workflow cancellation'
Require-Text $codeqlWorkflow 'timeout-minutes: 60' 'bounded CodeQL wall time'
Require-Text $codeqlWorkflow 'build-mode: manual' 'manual compiled-language CodeQL build mode'
Require-Text $codeqlWorkflow 'BLACKBOX_BUILD_TESTS=OFF' 'production-only CodeQL graph'
$dependencyStep = $codeqlWorkflow.IndexOf(
    '- name: Resolve production dependencies outside CodeQL tracing',
    [StringComparison]::Ordinal)
$initializationStep = $codeqlWorkflow.IndexOf(
    '- name: Initialize CodeQL C++ analysis', [StringComparison]::Ordinal)
$productionBuildStep = $codeqlWorkflow.IndexOf(
    '- name: Build observed production graph', [StringComparison]::Ordinal)
if ($dependencyStep -lt 0 -or $initializationStep -le $dependencyStep -or
    $productionBuildStep -le $initializationStep) {
    throw 'Quality gate contract failed: CodeQL must resolve dependencies before tracing the production build'
}
foreach ($target in @(
    'blackbox',
    'blackbox_dataset_tool',
    'blackbox_soak_archive_fault',
    'blackbox_dogfood_tool',
    'blackbox_offline_ml_tool',
    'blackbox_dogfood_capture')) {
    Require-Text $codeqlWorkflow ("(?m)^\s+" + [regex]::Escape($target) + '\s*`?$') `
        "CodeQL production target $target"
}
Require-Text $workflow 'fail-on-severity: moderate' 'dependency vulnerability floor'
Require-Text $workflow '-E "Windows unhandled exception probe"' 'sanitizer crash-probe exclusion'
Require-Text $workflow 'VCPKG_TARGET_TRIPLET=x64-windows-blackbox-asan' 'instrumented Windows dependency triplet'
Require-Text $presets '"VCPKG_TARGET_TRIPLET": "x64-windows-blackbox-asan"' 'local instrumented dependency preset'

if (-not (Test-Path -LiteralPath $asanTripletPath -PathType Leaf)) {
    throw 'Quality gate contract failed: missing MSVC ASan dependency triplet'
}
$asanTriplet = Get-Content -LiteralPath $asanTripletPath -Raw
Require-Text $asanTriplet 'VCPKG_CXX_FLAGS "\/fsanitize=address \/Zi"' 'ASan dependency C++ instrumentation'

foreach ($path in @(
    'tests/fuzz/settings_fuzzer.cpp',
    'tests/fuzz/native_parser_fuzzer.cpp',
    'tests/fuzz/corpus/product-settings.ini',
    'tests/fuzz/corpus/recorder-settings.ini',
    'tests/fuzz/corpus/linux-proc-stat.txt',
    'tests/fuzz/corpus/linux-psi.txt',
    'scripts/verify-dependency-policy.ps1',
    'scripts/generate-sbom.ps1')) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $path) -PathType Leaf)) {
        throw "Quality gate contract failed: missing $path"
    }
}

Write-Output 'Quality gate contract verified: asan=1 ubsan=1 tsan=1 msvc_analysis=1 fuzz_targets=2 property=2 dependency_review=1 codeql_production_targets=6 codeql_dependency_prime=1 codeql_cache=1 codeql_concurrency=1 sbom=1 coverage=60/45 components=45/30'
