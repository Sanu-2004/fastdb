# Serial native persistence and checkpoint coverage

Accepted for P2 on 2026-11-25.

FastDB uses the existing per-handle mutex to serialize native mutation
checks, WAL append, and visibility. On a successful WAL acknowledgement,
the engine publishes replacement nodes it has already allocated. Recovery
validates frames and reconstructs a provisional tree before returning a
public handle. LSM keeps tombstones in that tree. Persistent memory
physically deletes the corresponding live nodes while retaining WAL
tombstones until checkpoint coverage is durable.

Persistent-memory checkpoints use the bounded SSTable writer with one live
record per key. The manifest stores the database revision covered by the
checkpoint independently of record revisions. An empty checkpoint can
therefore preserve deletion history without retaining tombstone nodes.
Automatic checkpoints run before rotation beyond the configured number of
WAL segments. File publication uses the same transaction order as L0 flush.

## Interval WAL synchronization

The interval worker shares the database mutex with WAL append and
rotation. It uses the active descriptor directly, keeping the existing
descriptor budget. It waits until dirty work arrives and schedules sync
with a monotonic condition. Later appends can join the pending interval.
Successful sync clears the dirty marker. Recovery treats a replayed
active segment as potentially dirty unless recovery has already synced a
tail repair. A sync failure uses the WAL set's existing poisoned state.
Clean close joins the worker before the last sync and descriptor close.

## Bounded group commit

One worker accepts a bounded FIFO of borrowed caller requests. A monotonic
collection window allows concurrent requests to share a sync. A separate
queue mutex protects admission and result collection. The worker releases
that mutex before applying the group under the database mutex. All normal
operation checks, per-request staging limits, WAL rotation and checkpoint
rules still apply.

The database mutex excludes observers through the final sync. Conditional
requests see preceding provisional writes while the worker owns the mutex.
A failed durability check poisons the database and marks the provisional
suffix uncertain, including dependent condition outcomes. This preserves
independent atomic WAL frames and the existing file format. It also keeps
storage and descriptor lifetimes unchanged during the barrier.
