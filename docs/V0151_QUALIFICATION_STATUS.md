# V0.15.1 diagnostic reliability qualification status

Status date: 2026-08-22. The implementation and qualification machinery are complete; the
multi-machine evidence collection and one-shot held-out result are not.

## Frozen implementation provenance

- Intelligent pipeline: 13
- Diagnosis evidence model: 12
- Default configuration fingerprint: `6701770989141957614`
- Corpus and evaluation artifacts: direct format version 1 only
- Runtime inference: local statistical analysis; native ML not adopted

## Implemented reliability changes

- Resource evidence must clear both robust statistical deviation and a unit-specific practical
  effect floor. The raw statistical score remains inspectable even when practical pressure is zero.
- Observed pressure is separate from symptom explanation in analysis results, the viewer, dashboard
  wording, and privacy-safe evaluation exports. Nearby pressure alone leaves the explanation
  `Unknown`.
- Resource explanations require alignment with an automatic capture, direct disk/network quality
  degradation, or strong preceding process activity. Multi-resource wording requires alignment for
  both resources.
- Windows Application Hang event 1002 can request capture through the existing bounded coordinator
  and supports a direct application-hang explanation. Frame-pacing and audio-glitch automatic
  capture remain visibly unsupported; endpoint changes and DPC/ISR activity are context only.
- Future-only Application Error event 1000 is retained without application/module names, exception
  codes, fault paths, message, or payload. It can request bounded capture and supports only the
  exact Windows-reported crash symptom, never the defect or another root-cause attribution.
- Future-only DNS Client event 1014 is retained without its hostname/message/payload and can support
  only a five-second-aligned Windows-reported timeout symptom. It does not claim a cause and cannot
  request automatic capture.
- Future-only Display event 4101 is retained without its message/driver/adapter/payload and can
  request capture through the same bounded coordinator. It supports only the exact Windows-reported
  timeout-recovery symptom, never a driver, application, GPU, or root-cause attribution.
- Future-only provider `disk` event 153 is retained without its LBA/device/PDO/message/payload and
  can request a disk-scoped capture through the bounded coordinator. It supports only the exact
  Windows-reported I/O-retry symptom, never a device, controller, driver, media, firmware,
  application, hardware-failure, or other root-cause attribution.
- The direct-v1 corpus records session-operator pseudonyms and separate bounded annotation ballots.
  Ballots must be distinct, cannot belong to the session operator, and mechanically determine the
  truth-row count and disagreement flag.
- The direct-v1 truth vocabulary now treats `application_crash` as its own ninth symptom rather than
  forcing a Windows-reported crash into `application_hang` or `ambiguous`. Qualification coverage,
  campaign status, evaluation counts, ballots, and the acquisition helper share one compile-time
  canonical class count. This is a prerelease V1 definition change with no migration or legacy
  reader; prior development corpora must be recollected or edited as current direct V1 before use.
- The corpus freezer requires the same three or more hardware profiles to contribute natural,
  per-split quiet, and scorable evidence to calibration and held-out data, ten aggregate quiet
  exposure hours, full per-split symptom coverage, and an explicit V0.15 incident table whose
  held-out keys may not be reused. The readiness command reports every missing denominator.
- Multi-hardware evaluation consumes an explicit local profile-to-archive map. It rejects missing
  profiles, shared archive paths, duplicate incident keys, and incidents stored under a profile
  different from the linked session rather than pretending one database proves a distributed run.
- The one-shot evaluator requires nonzero denominators and at least 80% supported-diagnosis
  precision, 60% supported recall, 90% Unknown-truth abstention, and 70% contributor top-3. Failure
  is written, locked, and returned as a failing command.
- Held-out evaluation now acquires an exclusive attempt directory before analysis. It binds the
  calibration fingerprint, retains a visible running state after crashes, records the complete
  report fingerprint and pass/fail result, and refuses concurrent/repeated attempts. Calibration
  and report directories publish only through complete sibling staging and atomic rename.
