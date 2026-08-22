# Tools

`blackbox_dogfood_tool` creates, validates, freezes, inspects, calibrates, and evaluates the V0.15
local diagnostic corpus described in `docs/DOGFOOD_PROTOCOL.md`. It is offline-only and never runs
inside the recorder. `blackbox_dogfood_capture` is a Windows development target that records a real
provider/normalizer/recorder incident around an optional child workload. Controlled CPU, disk,
network-reset, hung-window, and frame-stall modes are supplied by the test-only
`blackbox_dogfood_workload` target; their limitations must remain attached to any result.

Repository-local profiling, benchmark, maintenance, and development utilities will be added here as their milestones require them. Shipped application functionality must not depend on Python or Node.js.
