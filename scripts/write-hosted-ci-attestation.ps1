[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('windows', 'quality')]
    [string]$WorkflowKey,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Require-Environment([string]$Name, [string]$Pattern, [int]$MaximumLength = 512) {
    $value = [Environment]::GetEnvironmentVariable($Name, 'Process')
    if ([string]::IsNullOrWhiteSpace($value) -or $value.Length -gt $MaximumLength -or
        $value -notmatch $Pattern -or $value -match '[\r\n\t=]') {
        throw "Hosted CI environment is missing or invalid: $Name"
    }
    return $value
}

function Write-AtomicText([string]$Path, [string]$Contents) {
    $temporary = "$Path.tmp"
    [IO.File]::WriteAllText($temporary, $Contents, [Text.UTF8Encoding]::new($false))
    [IO.File]::Move($temporary, $Path)
}

if ([Environment]::GetEnvironmentVariable('GITHUB_ACTIONS', 'Process') -cne 'true') {
    throw 'Hosted CI attestation can only be written by GitHub Actions.'
}

$expected = @{
    windows = @{
        Name = 'Windows validation'
        File = '.github/workflows/windows.yml'
    }
    quality = @{
        Name = 'Quality and security'
        File = '.github/workflows/quality.yml'
    }
}[$WorkflowKey]
$sourceRevision = (Require-Environment 'GITHUB_SHA' '^[0-9A-Fa-f]{40}$' 40).ToLowerInvariant()
$repository = Require-Environment 'GITHUB_REPOSITORY' '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' 200
$runId = Require-Environment 'GITHUB_RUN_ID' '^[1-9][0-9]{0,19}$' 20
$runAttempt = Require-Environment 'GITHUB_RUN_ATTEMPT' '^[1-9][0-9]{0,9}$' 10
$eventName = Require-Environment 'GITHUB_EVENT_NAME' '^(push|workflow_dispatch)$' 32
$ref = Require-Environment 'GITHUB_REF' '^refs/(heads|tags)/[^\r\n\t=]{1,240}$' 256
$workflowName = Require-Environment 'GITHUB_WORKFLOW' '^[^\r\n\t=]{1,200}$' 200
$workflowRef = Require-Environment 'GITHUB_WORKFLOW_REF' '^[^\r\n\t=]{1,500}$' 500
if ($workflowName -cne $expected.Name -or
    $workflowRef -cnotmatch ('^' + [regex]::Escape("$repository/$($expected.File)@") +
                             'refs/(heads|tags)/.+$')) {
    throw 'Hosted CI workflow identity does not match the requested attestation.'
}

$output = [IO.Path]::GetFullPath($OutputDirectory)
$staging = "$output.partial"
if ([IO.Directory]::Exists($output) -or [IO.File]::Exists($output) -or
    [IO.Directory]::Exists($staging) -or [IO.File]::Exists($staging)) {
    throw 'Hosted CI attestation output and staging destinations must not exist.'
}

$writer = [IO.Path]::GetFullPath($PSCommandPath)
$writerHash = (Get-FileHash -LiteralPath $writer -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.Directory]::CreateDirectory($staging) | Out-Null
try {
    $summary = @(
        'format=1', 'state=passed', 'provider=github-actions',
        "workflow_key=$WorkflowKey", "workflow_name=$($expected.Name)",
        "workflow_file=$($expected.File)", "source_revision=$sourceRevision",
        "repository=$repository", "run_id=$runId", "run_attempt=$runAttempt",
        "event_name=$eventName", "ref=$ref", "workflow_ref=$workflowRef",
        "writer_sha256=$writerHash"
    )
    Write-AtomicText (Join-Path $staging 'summary.ini') (($summary -join "`n") + "`n")
    $summaryHash = (Get-FileHash -LiteralPath (Join-Path $staging 'summary.ini') `
                                    -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-AtomicText (Join-Path $staging 'manifest.sha256.ini') (
        "format=1`nalgorithm=sha256`nfile_count=1`nsummary.ini=$summaryHash`n")
    [IO.Directory]::Move($staging, $output)
    Write-Output "Hosted CI attestation generated: $output"
} catch {
    throw
}
