# Immutable tree copies for memory snapshots

Status: Accepted for the memory and persistent memory modes.

The live red-black tree replaces nodes on overwrite and removes them on
delete. Capturing the revision alone would lose the earlier value. The
snapshot therefore clones the tree under the same database mutex used to
publish an atomic batch. The capture also records the database revision
and UTC read time. Checkpoint and WAL generation changes leave the clone
independent of later writes and persistent files.

The engine admits one live snapshot per database. Admission charges the
handle and every copied node to the database memory limit before allocating.
Failure frees the partial copy and leaves the slot available. On success,
the charge reduces write and scan headroom until release. A snapshot
prevents database close, and callers quiesce its readers before release.

Capture holds the mutex and costs time and allocation proportional to the
tree copied. Snapshot point reads use the immutable copy without the engine
mutex. Large LSM datasets require bounded memtable copies and pinned disk
history. That design is tracked separately in P3.8.
