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
