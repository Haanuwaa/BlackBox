# V0.3 personalized process baselines

Personalization lets BlackBox distinguish “unusual for this executable” from merely high absolute
or incident-local activity. It remains deterministic statistical analysis: there is no model,
training runtime, background learner, or collection-path dependency.

## Learning and scoring policy

When an archived incident is opened, the viewer worker loads prior observations for executable
identities present in that incident. The personalized analyzer first runs the V0.2 incident-local
analysis, then compares each process candidate's strongest evaluation-window CPU fraction, working
set bytes, disk-read rate, and disk-write rate with that executable's prior incident observations.

Eight available prior observations are required for a metric. With enough history, the executable
profile's median/MAD/IQR robust score replaces the incident-local score for that metric. This is
what allows repeated compiler-like CPU activity to become normal for the compiler while the same
CPU change remains anomalous for an unrelated executable. Metrics without enough history retain
their incident-local evidence, and the process explicitly reports profile cold start. Confidence
is low during cold start, moderate from 8 observations, and high from 30 observations.

After scoring, one observation per executable and incident is stored. Multiple process instances
with the same executable identity contribute the maximum evaluated value per metric. The
`(executable_key, incident_id)` primary key makes reopening an incident idempotent. An incident is
never included in its own baseline, and older incidents cannot use observations from newer ones.

## Identity policy

Identity is normalized without filesystem access so analysis remains portable and reproducible:

1. An available executable path is preferred. ASCII letters are lowercased, `/` becomes `\`,
   repeated separators and a Windows `\\?\` prefix are removed, and trailing separators are
   trimmed. The key is namespaced as `path:`.
2. If the path is inaccessible, unsupported, or empty, an available process name is trimmed,
   ASCII-lowercased, and namespaced as `name:`. Path and name namespaces never merge.
3. If neither value is usable, that process remains incident-local and no profile is learned.
4. Keys longer than 2,048 bytes are not personalized rather than truncated or ambiguously hashed.

A rename or path move creates a cold new identity. An in-place upgrade at the same normalized path
shares history. V0.3 does not collect or use file hashes, versions, publishers, or signatures, so
it cannot distinguish two binaries that replace one another at the same path. Non-ASCII case
folding is intentionally not attempted without a platform/filesystem identity service.

## Aging and bounds

Only observations from the preceding 30 days are eligible. Each executable retains its newest 64
incident observations. SQLite retains at most 2,048 executable identities; inserting a new one at
the cap evicts the least-recently-seen key, with lexical key order as a deterministic tie-breaker.
At most 512 keys are queried or updated for one incident. These limits bound profile storage to
131,072 observation rows plus 2,048 identity rows, independent of recorder uptime or archive size.

The profile tables share the incident archive's transaction, WAL, busy timeout, logical size cap,
and error categories. Profile query/update failures degrade only personalization for that viewer
result; incident-local analysis and recording continue. Learning happens only on explicit incident
view, never during normal rolling telemetry.

## Interpretation and poisoning risk

Profiles describe recurrence, not correctness. Repeated faults can eventually become statistically
normal, and shared paths can combine behavior from replaced binaries. The UI therefore labels the
baseline scope and continues to state that scores are correlation, not proven causation. The
30-day/64-observation aging window limits stale influence but does not solve poisoned baselines;
future feedback and executable-signature policy must be evaluated before stronger claims.