- The held-out report/prediction pair is independently reparsed, recomputed from frozen truth, and
  required to match canonical bytes both before and after publication. Missing predictions remain
  failures in every truth-based denominator, and the exact V1 report publishes explicit rate
  numerators/denominators, hardware/symptom/exposure counts, calibration bins, and recurrence counts.
- Confidence calibration has one bounded offline direct-V1 codec. It rejects linked/non-regular,
  oversized, CRLF, reordered, numerically noncanonical, nonmonotonic, sample-inconsistent, and
  assertion-inconsistent input; creation refuses occupied paths, publishes atomically, and is
  exactly reloaded after both its file rename and the outer evaluation-directory rename. There is no
  permissive app-local or older calibration reader.

## Validation completed

- Full Release: the current graph passed 311/311 tests with its scoped Windows process and
  current-user registry integrations enabled. This includes both shell concurrency/efficiency
  regressions, wall-clock semantic tamper rejection, repository hygiene, all five executable version
  resources, and the architecture/direct-V1/release-evidence contracts. The rebuilt application is
  SHA-256 `57ac61b36dd10e8d7e5741a006135c50e8df5d5fd8c0b3b5b8959d63d1d79d69`.
- Full Debug: 310/311 tests passed in the restricted environment and the sole HKCU launch-at-login
  integration passed 1/1 with current-user registry access, completing the effective 311/311 graph.
- The ninth-symptom alignment passed its focused evaluation/acquisition/direct-V1 set 14/14. Its
  earlier complete Debug run retained every then-registered test; the current total above supersedes
  that historical graph count.
- Reduced Windows headless Release: 126/127 tests passed in the restricted environment and the HKCU
  integration passed 1/1 with registry access, completing the effective 127/127 graph.
- Full-app analysis-disabled Release: 195/196 tests passed in the restricted environment and the
  HKCU integration passed 1/1 with registry access, completing the effective 196/196 graph.
- Windows AddressSanitizer: 309/310 applicable tests passed in the restricted environment and the
  HKCU integration passed 1/1 with registry access, completing the effective 310/310 applicable
  graph from 311 registered tests. The unhandled-exception probe is intentionally excluded by the
  official preset because its required access violation is the test input.
- MSVC native analysis completed with warnings-as-errors across the app and unified test graph.
- Architecture boundary checks pass in every applicable graph.
- The eleven-case Windows event-provider benchmark observed 0.000000% total-machine CPU for the
  isolated provider `disk` event 153 subscription and a 0.128763% maximum on the Windows Update case. These are
  controlled one-host measurements, not a population overhead claim.
- The assembled Release dogfood CLI created the five-file direct-v1 corpus and reported every empty
  campaign deficit; `readiness` returned the specified incomplete exit code 3.
- The assembled Release CLI reported a new corpus as `state=not_started`; deterministic
  race tests prove exactly one concurrent attempt acquires the lock, and crash-state/publication
  tests prove incomplete artifacts retain `.partial` rather than appearing final.
- The one-incident natural-session helper passed its real CLI contract: two independent ballots,
  fixed consensus, archive-key/automatic-capture proof, atomic packet publication, and byte-identical
  base/archive evidence. This proves acquisition mechanics only, not a collected natural session.
- The pipeline-thirteen fingerprint includes conservative local feedback calibration,
  source-timing-gated contributor attribution, and
  confirmed-similar-incident policy.
  Therefore no earlier calibration or held-out artifact can qualify the current analyzer. The
  corpus format remains direct v1; its canonical symptom vocabulary was tightened in place before
  evidence collection, so no earlier development corpus or report can qualify it.

## Evidence still required

Do not mark V0.15.1 complete until a genuinely new corpus is collected on at least three hardware
profiles, independently annotated, frozen against the V0.15 held-out incident table, calibrated,
and evaluated once against pipeline thirteen. The one-shot report must pass every predeclared floor.
Controlled workloads and the consumed V0.15 held-out set cannot substitute for this evidence. A
locally executable V0.16 safety work now suppresses repeated exact-signature false positives and
shows bounded confirmed-similar historical context, but it is not symptom classification and does
not satisfy or bypass this qualification gate.
