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

foreach ($option in @(
    'BLACKBOX_ENABLE_ADDRESS_SANITIZER',
    'BLACKBOX_ENABLE_UNDEFINED_SANITIZER',
    'BLACKBOX_ENABLE_MSVC_CODE_ANALYSIS',
    'BLACKBOX_ENABLE_COVERAGE',
    'BLACKBOX_BUILD_FUZZERS')) {
    Require-Text $cmake ("option\(" + $option) "CMake option $option"
}
Require-Text $cmake 'Coverage must run in its own build graph' 'incompatible-mode rejection'
Require-Text $cmake 'clang_rt\.asan_dynamic-x86_64\.dll' 'MSVC ASan runtime discovery'
Require-Text $cmake 'blackbox_copy_address_sanitizer_runtime' 'MSVC ASan runtime staging helper'
Require-Text $tests 'settings_native_fuzz_smoke' 'native fuzz smoke registration'
Require-Text $tests 'strict_v1_input_property_tests\.cpp' 'strict-v1 mutation property test'
Require-Text $tests 'corrupt_archive_property_tests\.cpp' 'corrupt archive property test'

foreach ($job in @(
    'dependency-policy-sbom',
    'dependency-review',
    'codeql',
    'msvc-static-analysis',
    'windows-address-sanitizer',
    'linux-undefined-sanitizer',
    'linux-native-fuzz',
    'linux-coverage')) {
    Require-Text $workflow ("(?m)^  " + [regex]::Escape($job) + ':$') "workflow job $job"
}
Require-Text $workflow 'BLACKBOX_ENABLE_ADDRESS_SANITIZER=ON' 'AddressSanitizer activation'
Require-Text $workflow 'BLACKBOX_ENABLE_UNDEFINED_SANITIZER=ON' 'UndefinedBehaviorSanitizer activation'
Require-Text $workflow 'BLACKBOX_ENABLE_MSVC_CODE_ANALYSIS=ON' 'MSVC analysis activation'
Require-Text $workflow 'BLACKBOX_BUILD_FUZZERS=ON' 'libFuzzer activation'
Require-Text $workflow 'BLACKBOX_ENABLE_COVERAGE=ON' 'coverage activation'
Require-Text $workflow '-max_total_time=60' 'bounded 60-second native fuzz campaign'
Require-Text $workflow '--fail-under-line 60' 'line coverage floor'
Require-Text $workflow '--fail-under-branch 45' 'branch coverage floor'
Require-Text $workflow 'queries: security-extended' 'extended CodeQL security query suite'
Reject-Text $workflow 'queries:\s*[^\r\n]*security-and-quality' `
    'broad CodeQL quality suite in security alert output'
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
    'tests/fuzz/corpus/product-settings.ini',
    'tests/fuzz/corpus/recorder-settings.ini',
    'scripts/verify-dependency-policy.ps1',
    'scripts/generate-sbom.ps1')) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $path) -PathType Leaf)) {
        throw "Quality gate contract failed: missing $path"
    }
}

Write-Output 'Quality gate contract verified: asan=1 ubsan=1 msvc_analysis=1 fuzz=1 property=2 dependency_review=1 codeql=1 sbom=1 coverage=60/45'
