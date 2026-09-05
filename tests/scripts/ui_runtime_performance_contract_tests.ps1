param([Parameter(Mandatory = $true)][string]$SourceRoot)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$script = Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/measure-ui-runtime.ps1') -Raw
$required = @(
    "[ValidateSet('Visible', 'Minimized', 'Hidden', 'Background')]",
    '--visible-diagnostic-seconds=',
    '--background-diagnostic-seconds=',
    'TotalProcessorTime',
    '[Environment]::ProcessorCount',
    'WorkingSet64',
    'PrivateMemorySize64',
    'blackbox-ui-runtime-measurement',
    'application_sha256=',
    'operating_system=',
    'logical_processors=',
    'process_lifetime_seconds=',
    'isolated_settings=1',
    '[switch]$UseProductDefaults',
    'automatic_detection={0}',
    'collect_process_paths={0}',
    'BLACKBOX_PRODUCT_SETTINGS_PATH',
    'BLACKBOX_SETTINGS_PATH',
    '--validate-settings-only',
    '$elapsed.TotalSeconds -le $DurationSeconds',
    '.partial'
)
foreach ($literal in $required) {
    if (-not $script.Contains($literal, [StringComparison]::Ordinal)) {
        throw "UI runtime measurement contract is missing: $literal"
    }
}
if ($script.Contains('Win32_Process', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'UI runtime measurement must use the exact launched process, not a name-wide query.'
}
