[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CampaignDirectory,

    [Parameter(Mandatory = $true)]
    [ValidateSet('sleep_resume', 'lock_unlock', 'device_churn')]
    [string]$Event
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$campaign = [IO.Path]::GetFullPath($CampaignDirectory)
$marker = Join-Path $campaign 'campaign.ini'
if (-not [IO.File]::Exists($marker)) {
    throw 'The campaign marker does not exist.'
}
$markerText = [IO.File]::ReadAllText($marker)
if ($markerText -notmatch '(?m)^format=1$' -or $markerText -notmatch '(?m)^state=running$') {
    throw 'Events can only be recorded for a running direct-v1 campaign.'
}

$journal = Join-Path $campaign 'operator-events.tsv'
$line = "{0}`t{1}`r`n" -f [DateTimeOffset]::UtcNow.ToString('O'), $Event
$bytes = [Text.Encoding]::UTF8.GetBytes($line)
for ($attempt = 1; $attempt -le 20; ++$attempt) {
    try {
        $stream = [IO.File]::Open($journal, [IO.FileMode]::Append,
                                  [IO.FileAccess]::Write, [IO.FileShare]::Read)
        try {
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush($true)
        } finally {
            $stream.Dispose()
        }
        break
    } catch [IO.IOException] {
        if ($attempt -eq 20) { throw }
        Start-Sleep -Milliseconds 50
    }
}

Write-Host "Recorded $Event for the active soak campaign."
