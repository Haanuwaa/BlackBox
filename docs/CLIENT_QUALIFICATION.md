# Clean-client and physical qualification

The V0.17 client gate qualifies the portable ZIP that would be released, not a binary left in a
developer build tree. BlackBox uses direct-format-v1 evidence with no migration or compatibility
reader. A local automated smoke is useful wiring evidence, but it cannot represent a clean-client,
accessibility, display, battery, or signing pass.

## Package smoke

Build the current Release configuration, run CPack from its build directory, generate the checksum,
and choose a new evidence destination:

```powershell
cmake --build out/build/windows-vs2026-release --config Release
Push-Location out/build/windows-vs2026-release
cpack --config CPackConfig.cmake -C Release
Pop-Location
./scripts/write-release-checksum.ps1 `
-PackagePath ./out/build/windows-vs2026-release/BlackBox-0.24.0-windows-x64.zip
./scripts/run-client-qualification.ps1 `
-PackagePath ./out/build/windows-vs2026-release/BlackBox-0.24.0-windows-x64.zip `
  -OutputDirectory ./out/client-smoke-<revision> -Mode smoke
```

The runner first copies the ZIP and sidecar into fresh staging and verifies that exact copy. The
package verifier requires one exact package-named ZIP root, safe non-duplicated paths, bounded entry
counts and sizes, the three shipped executables, release/user documentation, and one exact
filename-bound SHA-256 sidecar. `-RequireAuthenticode` additionally requires every shipped
executable to have a valid trusted Authenticode signature and timestamp. Signing only the desktop
executable is not sufficient.

Smoke mode refuses to start while any other `blackbox.exe` process is running, extracts the verified
ZIP into a fresh staging directory, redirects both settings files and the schema-v1 archive, launches
the packaged desktop executable for a bounded hidden diagnostic, checks collection/writer/archive
health and the 80 MiB working-set gate, and binds the ZIP, launched application, qualification
runner, evidence verifier, and every executable hash/signature state. It records
`single_host_profile_satisfied=0`,
`clean_client_matrix_satisfied=0`, and `physical_matrix_satisfied=0` by construction.

The runner invokes the verifier before publication. Re-run it independently on the immutable final
directory before relying on the bundle:

```powershell
./scripts/verify-client-evidence.ps1 `
  -CampaignDirectory ./out/client-smoke-<revision>
```

For an official interactive bundle, add `-RequireInteractive -RequireAuthenticode`. Never pass
`-AllowStaging` during independent review; that switch exists only for the runner's pre-publication
check of its own `.partial` directory. The package-smoke example above deliberately names the
current `0.24.0` engineering build. Final release evidence must instead use the exact signed
`BlackBox-1.0.0-windows-x64.zip`; no prerelease filename can satisfy the V1 composition gate.

## Interactive profiles

Run these only as an ordinary user on clean x64 client installations. No other BlackBox instance
may be running. Interactive evidence requires the exact lowercase 40-character source revision
used to build the package:

```powershell
./scripts/run-client-qualification.ps1 `
  -PackagePath C:/qualification/BlackBox-1.0.0-windows-x64.zip `
  -OutputDirectory C:/qualification/win11-standard-<revision> `
  -Mode interactive -Profile standard -SourceRevision <40-hex-revision>
```

Before extraction or launch, the runner passes that revision to the strict package verifier. All
three packaged executables must expose the same embedded source identity, so an operator-supplied
revision cannot relabel a package built from another commit. This check happens before any physical
case is recorded.

The runner launches the extracted application visibly and prints its `.partial` campaign path.
Perform one required observation at a time from a second PowerShell window and record only what was
actually observed:

```powershell
./scripts/record-client-case.ps1 `
  -CampaignDirectory C:/qualification/win11-standard-<revision>.partial `
  -Case tray_hide_restore -Result pass
