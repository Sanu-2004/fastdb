# FastDB native architecture

The native library is C17. Its public ABI lives in `include/fastdb/fastdb.h`.
`src/core/engine.c` validates API inputs and options, assigns revisions,
stages mutations and owns the memory database. `memtable.c` implements
unsigned lexicographic ordering with a left-leaning red-black tree. `record.c`
defines a checked binary record primitive. `scan.c` provides bounded,
immutable pages of a coherent read view.

`snapshot.c` captures active and frozen trees, the LSM table set, and the
revision and read time under the database mutex. LSM maintenance may
publish a later catalog while the snapshot retains its original files.
See [ADR 0008](adr/0008-lsm-snapshots-and-bounded-pages.md).

Each database owns its tree, revision counter, limits and mutex. Key ordering
compares unsigned bytes and then lengths. Keys must be nonempty. Values can
be empty. Every successful mutation consumes one positive revision, including
an idempotent delete of an absent key. If the next revision would overflow,
the whole request fails. Revision zero denotes absence for conditions.
Fresh database handles start independent incarnations with revision zero.

The memory modes hold the latest live record for each key. Replacing and
deleting a record releases its previous node immediately. LSM retains active
tombstones until a flush writes them to disk. With background flush enabled,
LSM moves the active tree into one immutable slot after durably rotating its
WAL. The foreground can then append to a fresh active tree and WAL while the
worker writes the immutable tree's SSTable. An atomic batch evaluates
conditions against the pre-batch tree, assigns revisions in submission order,
and stages the last mutation for each distinct key. Persistent modes retain
the submission order in one WAL frame, including repeated mutations to the
same key. The commit performs no allocation after WAL acknowledgement.

Allocated memory accounting uses requested heap allocation sizes, including
node metadata and copied pages. The budget also covers batch commit workspace
and staged nodes while allocating them. The process RSS, allocator overhead,
thread stacks, caller-owned GET results, and caller-owned options and batches
are outside the engine budget. Batches have their own per-handle limits.
LSM can allocate memtable nodes in chunks owned by the active or frozen
tree. The budget charges each full chunk until its tree is released. A
snapshot still copies only live nodes and its catalog bounds.
Persistent handles reserve workspace for bounded table and catalog
operations. `FastDBStats.memory_bytes` includes this reservation.
An unlocked LSM read charges copied candidate-table bounds and an extra
table reader workspace against that same engine budget.
`FastDBStats.record_count` reports in-memory entries. The LSM count includes
entries and tombstones in both trees. An overwritten key can appear in both
counts until publication removes the frozen copy.

The internal merge streams sorted SSTables through one block buffer per
reader and a min-heap of current records. Identical copies of a revision for
the same key are deduplicated. Conflicting copies fail the merge. Compaction
omits older PUTs shadowed by a newer record in the same merge. An unsplit
bottom-level job can also omit a DELETE and its older versions when the
catalog rules out older overlapping data and the job retains its highest
input revision. Compaction keeps the latest PUT even after its TTL expires.
Snapshot pins keep the original tables available. File
targets are soft at key boundaries, so one key's versions are never split
across two outputs. The writer's hard metadata limits can reject an
oversized group. The splitter writes synced temporary files and leaves
catalog publication and deletion to the serial compaction executor.
See [configuration](CONFIGURATION.md) for exact limits.

The leveled planner allows overlapping L0 files and checks disjoint closed
key ranges in L1 and deeper levels. After a flush it checks the L0 file count
and upper-level byte thresholds, giving the deepest eligible source level
priority. Each job selects one source file and its overlapping destination
files. A job that exceeds the fan-in promotes the prefix through the
leftmost destination file's last key and rewrites the suffix at the source
level. Every key group stays together. Subsequent jobs replan from the
published catalog. The last level receives data and has no automatic
promotion. Every compaction job retains the manifest sequence and the active
WAL. A manual call can complete several independent catalog edits.
See [ADR 0004](adr/0004-serial-leveled-compaction.md).

Point lookups use one bounded, reference-protected catalog view per LSM
database. The capture contains the matching entries from one installed
generation and the observed live revision. It also represents the absence
of the key from both memory trees at that instant. File reclamation defers
while this view is pinned. See [ADR 0006](adr/0006-pinned-sstable-reads.md).

LSM point lookups and scans can use a per-handle LRU for verified data
blocks. The catalog still selects the SSTable and supplies the file ID.
Cache pins hold in-memory block bytes independently of catalog file pins.
See [ADR 0010](adr/0010-bounded-sstable-block-cache.md).

The v1 reader can stream one Bloom partition per iterator when the full
filter region exceeds the configured reader budget. The flat sparse index
still fits in memory. Each streamed partition is checked on use, and the
recovery walk checks every partition in the referenced tables.

See [concurrency](CONCURRENCY.md) for linearization and ownership,
[durability](DURABILITY.md) for persistent modes and publication,
and [ADR 0001](adr/0001-native-memory-tree.md) for the tree and page design.
