# Storage module

V0.0.8 consumes immutable incident snapshots on a dedicated bounded writer path and persists them transactionally in SQLite. The module depends only on core contracts; it neither polls telemetry nor blocks the collector. See `docs/STORAGE.md` for schema, durability, size, pre-release reset, and recovery policy.
