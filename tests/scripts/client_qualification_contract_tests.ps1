[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-Sidecar([string]$Package) {
    $hash = (Get-FileHash -LiteralPath $Package -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText("$Package.sha256",
        "$hash  $([IO.Path]::GetFileName($Package))`n", [Text.Encoding]::ASCII)
}

function Expect-Failure([scriptblock]$Action, [string]$Name) {
    try {
        & $Action
    } catch {
        Write-Output "Expected rejection: $Name"
        return
    }
    throw "Expected failure did not occur: $Name"
}

function Write-EvidenceManifest([string]$Directory) {
    $manifestPath = Join-Path $Directory 'manifest.sha256.ini'
    $files = @(Get-ChildItem -LiteralPath $Directory -Recurse -File |
        Where-Object { $_.FullName -cne $manifestPath } | Sort-Object FullName)
    $manifest = @('format=1', 'algorithm=sha256', "file_count=$($files.Count)")
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($Directory.Length + 1).Replace('\', '/')
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $manifest += "$relative=$hash"
    }
    [IO.File]::WriteAllText($manifestPath, (($manifest -join "`n") + "`n"))
}

function New-ClientEvidence([string]$Directory, [string]$Package,
                            [string]$Profile, [string]$OsFamily,
                            [int]$DisplayCount, [uint64]$MemoryBytes,
                            [int]$LogicalProcessors, [int]$BatteryPresent,
                            [string[]]$Cases, [hashtable]$BinaryFacts) {
    [IO.Directory]::CreateDirectory((Join-Path $Directory 'data')) | Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $Directory 'data\crashes')) | Out-Null
    $packageName = [IO.Path]::GetFileName($Package)
    Copy-Item -LiteralPath $Package -Destination (Join-Path $Directory $packageName)
    Copy-Item -LiteralPath "$Package.sha256" -Destination (Join-Path $Directory "$packageName.sha256")
    $packageHash = (Get-FileHash -LiteralPath $Package -Algorithm SHA256).Hash.ToLowerInvariant()
    $revision = 'a' * 40
    [IO.File]::WriteAllText((Join-Path $Directory 'summary.ini'), @"
format=1
state=passed
mode=interactive
profile=$Profile
source_revision=$revision
application_sha256=$($BinaryFacts['blackbox.exe'].Hash)
runner_sha256=$('d' * 64)
verifier_sha256=$('e' * 64)
package_name=$packageName
package_sha256=$packageHash
package_authenticode_valid=0
package_timestamped=0
automated_package_smoke_satisfied=1
operator_cases_required=$($Cases.Count)
operator_cases_passed=$($Cases.Count)
single_host_profile_satisfied=1
clean_client_matrix_satisfied=0
physical_matrix_satisfied=0
maximum_working_set_bytes=1024
maximum_private_bytes=2048
maximum_handles=10
blackbox_exe.sha256=$($BinaryFacts['blackbox.exe'].Hash)
blackbox_exe.signature_status=$($BinaryFacts['blackbox.exe'].Status)
blackbox_exe.signer_thumbprint=none
blackbox_exe.timestamp_thumbprint=none
blackbox_dataset_tool_exe.sha256=$($BinaryFacts['blackbox_dataset_tool.exe'].Hash)
blackbox_dataset_tool_exe.signature_status=$($BinaryFacts['blackbox_dataset_tool.exe'].Status)
blackbox_dataset_tool_exe.signer_thumbprint=none
blackbox_dataset_tool_exe.timestamp_thumbprint=none
blackbox_dogfood_tool_exe.sha256=$($BinaryFacts['blackbox_dogfood_tool.exe'].Hash)
blackbox_dogfood_tool_exe.signature_status=$($BinaryFacts['blackbox_dogfood_tool.exe'].Status)
blackbox_dogfood_tool_exe.signer_thumbprint=none
blackbox_dogfood_tool_exe.timestamp_thumbprint=none
"@)
    [IO.File]::WriteAllText((Join-Path $Directory 'campaign.ini'), @"
format=1
state=passed
mode=interactive
profile=$Profile
source_revision=$revision
process_id=0
application_sha256=$($BinaryFacts['blackbox.exe'].Hash)
runner_sha256=$('d' * 64)
verifier_sha256=$('e' * 64)
"@)
    [IO.File]::WriteAllText((Join-Path $Directory 'host.ini'), @"
format=1
os_family=$OsFamily
os_build=fixture
architecture=x64
supported_client=1
display_count=$DisplayCount
battery_present=$BatteryPresent
physical_memory_bytes=$MemoryBytes
logical_processors=$LogicalProcessors
"@)
    [IO.File]::WriteAllText((Join-Path $Directory 'app-report.ini'), @"
format=1
completed=1
archive_healthy=1
archive_schema_version=1
collections=10
failed_samples=0
dropped_samples=0
deadline_misses=0
collector_worker_failures=0
snapshot_failures=0
capture_queue_rejections=0
event_worker_failures=0
native_events_dropped=0
writer_cancelled=0
writer_retry_exhausted=0
writer_failed=0
power_events_recorded=0
device_events_recorded=0
audio_events_recorded=0
service_events_recorded=0
defender_events_recorded=0
windows_update_events_recorded=0
application_events_recorded=0
network_events_recorded=0
graphics_events_recorded=0
storage_events_recorded=0
system_events_recorded=0
writer_succeeded=1
incidents_completed=1
archive_incidents=1
"@)
    [IO.File]::WriteAllText((Join-Path $Directory 'data\product-settings.ini'),
        "format=1`nfixture=1`n")
    [IO.File]::WriteAllText((Join-Path $Directory 'data\recorder-settings.ini'),
        "format=1`nfixture=1`n")
    $archive = [byte[]]::new(512)
    [Text.Encoding]::ASCII.GetBytes("SQLite format 3`0").CopyTo($archive, 0)
    [IO.File]::WriteAllBytes((Join-Path $Directory 'data\incidents.sqlite3'), $archive)
    [IO.File]::WriteAllText((Join-Path $Directory 'process-samples.tsv'),
        "elapsed_seconds`tutc`tphase`tworking_set_bytes`tprivate_bytes`thandles`ttotal_cpu_seconds`r`n" +
        "1`t2026-01-01T00:00:00Z`tdiagnostic`t1`t1`t1`t0`r`n")
    $required = @("format`tcase") + @($Cases | ForEach-Object { "1`t$_" })
    [IO.File]::WriteAllText((Join-Path $Directory 'required-cases.tsv'),
        (($required -join "`r`n") + "`r`n"))
    $results = @("utc`tcase`tresult") + @($Cases | ForEach-Object {
        "2026-01-01T00:00:00Z`t$_`tpass"
    })
    [IO.File]::WriteAllText((Join-Path $Directory 'operator-results.tsv'),
        (($results -join "`r`n") + "`r`n"))
    Write-EvidenceManifest $Directory
}

$root = Join-Path ([IO.Path]::GetTempPath()) ("blackbox-client-contract-" + [guid]::NewGuid())
[IO.Directory]::CreateDirectory($root) | Out-Null
try {
    $verify = Join-Path $SourceRoot 'scripts\verify-release.ps1'
    $verifyEvidence = Join-Path $SourceRoot 'scripts\verify-client-evidence.ps1'
    $record = Join-Path $SourceRoot 'scripts\record-client-case.ps1'
    $matrix = Join-Path $SourceRoot 'scripts\verify-client-matrix.ps1'
    $verifyMatrixEvidence = Join-Path $SourceRoot 'scripts\verify-client-matrix-evidence.ps1'
    foreach ($script in @($verify, $verifyEvidence, $record, $matrix, $verifyMatrixEvidence,
                          (Join-Path $SourceRoot 'scripts\run-client-qualification.ps1'))) {
        $tokens = $null
        $errors = $null
        [void][System.Management.Automation.Language.Parser]::ParseFile(
            $script, [ref]$tokens, [ref]$errors)
        if ($errors.Count -ne 0) { throw "PowerShell parser rejected $script" }
    }
    $runnerContract = [IO.File]::ReadAllText(
        (Join-Path $SourceRoot 'scripts\run-client-qualification.ps1'))
    foreach ($requiredRunnerClause in @(
        "Get-Process -Name 'blackbox'",
        'requires a clean single-instance host',
        '-ExpectedSourceRevision ($SourceRevision.ToLowerInvariant())',
        '& $verifierScript -CampaignDirectory $staging -AllowStaging')) {
        if (-not $runnerContract.Contains($requiredRunnerClause)) {
            throw "Client qualification runner is missing its required contract: $requiredRunnerClause"
        }
    }

    $packageName = 'BlackBox-contract-windows-x64'
    $source = Join-Path $root $packageName
    [IO.Directory]::CreateDirectory((Join-Path $source 'docs')) | Out-Null
    foreach ($relative in @('blackbox.exe', 'blackbox_dataset_tool.exe',
                            'blackbox_dogfood_tool.exe', 'docs\RELEASE_READINESS.md',
                            'docs\USER_GUIDE.md')) {
        [IO.File]::WriteAllText((Join-Path $source $relative), "fixture-$relative")
    }
    $package = Join-Path $root "$packageName.zip"
    Compress-Archive -LiteralPath $source -DestinationPath $package
    Write-Sidecar $package
    & $verify -PackagePath $package | Out-Null
    $binaryFacts = @{}
    foreach ($binary in @('blackbox.exe', 'blackbox_dataset_tool.exe', 'blackbox_dogfood_tool.exe')) {
        $binaryPath = Join-Path $source $binary
        $binaryFacts[$binary] = @{
            Hash = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
            Status = [string](Get-AuthenticodeSignature -LiteralPath $binaryPath).Status
        }
    }

    $badChecksum = Join-Path $root 'BlackBox-bad-checksum-windows-x64.zip'
    Copy-Item -LiteralPath $package -Destination $badChecksum
    [IO.File]::WriteAllText("$badChecksum.sha256",
        (('0' * 64) + "  $([IO.Path]::GetFileName($badChecksum))`n"),
        [Text.Encoding]::ASCII)
    Expect-Failure { & $verify -PackagePath $badChecksum | Out-Null } 'checksum mismatch'

    $traversal = Join-Path $root 'BlackBox-traversal-windows-x64.zip'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $stream = [IO.File]::Open($traversal, [IO.FileMode]::CreateNew)
    try {
        $archive = [IO.Compression.ZipArchive]::new(
            $stream, [IO.Compression.ZipArchiveMode]::Create, $true)
        try {
            [void]$archive.CreateEntry('../escape.txt')
        } finally {
            $archive.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
    Write-Sidecar $traversal
    Expect-Failure { & $verify -PackagePath $traversal | Out-Null } 'ZIP traversal'

    $campaign = Join-Path $root 'campaign.partial'
    [IO.Directory]::CreateDirectory($campaign) | Out-Null
    [IO.File]::WriteAllText((Join-Path $campaign 'campaign.ini'),
        "format=1`nstate=running`nmode=interactive`nprofile=standard`n" +
        "source_revision=$('a' * 40)`nprocess_id=$PID`n" +
        "application_sha256=$((Get-FileHash -LiteralPath (Get-Process -Id $PID).Path -Algorithm SHA256).Hash.ToLowerInvariant())`n" +
        "runner_sha256=$('d' * 64)`nverifier_sha256=$('e' * 64)`n")
    [IO.File]::WriteAllText((Join-Path $campaign 'required-cases.tsv'),
        "format`tcase`r`n1`tpackage_launch_ordinary_user`r`n")
    [IO.File]::WriteAllText((Join-Path $campaign 'operator-results.tsv'),
        "utc`tcase`tresult`r`n")
    & $record -CampaignDirectory $campaign -Case package_launch_ordinary_user -Result pass | Out-Null
    Expect-Failure {
        & $record -CampaignDirectory $campaign -Case package_launch_ordinary_user -Result pass | Out-Null
    } 'duplicate operator result'
    Expect-Failure {
        & $record -CampaignDirectory $campaign -Case scale_100 -Result pass | Out-Null
    } 'case outside profile'

    Expect-Failure {
        & $matrix -EvidenceDirectory @($campaign) -OutputDirectory (Join-Path $root 'matrix') | Out-Null
    } 'incomplete client matrix'

    $caseMap = @{
        standard = @(
            'package_launch_ordinary_user', 'tray_hide_restore', 'global_hotkey_capture',
            'first_run_onboarding_keyboard', 'focus_visibility_text_scaling',
            'incident_view', 'settings_diagnostics', 'keyboard_navigation',
            'high_contrast_live_toggle', 'hidden_high_contrast_catchup',
            'scale_100', 'scale_125', 'scale_150', 'scale_200')
        multimonitor = @(
            'mixed_scale_monitor_move', 'taskbar_work_area_change',
            'monitor_disconnect_reconnect', 'suspend_resume')
        'low-end' = @('low_end_responsiveness', 'low_end_resource_bounds')
        battery = @(
            'battery_operation', 'battery_saver', 'balanced_power',
            'performance_power', 'suspend_resume_battery')
    }
    $bundles = @()
    $specs = @(
        @('standard', 'windows10_22h2', 1, 16GB, 8, 0),
        @('standard', 'windows11', 1, 16GB, 8, 0),
        @('multimonitor', 'windows11', 2, 16GB, 8, 0),
        @('low-end', 'windows10_22h2', 1, 8GB, 4, 0),
        @('battery', 'windows11', 1, 16GB, 8, 1)
    )
    for ($index = 0; $index -lt $specs.Count; ++$index) {
        $spec = $specs[$index]
        $bundle = Join-Path $root "bundle-$index"
        $evidenceArguments = @{
            Directory = $bundle
            Package = $package
            Profile = $spec[0]
            OsFamily = $spec[1]
            DisplayCount = $spec[2]
            MemoryBytes = $spec[3]
            LogicalProcessors = $spec[4]
            BatteryPresent = $spec[5]
            Cases = $caseMap[$spec[0]]
            BinaryFacts = $binaryFacts
        }
        New-ClientEvidence @evidenceArguments
        $bundles += $bundle
    }
    & $verifyEvidence -CampaignDirectory $bundles[0] -RequireInteractive | Out-Null
    $windowsPowerShell = Get-Command powershell.exe -ErrorAction SilentlyContinue
    if ($null -ne $windowsPowerShell) {
        & $windowsPowerShell.Source -NoProfile -NonInteractive -ExecutionPolicy Bypass `
            -File $verifyEvidence -CampaignDirectory $bundles[0] -RequireInteractive | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw 'The client evidence verifier failed under Windows PowerShell 5.1.'
        }
    }

    $smokeEvidence = Join-Path $root 'bundle-smoke'
    Copy-Item -LiteralPath $bundles[0] -Destination $smokeEvidence -Recurse
    foreach ($relative in @('summary.ini', 'campaign.ini')) {
        $path = Join-Path $smokeEvidence $relative
        $text = [IO.File]::ReadAllText($path) -replace
            '(?m)^mode=interactive$', 'mode=smoke'
        [IO.File]::WriteAllText($path, $text)
    }
    $smokeSummaryPath = Join-Path $smokeEvidence 'summary.ini'
    $smokeSummary = [IO.File]::ReadAllText($smokeSummaryPath) `
        -replace '(?m)^operator_cases_required=\d+$', 'operator_cases_required=0' `
        -replace '(?m)^operator_cases_passed=\d+$', 'operator_cases_passed=0' `
        -replace '(?m)^single_host_profile_satisfied=1$', 'single_host_profile_satisfied=0'
    [IO.File]::WriteAllText($smokeSummaryPath, $smokeSummary)
    [IO.File]::WriteAllText((Join-Path $smokeEvidence 'required-cases.tsv'),
        "format`tcase`r`n")
    [IO.File]::WriteAllText((Join-Path $smokeEvidence 'operator-results.tsv'),
        "utc`tcase`tresult`r`n")
    Write-EvidenceManifest $smokeEvidence
    & $verifyEvidence -CampaignDirectory $smokeEvidence | Out-Null

    $partialEvidence = Join-Path $root 'bundle-partial.partial'
    Copy-Item -LiteralPath $bundles[0] -Destination $partialEvidence -Recurse
    Expect-Failure {
        & $verifyEvidence -CampaignDirectory $partialEvidence -RequireInteractive | Out-Null
    } 'partial client evidence'
    & $verifyEvidence -CampaignDirectory $partialEvidence -RequireInteractive -AllowStaging | Out-Null

    $wrongApplication = Join-Path $root 'bundle-wrong-application'
    Copy-Item -LiteralPath $bundles[0] -Destination $wrongApplication -Recurse
    foreach ($relative in @('summary.ini', 'campaign.ini')) {
        $path = Join-Path $wrongApplication $relative
        $text = [IO.File]::ReadAllText($path) -replace
            '(?m)^application_sha256=[0-9a-f]{64}$', "application_sha256=$('f' * 64)"
        [IO.File]::WriteAllText($path, $text)
    }
    Write-EvidenceManifest $wrongApplication
    Expect-Failure {
        & $verifyEvidence -CampaignDirectory $wrongApplication -RequireInteractive | Out-Null
    } 'application outside package'

    $extraDirectory = Join-Path $root 'bundle-extra-directory'
    Copy-Item -LiteralPath $bundles[0] -Destination $extraDirectory -Recurse
    [IO.Directory]::CreateDirectory((Join-Path $extraDirectory 'unexpected-empty')) | Out-Null
    Expect-Failure {
        & $verifyEvidence -CampaignDirectory $extraDirectory -RequireInteractive | Out-Null
    } 'extra evidence directory'

    $matrixOutput = Join-Path $root 'valid-matrix'
    & $matrix -EvidenceDirectory $bundles -OutputDirectory $matrixOutput | Out-Null
    & $verifyMatrixEvidence -MatrixDirectory $matrixOutput -EvidenceDirectory $bundles `
        -ExpectedSourceRevision ('a' * 40) | Out-Null
    $matrixSummary = [IO.File]::ReadAllText((Join-Path $matrixOutput 'summary.ini'))
    if ($matrixSummary -notmatch '(?m)^clean_client_matrix_satisfied=1$' -or
        $matrixSummary -notmatch '(?m)^physical_matrix_satisfied=1$') {
        throw 'The complete fixture matrix did not publish its qualification result.'
    }
    Expect-Failure {
        & $verifyMatrixEvidence -MatrixDirectory $matrixOutput -EvidenceDirectory $bundles `
            -ExpectedSourceRevision ('f' * 40) | Out-Null
    } 'wrong client matrix revision'
    Expect-Failure {
        & $verifyMatrixEvidence -MatrixDirectory $matrixOutput -EvidenceDirectory $bundles `
            -RequireAuthenticode | Out-Null
    } 'unsigned official client matrix'
    $matrixPartial = "$matrixOutput.partial"
    [IO.Directory]::Move($matrixOutput, $matrixPartial)
    Expect-Failure {
        & $verifyMatrixEvidence -MatrixDirectory $matrixPartial -EvidenceDirectory $bundles | Out-Null
    } 'partial client matrix evidence'
    [IO.Directory]::Move($matrixPartial, $matrixOutput)
    $matrixSourcesPath = Join-Path $matrixOutput 'sources.tsv'
    [IO.File]::AppendAllText($matrixSourcesPath, "tampered`r`n")
    Expect-Failure {
        & $verifyMatrixEvidence -MatrixDirectory $matrixOutput -EvidenceDirectory $bundles | Out-Null
    } 'changed client matrix evidence'
    [IO.File]::AppendAllText((Join-Path $bundles[0] 'host.ini'), "tampered=1`n")
    Expect-Failure {
        & $matrix -EvidenceDirectory $bundles -OutputDirectory (Join-Path $root 'tampered-matrix') | Out-Null
    } 'tampered client evidence'

    Write-Output 'Client qualification script contracts passed.'
} finally {
    if ([IO.Directory]::Exists($root)) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