```

The helper accepts only a case required by that running profile, refuses duplicates, verifies that
the packaged process is still alive **and has the exact packaged application hash**, and stores only
UTC time, case identifier, and pass/fail. A failed case is evidence of failure and must not be
changed to pass; fix the defect and run a new campaign. After every case is recorded, exit BlackBox
using the tray menu. The runner requires an orderly zero exit and then runs the same isolated package
diagnostic as smoke mode before it can publish.

Profiles have non-overlapping purposes:

- `standard`: ordinary-user launch, keyboard-only first-run onboarding, focus visibility under
  increased text/display scaling, tray restore, hotkey/capture/viewer/settings/diagnostics,
  keyboard-only use, live and hidden high-contrast transitions, and 100/125/150/200% scaling;
- `multimonitor`: mixed-scale movement, taskbar/work-area changes, monitor disconnect/reconnect, and
  suspend/resume; at least two active displays are mechanically required;
- `low-end`: responsiveness and resource bounds on a host with at most 8 GiB RAM or at most four
  logical processors;
- `battery`: battery operation, battery saver, balanced/performance modes, and battery
  suspend/resume; a detected battery is mechanically required.

The interactive `standard` profile deliberately launches with onboarding incomplete. Record
`first_run_onboarding_keyboard` only after completing the modal without a pointer: traverse every
focus target with Tab/Shift+Tab, scroll its bounded content, and activate the final action with the
keyboard. Record `focus_visibility_text_scaling` only after confirming visible focus, legible text,
and reachable controls at increased text size and each required display scale. These operator cases
add evidence obligations; they do not make raster output or one development host physical proof.

Host evidence deliberately excludes computer name, username, serial numbers, device IDs, local
paths, and operator notes. It records OS family/build/edition, architecture, CPU/GPU, memory and
logical processors, process count, power state/scheme, accessibility preferences, and bounded
display bounds/work areas/DPI. Windows family is derived from the kernel build because Windows can
report a legacy product-name string on newer clients.

## Matrix verification

Retain at least five independent interactive bundles for the exact same ZIP and source revision:

- a `standard` pass on clean Windows 10 22H2;
- a `standard` pass on supported Windows 11;
- one `multimonitor` pass;
- one qualifying `low-end` pass;
- one qualifying `battery` pass.

Then aggregate them without editing any bundle:

```powershell
./scripts/verify-client-matrix.ps1 `
  -EvidenceDirectory C:/qualification/win10-standard, `
                     C:/qualification/win11-standard, `
                     C:/qualification/multimonitor, `
                     C:/qualification/low-end, `
                     C:/qualification/battery `
  -OutputDirectory C:/qualification/client-matrix-<revision> `
  -RequireAuthenticode
```

The matrix invokes the independent single-bundle verifier for every input. That verifier rejects
partial output by default, links, extra files or even extra empty directories, oversized inputs,
locale-dependent or malformed process rows, and any mismatch between the copied package, launched
application, runner/verifier provenance, summary, campaign, or manifest. It streams a bounded ZIP
header check rather than loading the archive into memory, re-verifies each embedded ZIP and checksum,
validates the direct-v1 settings/report and SQLite signature, and checks every exact operator case.
The matrix then rejects reuse of one bundle and requires common package/revision identity. It
publishes only after every coverage rule passes. Omit `-RequireAuthenticode` for a pre-signing
rehearsal; only the signed invocation can produce `official_signed_matrix_satisfied=1`.

Retain every source bundle. Independently recheck a published matrix by regenerating it from those
same bundles and requiring exact byte-for-byte agreement:

```powershell
./scripts/verify-client-matrix-evidence.ps1 `
  -MatrixDirectory C:/qualification/client-matrix-<revision> `
  -EvidenceDirectory C:/qualification/win10-standard, `
                     C:/qualification/win11-standard, `
                     C:/qualification/multimonitor, `
                     C:/qualification/low-end, `
                     C:/qualification/battery `
  -ExpectedSourceRevision <40-hex-revision> -RequireAuthenticode
```

A passed one-host bundle contains the ZIP and sidecar, `app-report.ini`, `campaign.ini`, `host.ini`,
the isolated `data/` files, case/process journals, `summary.ini`, and `manifest.sha256.ini`.
Destinations are immutable: existing final or `.partial` paths are refused, failures remain
`.partial`, and success uses one same-volume directory rename. Do not rename, repair, or reuse failed
evidence.
