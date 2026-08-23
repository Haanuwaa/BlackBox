# Logging

BlackBox uses the small replaceable logger in `core`; runtime modules do not depend on a logging
framework. The default sink writes one record per physical line:

```text
[blackbox +125ms] [info] [telemetry] Provider sampling recovered
```

The elapsed timestamp is relative to process start and is diagnostic ordering evidence, not wall-clock
time. Components are bounded to 32 bytes and messages to 2,048 bytes. Leading/trailing whitespace is
removed, internal whitespace is collapsed, control bytes are replaced, and truncated fields end in
`...`. Warning and error records flush immediately.

Custom sinks receive `(level, component, message)` after normalization. The logger copies the active
sink while holding its configuration lock and invokes it after releasing the lock, so adapters may
reenter or replace the sink without deadlocking. Sinks must not retain either `string_view`; both views
refer to bounded call-local storage. Logging remains transition-oriented on collection paths so a
persistent provider failure does not produce one record per sample.
