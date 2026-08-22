# Recovery runbooks

These procedures preserve evidence first. Stop BlackBox before manually copying or moving its active
files. Never rename an incompatible archive into the active location, edit SQLite/WAL files, or
delete a `.partial` artifact until its role is understood.

## A capture could not be saved

1. Keep BlackBox running; collection continues independently of storage.
2. Open **Settings** and check the writer status and the bounded recoverable-capture message.
3. If another safety copy is needed, export the failed incident to a new standalone SQLite file.
4. Correct the underlying issue (free space, permissions, read-only media, or another process holding
   the archive), then choose **Retry failed incident**.
5. If retry fails again, retain the exported standalone copy and create a support bundle. The one-slot
   recovery policy remains visible through the writer status.

## Archive locked, read-only, or full

1. Pause new reproduction work, but do not purge or replace anything.
2. Exit other tools that may hold the SQLite archive, its `-wal`, or `-shm` files.
3. Check that the archive directory is writable and has space beyond the configured logical limit.
4. Use **Refresh archive health**. If healthy, retry the recoverable incident.
5. If capacity is the intended limit, create a verified backup first, then use explicit retention to
   keep a confirmed number of newest incidents. BlackBox never deletes incidents automatically.

## Suspected corrupt archive

1. Exit BlackBox so the writer and viewer release the archive.
2. Copy the archive, `-wal`, and `-shm` files together to a safe location without modifying them.
3. Restart BlackBox and use health status only; do not allow any tool to “repair” the sole copy.
4. Restore only from a known verified BlackBox v1 backup. The UI validates identity, integrity, and
   the complete canonical table/index layout through a read-only source connection, and requires a
   new pre-restore safety-backup path.
5. Non-v1 or unversioned nonempty archives are rejected unchanged. This prerelease has no migrations
   or legacy readers. Preserve the rejected file, then explicitly select a new empty v1 archive path.

## Invalid settings or startup failure

1. Record the exact visible error and create a support bundle if the Diagnostics page is reachable.
2. Exit BlackBox and copy `product-settings.ini` and `recorder-settings.ini` privately; they are not
   included in a default support bundle.
3. An invalid file is not partially applied. Move it aside explicitly and restart to create/use direct
   v1 defaults. Do not rewrite its version field or attempt a migration.
4. If the app reports an existing instance, activate the tray instance before assuming startup failed.
5. If startup repeatedly fails, preserve completed `.dmp` files and the original settings/archive for
   investigation; do not send them without reviewing their sensitivity.

## BlackBox crashed

1. Restart BlackBox and open **Diagnostics**. Confirm whether completed local crash evidence exists.
2. Create a support bundle without the dump first; it contains safe bounded counters only.
3. Include the latest dump only if the recipient needs it and the explicit memory/path disclosure is
   acceptable. The bundle remains local until the user separately shares it.
4. Preserve the original `.dmp`; bundle creation copies rather than moves it. Completed dumps are not
   automatically deleted.
5. A `.dmp.partial` indicates publication did not complete. Do not rename it blindly. Preserve it for
   engineering inspection; only a completed `.dmp` is offered by the UI.

## Support bundle failed

1. Use a new absolute destination whose parent directory already exists.
2. Confirm neither the final directory nor `<destination>.partial` already exists.
3. Check free space and permissions. If raw-dump inclusion was selected, verify that the source remains
   a completed regular dump no larger than 64 MiB.
4. Preserve a partial directory if diagnosing a write failure. After inspection, remove or move that
   exact directory explicitly and retry with a new destination.
5. Never treat `.partial` as a completed bundle; a complete direct-v1 bundle has exactly the files
   declared in `manifest.ini`.

## Restore and backup verification

1. Create backups through **Create verified backup** to a new file; overwrite is refused.
2. Store the backup away from the active archive and record the application version that created it.
3. Before restore, choose a different new path for the pre-restore safety backup.
4. Let BlackBox validate the source. It checks integrity, application identity, required control
   state, and every canonical table/index definition without writing the source. A restore must
   preserve the exact direct schema-v1 archive and is not a schema conversion.
5. After restore, refresh health, verify incident count, open representative incidents, and confirm the
   writer can save a new controlled capture before deleting any safety copy.

## What to retain for support

- Privacy-safe support bundle (preferred first artifact).
- Exact BlackBox version and whether the build is an unsigned developer build.
- Reproduction time, action, and visible error written separately by the user.
- Verified backup or failed-incident export, shared only when incident evidence is necessary.
- Raw minidump, settings, or archive only after explicit sensitivity review.

BlackBox has no automatic support upload, remote control, updater, or evidence transmission channel.
