[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$FixtureGenerator,
    [Parameter(Mandatory = $true)] [string]$DogfoodTool,
    [Parameter(Mandatory = $true)] [string]$DatasetTool
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Checked([string]$Executable, [string[]]$Arguments) {
    $output = @(& $Executable @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "$([IO.Path]::GetFileName($Executable)) failed: $($output -join ' ')"
    }
    return $output
}

function Append-Utf8([string]$Path, [string]$Text) {
    [IO.File]::AppendAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

$temporary = Join-Path ([IO.Path]::GetTempPath()) (
    'blackbox-dogfood-acquisition-' + [guid]::NewGuid())
[IO.Directory]::CreateDirectory($temporary) | Out-Null
try {
    $archive = Join-Path $temporary 'incidents.sqlite3'
    [void](Invoke-Checked $FixtureGenerator @($archive))
    $dataset = Join-Path $temporary 'dataset'
    [void](Invoke-Checked $DatasetTool @('export', $archive, $dataset))
    $datasetRows = @(Import-Csv -LiteralPath (Join-Path $dataset 'incidents.tsv') -Delimiter "`t")
    if ($datasetRows.Count -lt 1) { throw 'Fixture dataset contains no incidents.' }
    $incidentKey = $datasetRows[0].incident_key
    if ($incidentKey -notmatch '^[0-9a-f]{32}$') { throw 'Fixture incident key is invalid.' }
    $automaticCaptures = if ([uint64]$datasetRows[0].automatic_trigger_count -gt 0) { 1 } else { 0 }
    $archiveHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash

    $listed = @(Invoke-Checked $DogfoodTool @('list-truth', $archive))
    if ($listed.Count -lt 2 -or $listed[0] -cne
        "incident_key`tcreated_utc_ms`tsystem_samples`tprocess_samples" -or
        $listed[1] -notmatch "^$incidentKey`t") {
        throw 'Blinded incident listing is malformed.'
    }
    $truth = @(Invoke-Checked $DogfoodTool @('inspect-truth', $archive, $incidentKey))
    $truthText = $truth -join "`n"
    if ($truthText -notmatch '(?m)^ordinal\tpid\tname$' -or
        $truthText -match 'diagnosis=|contributors|resources|confidence=|context=|automatic_trigger') {
        throw 'Blinded truth inspection omitted ordinals or leaked prediction output.'
    }

    $safeReview = Join-Path $temporary 'truth-review-safe'
    [void](Invoke-Checked $DogfoodTool @(
        'export-truth', $archive, $incidentKey, $safeReview, 'ordinal-only'))
    $expectedReviewFiles = @(
        'ballot-template.tsv', 'manifest.ini', 'process-samples.tsv', 'processes.tsv',
        'review.html', 'system-events.tsv', 'system-samples.tsv')
    $actualReviewFiles = @(Get-ChildItem -LiteralPath $safeReview -File |
        Sort-Object Name | ForEach-Object Name)
    if (($actualReviewFiles -join '|') -cne ($expectedReviewFiles -join '|')) {
        throw 'Truth review does not contain the exact direct-v1 file set.'
    }
    $safeManifest = [IO.File]::ReadAllText((Join-Path $safeReview 'manifest.ini'))
    $safeProcesses = [IO.File]::ReadAllText((Join-Path $safeReview 'processes.tsv'))
    $safeHtml = [IO.File]::ReadAllText((Join-Path $safeReview 'review.html'))
    if ($safeManifest -notmatch '(?m)^format=1$' -or
        $safeManifest -notmatch '(?m)^prediction_free=1$' -or
        $safeManifest -notmatch '(?m)^local_process_identities=0$' -or
        $safeProcesses -match 'fixture\.exe|4294967295' -or
        $safeHtml -notmatch '<canvas id="system"' -or
        $safeHtml -notmatch 'const sys=' -or
        $safeHtml -match 'diagnosis=|confidence=|automatic_trigger|C:\\Fixture') {
        throw 'Ordinal-only truth review is malformed or leaked private/prediction data.'
    }
    $ballotPath = Join-Path $safeReview 'ballot-template.tsv'
    $ballotHeader = "incident_key`tannotator_id`tsymptom`tcertainty`tuser_visible`t" +
        "expected_diagnosis`texpected_contributor_ordinal`texpected_context`t" +
        "recurrence_family`tusefulness`n"
    $completedBallot = "$incidentKey`tannotator-1`tcpu_starvation`tprobable`t1`t" +
        "cpu_pressure`t0`tdesktop`tfixture-family`tuseful`n"
    [IO.File]::WriteAllText(
        $ballotPath, $ballotHeader + $completedBallot, [Text.UTF8Encoding]::new($false))
    $ballotStatus = @(Invoke-Checked $DogfoodTool @(
        'validate-ballot', $ballotPath, $incidentKey, 'operator-a')) -join "`n"
    if ($ballotStatus -notmatch '(?m)^ballot_valid=1$' -or
        $ballotStatus -notmatch '(?m)^prediction_free=1$' -or
        $ballotStatus -notmatch '(?m)^annotator_id=annotator-1$' -or
        $ballotStatus -match 'cpu_pressure|confidence=|contributors') {
        throw 'Completed-ballot validation is malformed or leaked prediction-bearing fields.'
    }
    try {
        [void](Invoke-Checked $DogfoodTool @(
            'validate-ballot', $ballotPath, $incidentKey, 'annotator-1'))
        throw 'Collection operator was accepted as the incident annotator.'
    } catch {
        if ($_.Exception.Message -eq
            'Collection operator was accepted as the incident annotator.') { throw }
    }
    try {
        [void](Invoke-Checked $DogfoodTool @(
            'validate-ballot', $ballotPath, (('b' * 32) -join ''), 'operator-a'))
        throw 'Completed ballot was accepted for the wrong incident.'
    } catch {
        if ($_.Exception.Message -eq
            'Completed ballot was accepted for the wrong incident.') { throw }
    }
    $secondBallotPath = Join-Path $temporary 'completed-ballot-2.tsv'
    $agreeingBallot = "$incidentKey`tannotator-2`tcpu_starvation`tprobable`t1`t" +
        "cpu_pressure`t0`tdesktop`tfixture-family`tuseful`n"
    [IO.File]::WriteAllText(
        $secondBallotPath, $ballotHeader + $agreeingBallot,
        [Text.UTF8Encoding]::new($false))
    $agreement = @(Invoke-Checked $DogfoodTool @(
        'compare-ballots', $ballotPath, $secondBallotPath,
        $incidentKey, 'operator-a')) -join "`n"
    if ($agreement -notmatch '(?m)^ballots_valid=1$' -or
        $agreement -notmatch '(?m)^prediction_free=1$' -or
        $agreement -notmatch '(?m)^annotator_count=2$' -or
        $agreement -notmatch '(?m)^disagreement=0$' -or
        $agreement -match 'cpu_pressure|confidence=|contributors') {
        throw 'Agreeing-ballot comparison is malformed or leaked ballot payload.'
    }
    $differingBallot = "$incidentKey`tannotator-2`tcpu_starvation`tprobable`t1`t" +
        "storage_pressure`t0`tdesktop`tfixture-family`tuseful`n"
    [IO.File]::WriteAllText(
        $secondBallotPath, $ballotHeader + $differingBallot,
        [Text.UTF8Encoding]::new($false))
    $disagreement = @(Invoke-Checked $DogfoodTool @(
        'compare-ballots', $ballotPath, $secondBallotPath,
        $incidentKey, 'operator-a')) -join "`n"
    if ($disagreement -notmatch '(?m)^disagreement=1$' -or
        $disagreement -match 'storage_pressure|cpu_pressure|confidence=|contributors') {
        throw 'Differing-ballot comparison did not emit only the disagreement bit.'
    }
    try {
        [void](Invoke-Checked $DogfoodTool @(
            'compare-ballots', $ballotPath, $ballotPath,
            $incidentKey, 'operator-a'))
        throw 'Duplicate annotator was accepted as independent ballot evidence.'
    } catch {
        if ($_.Exception.Message -eq
            'Duplicate annotator was accepted as independent ballot evidence.') { throw }
    }
    try {
        [void](Invoke-Checked $DogfoodTool @(
            'export-truth', $archive, $incidentKey, $safeReview, 'ordinal-only'))
        throw 'Occupied truth-review output was accepted.'
    } catch {
        if ($_.Exception.Message -eq 'Occupied truth-review output was accepted.') { throw }
    }

    $localReview = Join-Path $temporary 'truth-review-local'
    [void](Invoke-Checked $DogfoodTool @(
        'export-truth', $archive, $incidentKey, $localReview, 'include-local-identities'))
    $localManifest = [IO.File]::ReadAllText((Join-Path $localReview 'manifest.ini'))
    $localProcesses = [IO.File]::ReadAllText((Join-Path $localReview 'processes.tsv'))
    if ($localManifest -notmatch '(?m)^local_process_identities=1$' -or
        $localProcesses -notmatch 'fixture\.exe' -or
        $localProcesses -match 'C:\\Fixture') {
        throw 'Explicit local-identity truth review did not enforce its privacy contract.'
    }
    $invalidReview = Join-Path $temporary 'truth-review-invalid'
    try {
        [void](Invoke-Checked $DogfoodTool @(
            'export-truth', $archive, $incidentKey, $invalidReview, 'unsafe-default'))
        throw 'Invalid truth-review privacy mode was accepted.'
    } catch {
        if ($_.Exception.Message -eq 'Invalid truth-review privacy mode was accepted.') { throw }
    }
    if ([IO.Directory]::Exists($invalidReview) -or
        [IO.Directory]::Exists("$invalidReview.partial")) {
        throw 'Invalid truth-review request created output.'
    }

    $base = Join-Path $temporary 'corpus-000'
    [void](Invoke-Checked $DogfoodTool @('init', $base, 'cli-contract'))
    try {
        [void](Invoke-Checked $DogfoodTool @(
            'verify-evaluation', $base, $safeReview, 'none'))
        throw 'Collecting corpus was accepted for evaluation verification.'
    } catch {
        if ($_.Exception.Message -eq
            'Collecting corpus was accepted for evaluation verification.') { throw }
    }
    $campaignStatus = Join-Path $temporary 'campaign-status'
    [void](Invoke-Checked $DogfoodTool @('campaign-status', $base, $campaignStatus))
    $expectedCampaignFiles = @(
        'manifest.ini', 'profiles.tsv', 'status.html', 'summary.tsv',
        'symptoms.tsv', 'unmet.tsv')
    $actualCampaignFiles = @(Get-ChildItem -LiteralPath $campaignStatus -File |
        Sort-Object Name | ForEach-Object Name)
    if (($actualCampaignFiles -join '|') -cne ($expectedCampaignFiles -join '|')) {
        throw 'Campaign status does not contain the exact schema-v1 file set.'
    }
    $campaignManifest = [IO.File]::ReadAllText((Join-Path $campaignStatus 'manifest.ini'))
    $campaignHtml = [IO.File]::ReadAllText((Join-Path $campaignStatus 'status.html'))
    if ($campaignManifest -notmatch '(?m)^format=1$' -or
        $campaignManifest -notmatch '(?m)^prediction_free=1$' -or
        $campaignManifest -notmatch '(?m)^evidence_neutral=1$' -or
        $campaignManifest -notmatch '(?m)^qualification_ready=0$' -or
        $campaignHtml -notmatch 'Prediction-free, evidence-neutral status' -or
        $campaignHtml -match 'diagnosis=|confidence=|analyzer') {
        throw 'Campaign status is malformed or leaked prediction output.'
    }
    try {
        [void](Invoke-Checked $DogfoodTool @('campaign-status', $base, $campaignStatus))
        throw 'Occupied campaign-status output was accepted.'
    } catch {
        if ($_.Exception.Message -eq 'Occupied campaign-status output was accepted.') { throw }
    }
    if ([IO.Directory]::Exists("$campaignStatus.partial")) {
        throw 'Campaign-status publication left staging residue.'
    }

    $unconsentedPacket = Join-Path $temporary 'unconsented-packet'
    [void](Invoke-Checked $DogfoodTool @('init-session', $base, $unconsentedPacket))
    Append-Utf8 (Join-Path $unconsentedPacket 'hardware.tsv') "host-a`twindows`twin11-current`tamd-zen3`t12`t32-63`tnvidia`tbalanced`n"
    Append-Utf8 (Join-Path $unconsentedPacket 'sessions.tsv') "unconsented-session`thost-a`toperator-a`tcalibration`tquiet`tquiet`t3600`t0`t0`t0`n"
    try {
        [void](Invoke-Checked $DogfoodTool @('validate', $unconsentedPacket))
        throw 'An unconsented session packet was accepted.'
    } catch {
        if ($_.Exception.Message -eq 'An unconsented session packet was accepted.') { throw }
        if ($_.Exception.Message -notmatch 'consent_attested=1') {
            throw 'Unconsented packet rejection did not identify the required attestation.'
        }
    }

    $quietPacket = Join-Path $temporary 'quiet-packet'
    [void](Invoke-Checked $DogfoodTool @('init-session', $base, $quietPacket))
    Append-Utf8 (Join-Path $quietPacket 'hardware.tsv') "host-a`twindows`twin11-current`tamd-zen3`t12`t32-63`tnvidia`tbalanced`n"
    Append-Utf8 (Join-Path $quietPacket 'sessions.tsv') "quiet-session`thost-a`toperator-a`tcalibration`tquiet`tquiet`t3600`t0`t0`t1`n"
    [void](Invoke-Checked $DogfoodTool @('validate', $quietPacket))
    $quietCorpus = Join-Path $temporary 'corpus-001'
    [void](Invoke-Checked $DogfoodTool @(
        'merge-session', $base, $quietPacket, 'none', $quietCorpus))

    $naturalPacket = Join-Path $temporary 'natural-packet'
    [void](Invoke-Checked $DogfoodTool @('init-session', $quietCorpus, $naturalPacket))
    Append-Utf8 (Join-Path $naturalPacket 'hardware.tsv') "host-a`twindows`twin11-current`tamd-zen3`t12`t32-63`tnvidia`tbalanced`n"
    Append-Utf8 (Join-Path $naturalPacket 'sessions.tsv') "natural-session`thost-a`toperator-a`tcalibration`tnatural`tcpu_starvation`t60`t1`t$automaticCaptures`t1`n"
    Append-Utf8 (Join-Path $naturalPacket 'incidents.tsv') "$incidentKey`tnatural-session`tcalibration`tcpu_starvation`tprobable`t1`tcpu_pressure`t`tdesktop`tfixture-family`t1`tuseful`t2`t0`n"
    foreach ($annotator in @('annotator-1', 'annotator-2')) {
        Append-Utf8 (Join-Path $naturalPacket 'annotations.tsv') "$incidentKey`t$annotator`tcpu_starvation`tprobable`t1`tcpu_pressure`t`tdesktop`tfixture-family`tuseful`n"
    }
    [void](Invoke-Checked $DogfoodTool @('validate', $naturalPacket))
    $merged = Join-Path $temporary 'corpus-002'
    [void](Invoke-Checked $DogfoodTool @(
        'merge-session', $quietCorpus, $naturalPacket, $archive, $merged))
    $validation = @(Invoke-Checked $DogfoodTool @('validate', $merged)) -join "`n"
    if ($validation -notmatch 'with 1 hardware profiles, 2 sessions, 1 incidents, and 2 annotation ballots') {
        throw 'Merged acquisition corpus does not contain the expected exact rows.'
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -cne $archiveHash) {
        throw 'Read-only acquisition commands modified the archive evidence.'
    }
    if (@(Get-Content -LiteralPath (Join-Path $base 'sessions.tsv')).Count -ne 1) {
        throw 'Session merge modified its base corpus.'
    }

    $shouldNotExist = Join-Path $temporary 'missing.sqlite3'
    try {
        [void](Invoke-Checked $DogfoodTool @('list-truth', $shouldNotExist))
        throw 'Missing read-only archive was accepted.'
    } catch {
        if ($_.Exception.Message -eq 'Missing read-only archive was accepted.') { throw }
    }
    if ([IO.File]::Exists($shouldNotExist)) {
        throw 'Read-only archive access created a missing database.'
    }
    Write-Output 'Dogfood acquisition CLI contracts passed.'
} finally {
    if ([IO.Directory]::Exists($temporary)) {
        Remove-Item -LiteralPath $temporary -Recurse -Force
    }
}
