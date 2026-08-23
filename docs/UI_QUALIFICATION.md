# UI qualification

BlackBox uses deterministic software raster evidence to catch native UI rendering regressions before
the physical Windows client matrix. This is a development qualification aid, not a substitute for
testing the packaged application on real displays and assistive configurations.

The product UI uses one code-native semantic visual system: restrained navy surfaces, explicit
text/accent/success/warning roles, consistent padding and rounding, a system-native UI font when one
is available, and a reversible maximum-contrast palette. Live leads with recording state and one
primary capture action; incident browsing, explanation/annotation, collection profiles, capture
preferences, archive maintenance, and permanent removal are grouped into bordered surfaces. The
deterministic raster runner intentionally keeps its bundled Basic Latin font so screenshots remain
host-independent; production font loading has an explicit fallback and does not alter evidence text.

## Generate the direct-v1 evidence bundle

Build the Release tests, then choose a new destination that does not already exist:

```powershell
cmake --build out/build/windows-vs2026-release --config Release --target blackbox_tests
.\scripts\run-ui-qualification.ps1 `
  -OutputDirectory .\out\ui-qualification-raster-v1-<revision> `
  -SourceRevision <40-hex-revision>
```

The runner refuses an existing final or `.partial` destination. It renders representative and
large incident fixtures through the SDL3 software renderer at these two modes:

- 1100 x 700 logical/physical pixels at 100%, using the normal dark palette
- 1100 x 700 logical pixels on a 1650 x 1050 physical surface at 150%, using the high-contrast
  palette

Every Live, Incidents, Detail, Patterns, Settings, and Diagnostics page is rendered in both modes
for both fixtures. Four additional scrolled Detail cases pin a sticky cursor at -15 seconds and
show it beside the independent incident marker across a lower synchronized timeline and the process
table. Two representative Detail cases explicitly expose the feedback reset, one-step rollback,
and preceding/marker-spanning/post-marker contributor roles with their attribution controls. A
passing bundle contains exactly 30 BMP files, `summary.ini`, and `manifest.sha256.ini`.
The runner and independent verifier validate every filename, exact V5 BMP structure/payload,
dimension, SHA-256 digest, summary hash, source revision, and test/runner/verifier identity before one
same-volume rename publishes the directory. Re-run `verify-ui-qualification.ps1` on the immutable
result. A failed or interrupted attempt remains visibly
`.partial`. `format=1` is the only pre-release evidence format; there is no migration or
compatibility reader.

The Release test executable is compiled with the configure-time source revision. When evidence
output is enabled it compares that identity with `-SourceRevision` before rendering the first case;
a caller cannot relabel a raster generator built from another tree. `local-uncommitted` remains
available only for explicit rehearsal output.

`summary.ini` deliberately records:

```ini
manual_visual_review_required=1
physical_matrix_satisfied=0
```

Automated generation therefore cannot close the V0.17 physical qualification gate.

## Visual review

Review all 30 images at native pixel size. Record the bundle manifest hash and any finding outside
the bundle; do not edit generated evidence. Verify:

- the complete application viewport is rasterized, with no unused high-DPI canvas caused by an
  incorrect renderer scale;
- the six navigation targets remain visible and the active page is distinguishable;
- every Live status value, including the complete product version, remains visible rather than
  collapsing to a one-character column;
- Live leads with an unambiguous recording/capture state and its single primary capture action;
  technical status, forensic telemetry, rolling history, and active-process disclosure controls
  remain visible and keyboard reachable without making their dense contents the default view;
- text, controls, tables, plot regions, the incident marker, and timeline sections are not clipped
  or overlapped at the top, left, or right edges;
- the cyan synchronized cursor remains distinct from the orange incident marker at both scales,
  and wide PIDs or large metric values do not collide with adjacent process-table columns;
- feedback calibration status, confirmed-similar historical context, reset confirmation/action,
  rollback action, all three temporal-role labels, contributor-attribution selectors, raw/adjusted
  scores, and calibration state remain legible at 100% dark and 150% high contrast;
- long large-fixture content remains scrollable rather than silently truncated;
- high-contrast text, disabled text, selection, buttons, borders, accents, and warnings remain
  distinguishable without color being the only state cue;
- switching Windows high contrast on and off while the dashboard is open updates every semantic
  control color within one second and restores the normal palette without restarting; the Live page
  also reports the current system-animation preference;
- user-visible punctuation renders as a real glyph rather than `?`; the bundled default ImGui font
  is Basic Latin, so the UI source is intentionally ASCII-only and a regression test enforces that
  boundary;
- incident symptom, contributor, uncertainty, capture provenance, feedback controls, and evidence
  caveats remain readable in Detail; the collapsed evidence control follows the headline and at
  most three contributor previews without implying that hidden raw evidence was discarded;
- Settings and Diagnostics expose recovery/status information without hidden or ambiguous actions.

The physical walkthrough must additionally exercise first-run completion and the Incidents loading,
empty, no-match, and archive-unavailable presentations. Each state must name a safe next action,
preserve keyboard focus visibility, and avoid claiming that unsaved or filtered evidence was deleted.

Raster hashes are expected to change when rendering, fonts, layout, or fixture content changes.
Review the new images and retain a new complete bundle instead of blessing individual changed hashes.
After all 30 pass, use `record-ui-visual-review.ps1 -ConfirmAllCasesPassed` and independently run
`verify-ui-visual-review.ps1`. The separate review directory binds the exact raster manifest and
cannot exist without explicit confirmation. `V017_RELEASE_EVIDENCE.md` contains the full commands.

## Physical Windows matrix

The release revision still requires clean, ordinary-user Windows 10 22H2 and supported Windows 11
hosts. At minimum record OS build, BlackBox/package hash, CPU/GPU, display topology and work areas,
per-display scale, resolution, power source/mode, keyboard/accessibility settings, and result for:

- 100%, 125%, 150%, and 200% display scaling, including moving the window between monitors with
  different scales and work areas;
- keyboard-only navigation, focus visibility, tab order, activation, scrolling, dialogs, and global
  capture hotkey behavior;
- Windows high contrast and increased text/scale configurations;
- repeated live standard/high-contrast transitions, including changing the preference while the app
  is hidden and then reopening it from the tray;
- single- and multi-monitor placement, taskbar/work-area changes, disconnect/reconnect, and resume;
- low-end hardware, battery operation, battery saver, balanced, and performance power modes;
- packaged hidden startup, tray recovery, capture, incident inspection, settings, diagnostics, and
  orderly exit.

Record defects and rerun the affected cases on the same source revision after correction. Software
raster success proves deterministic page output only; it does not prove Windows DPI notifications,
font quality, input accessibility, real GPU behavior, monitor transitions, battery impact, or usable
layout on the supported client matrix. `CLIENT_QUALIFICATION.md` defines the immutable package-bound
interactive profiles and aggregate verifier used to retain that evidence.
