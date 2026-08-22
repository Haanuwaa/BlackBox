# Representative incident fixtures

`blackbox_fixture_generator` creates a deterministic schema-current SQLite archive for tests,
viewer development, screenshots, and current-schema checks. It writes three annotated incidents:

| Fixture | Shape | Purpose |
|---|---:|---|
| Unavailable metrics | 2 system rows, 1 process row | Explicit unsupported, inaccessible, and temporary states |
| Typical workstation | 150 frames x 50 processes | Default 120-second pre/30-second post window at ordinary scale |
| High process scale | 150 frames x 500 processes | Pagination, plotting, storage, and memory stress |

From a configured build, run:

```powershell
.\out\build\windows-msvc-release\tests\Release\blackbox_fixture_generator.exe `
  .\out\fixtures\representative-incidents.sqlite3
```

The destination is replaced intentionally. Generated databases are build artifacts rather than
source artifacts, preventing committed SQLite files from silently drifting behind the schema.
The generator verifies the incident count and database size before succeeding. CTest invokes it
on every storage-enabled validation run.
