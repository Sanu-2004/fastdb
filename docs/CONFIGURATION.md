# Native configuration

Create a `FastDBOptions` handle with `fastdb_options_create`, initialize a
`FastDBLimits` using `fastdb_limits_default`, and set limits through
`fastdb_options_set_limits`. Open copies the options. Options apply at open
and can be freed afterward. Pass `NULL` to open or batch creation to use defaults.

| Limit | Default | Scope |
|---|---:|---|
| `max_key_bytes` | 65,536 | One nonempty key |
| `max_value_bytes` | 16 MiB | One value |
| `max_memory_bytes` | 64 MiB | DB handle, tree allocations, copied pages, snapshot copies and transient commit allocations |
| `max_batch_operations` | 10,000 | Mutations plus conditions in one batch |
| `max_batch_bytes` | 32 MiB | One batch handle, copied inputs and per-entry metadata |
| `max_scan_records` | 10,000 | Requested record count per scan page |
| `max_scan_bytes` | 32 MiB | One copied page, including metadata |
| `max_open_iterators` | 16 | Simultaneous pages per database |

Key and value length limits fit unsigned 32-bit fields. Other options must
leave room for their own minimum metadata. A valid length is also subject to
the operation's aggregate budgets. The implementation can refuse a large
record or a replacement that would exhaust temporary headroom when the
current value and its staged replacement must coexist. In memory modes,
delete releases a node without allocating. A scan that exceeds its page
byte budget returns `FASTDB_LIMIT_EXCEEDED`. Retrying with a smaller record
count may help.
A record larger than a whole page remains accessible through GET.

All storage modes allow one open snapshot per database. Capture returns
`FASTDB_LIMIT_EXCEEDED` if the snapshot slot is occupied or if the copy
would exceed remaining memory headroom. Snapshot bytes appear in
`FastDBStats.memory_bytes` and reduce headroom for writes and scan pages
until release.

In LSM mode a snapshot pins up to one catalog's worth of SSTables. Flush
and compaction can continue. Only captured SSTables are retained after
retirement. Release makes those files eligible for cleanup on the next
maintenance operation or close. An LSM page also budgets two key buffers
of at most `max_key_bytes` each and uses the reserved table reader workspace.

### LSM memtable arena

After selecting LSM, `fastdb_options_set_lsm_arena(options, chunk_bytes)`
configures the allocator for the next open. Zero selects individual node
allocations and is the default. A nonzero value sets the minimum payload
capacity of each chunk. A larger batch can allocate one chunk sized for
all of its final mutations. The memory cap charges headers, alignment
padding, unused chunk capacity, and space held by overwritten nodes.
Frozen chunks stay charged until the corresponding SSTable is published.
Snapshot captures copy live records into independently owned nodes.

The background flush threshold counts full chunks. Set it and the chunk
size with room for WAL staging and application work within the memory cap.
If the next chunk cannot fit, a write returns `FASTDB_LIMIT_EXCEEDED` before
appending to the WAL. A manual flush releases the active arena after
durable table publication. Reopen to change the allocator. WAL and SSTable
formats are unchanged. To enable the arena or raise its chunk size on an
existing database, flush before reopening to minimize replay memory use.

### LSM block cache

After selecting LSM, configure the next open with
`fastdb_options_set_lsm_block_cache(options, max_bytes)`. Zero is the
default and disables the cache. The engine reserves the full cap from
`max_memory_bytes` when admitting writes, scans, snapshots and maintenance.
The cap includes hash buckets, cache bookkeeping, block headers and cached
data. Configuration requires capacity for cache metadata and room to store
data. Blocks larger than available capacity use the reader's reserved
scratch buffer. Reopen to change the cap.

`fastdb_block_cache_stats` reports hits, misses, capacity evictions, entry
count, allocated bytes, and the configured cap. Counts describe data-block
lookups after catalog and index selection. A cache miss counts even when
the disk read fails or the block cannot be retained. Counters saturate at
`UINT64_MAX` and reset on open. Disabled caches report zero for all fields.
The cap is already included in `FastDBStats.memory_bytes`.

### Record expiration

`fastdb_put_with_ttl` and `fastdb_batch_put_with_ttl` accept a duration in
milliseconds. Zero means no expiration. The engine samples UTC wall time
while staging the commit and adds the duration to produce an absolute
deadline. Timed writes in the same batch use one sample. If the sum
exceeds `UINT64_MAX`, the write or batch returns `FASTDB_LIMIT_EXCEEDED`
before publishing any mutation. The timestamp is stored in the existing
WAL and table record format.

A value expires at its deadline, including when the read time exactly
equals it. The latest expired value masks every older revision of its
key. Live reads evaluate the current UTC clock and snapshots use their
captured read time. A wall-clock correction can move a live read to the
other side of a deadline. An explicit deletion remains effective across
clock changes. Clock read failures are reported as `FASTDB_IO_ERROR`.

`fastdb_purge_expired` samples the clock once and removes up to the requested
number of expired keys from either memory mode. The positive limit may
not exceed `max_batch_operations`. It walks the current tree under the
database mutex and commits each deletion separately, reporting the number
confirmed even if a later deletion fails. Persistent memory uses the WAL
and configured durability for these deletes. A snapshot retains its
independent copy. LSM retains expired records as masks until safe history
collection and returns `FASTDB_UNSUPPORTED` for explicit purge.

### Optional memory eviction

Eviction defaults to `FASTDB_EVICTION_NONE`. For memory-only caches, call
`fastdb_options_set_eviction` with FIFO, LRU or LFU and an explicit
`max_live_bytes` capacity before opening. The capacity includes live tree
nodes, keys, values and policy metadata. It must fit inside
`max_memory_bytes`, which also covers the handle, temporary write
allocations, snapshots and pages. Leave extra room for writes to stage
replacement nodes while the current tree is still intact. Persistent modes
reject enabled eviction.

- FIFO selects the earliest insertion. A replacement keeps its insertion age.
- LRU selects the least recently touched entry. Successful live GET and
  exists calls touch an entry. Writes in one batch share a touch time and
  break ties by revision.
- LFU selects the fewest successful live GET and exists calls, then the
  earliest insertion. Replacements preserve the counter.

Snapshots, scans and batch conditions leave hit counts and recency unchanged.
An enabled cache can evict existing entries as part of publishing a
successful write. The final PUTs of a batch must fit together and are
protected from that batch's eviction. Admission or allocation failure
preserves all prior entries. Evictions share the write's visibility point
and the write's revision boundary. Finding a victim performs one traversal
of the live tree.

To configure persistence, initialize `FastDBPersistentOptions` with
`fastdb_persistent_options_default`, then pass it to `fastdb_options_set_storage`
with an existing directory and mode. A null settings pointer selects the
defaults. The setter copies the directory path and validates the combination
with the native limits. Storage settings apply at open. Reopening a directory
in the other persistent mode returns `FASTDB_UNSUPPORTED`.

| Persistent setting | Default | Scope |
|---|---:|---|
| `durability` | sync | WAL acknowledgement policy. Supported persistent policies are sync, async, interval, and group commit. |
| `max_wal_segment_bytes` | 64 MiB | One WAL segment, including header |
| `max_wal_frame_bytes` | 32 MiB | One atomic WAL frame, including header |
| `target_block_bytes` | 16 KiB | Target SSTable or checkpoint block size |
| `max_index_bytes` | 1 MiB | Flat v1 sparse index, or v2 directory and one index leaf |
| `max_filter_bytes` | 256 KiB | v1 output filter region, v2 output filter region per leaf, and reader Bloom workspace |
| `max_catalog_files` | 128 | Files referenced by one catalog |
| `max_catalog_bytes` | 512 KiB | Encoded catalog |
| `bloom_bits_per_key` | 10 | Bit budget per record, zero disables writing filters |
| `checkpoint_wal_segments` | 0 | Persistent memory uses up to eight segments, bounded by the catalog limit. A nonzero value configures the threshold. LSM requires zero. |
| `min_free_bytes` | 0 | Minimum filesystem-available bytes to preserve after a WAL append or estimated flush, checkpoint, or compaction output. Zero disables the check. |
| `max_open_files` | 256 | Per-database descriptor budget used to cap compaction readers. Persistent memory requires at least 5 and LSM at least 10. |

Select `FASTDB_DURABILITY_INTERVAL` for either persistent mode to start
one WAL sync worker. `fastdb_options_set_wal_interval` accepts a positive
`uint32_t` number of milliseconds after selecting that policy. The default
interval is 100 ms and changes apply on reopen. Interval acknowledgements
follow WAL writes, with later sync on a monotonic deadline. See
[durability](DURABILITY.md) for the failure and scheduling contract.

Select `FASTDB_DURABILITY_GROUP_COMMIT` for either persistent mode to
start a dedicated worker. Override its defaults using
`fastdb_options_set_group_commit` before opening the database. Settings
apply on reopen.

| Group commit setting | Default | Scope |
|---|---:|---|
| `max_pending_requests` | 16 | Requests admitted across the queue, active group, and results awaiting collection |
| `max_pending_bytes` | 64 MiB | Request metadata and borrowed key and value bytes across those requests |
| `collection_delay_ms` | 1 | Monotonic collection delay after the worker observes work. Zero processes the current queue immediately. |

The request and byte limits must allow at least one mutation. A request
larger than the byte cap returns `FASTDB_LIMIT_EXCEEDED`. An admissible
request that would fill beyond either limit returns `FASTDB_BUSY` before
committing. The queue's fixed worker state counts toward engine memory.
Its byte cap also bounds borrowed caller inputs, which are already owned
by the caller. The collection delay excludes I/O, lock acquisition, and
time behind earlier groups. Sync still holds the database mutex.

Readers load a v1 table's Bloom partitions together when the combined
region fits. If the combined region exceeds `max_filter_bytes` and each
partition fits, readers use one reusable partition buffer per iterator.
A partition above the configured cap returns `FASTDB_LIMIT_EXCEEDED`.
Reducing the cap on reopen can cause extra positional reads. New v1
tables still apply the cap to their combined filter region, which can
result in more compaction outputs.

`fastdb_options_set_partitioned_sstables(options, true)` selects v2 for
new flushes, checkpoints, and compaction outputs in either persistent
mode. The default writer uses v1. Reads select v1 or v2 from each file.
Changes apply on reopen. Flush or checkpoint before switching back to
v1 if the active WAL contains a tree that needs v2's metadata capacity.
Older FastDB builds require a version upgrade to open a database with
v2 tables.

The v2 writer divides `max_index_bytes` between the root directory and
its active leaf, and applies `max_filter_bytes` to each leaf. The reader
admits the root and its largest leaf together. A root at capacity stops
the table at a key boundary during compaction. A key group that cannot
fit in one table returns `FASTDB_LIMIT_EXCEEDED`. Memtable admission uses
conservative leaf and directory bounds so flush and replay have room.

### Optional SSTable compression

Build with `-DFASTDB_ENABLE_LZ4=ON` and `liblz4` 1.9.4 or newer, or
`-DFASTDB_ENABLE_ZSTD=ON` and the static `libzstd` 1.5.4 or newer.
Both build options default to off and may be enabled together. The native
Dockerfile accepts the same names as build arguments. CMake locates LZ4
through `pkg-config` and Zstd through its CMake package. Zstd uses a
position-independent static archive for its fixed-workspace API.

In either persistent mode, select `FASTDB_COMPRESSION_LZ4` or
`FASTDB_COMPRESSION_ZSTD` with
`fastdb_options_set_sstable_compression` before opening the database.
This enables v2 for new output. `FASTDB_COMPRESSION_NONE` selects
uncompressed output while preserving the selected table version. The
option applies on reopen. Reader codec selection follows each file.
A build returns `FASTDB_UNSUPPORTED` for options or files declaring a
codec that it lacks.

The writer budgets codec state and output. LZ4 uses a bounded in-place
decode margin. Zstd uses a fixed decoder context and disjoint input and
output buffers. Recovery reserves the largest required reader workspace
before decoding a committed table. A reopen with insufficient memory
returns `FASTDB_LIMIT_EXCEEDED`. A block stays raw unless compression
saves at least about one eighth of its size. Cache bytes reflect the
decoded block size. See [the format specification](FILE_FORMAT.md) for
the envelope and integrity checks.

Persistent memory needs five slots during WAL rotation for the directory,
lock, current WAL, replacement WAL, and temporary manifest. Increase any
previous persistent-memory setting of 4 to at least 5 before reopening.

For LSM, FastDB reserves eight descriptors in its compaction budget: the
directory, lock, active WAL, compaction output writer, a concurrent foreground
flush writer, one pinned point read, a replacement WAL during rotation, and a
manifest file during catalog publication. The effective compaction reader
fan-in is `min(max_merge_inputs, max_open_files - 8)`. Thus the minimum LSM
budget of 10 permits a two-reader merge. Insufficient settings are rejected
at option validation. During recovery, the directory and lock remain open
while catalog validation and WAL replay open one file at a time. Orphan
cleanup can hold a directory stream alongside the active WAL. The budget is
per database. Other database handles and unrelated process descriptors are
outside this accounting.

When `min_free_bytes` is nonzero, persistent writes query available filesystem
space before appending the WAL frame; if the append requires WAL rotation, the
estimate includes a new segment header and one maximum catalog image.
Flush/checkpoint admission estimates table output from records plus one
maximum catalog image. Compaction budgets twice each input's stored bytes
and one maximum catalog image. For a file declaring a codec, it also
considers the record count and configured maximum per-record output
overhead to cover expansion to raw blocks. A low space estimate returns
`FASTDB_LIMIT_EXCEEDED` before the operation
starts. A failed space query returns `FASTDB_IO_ERROR`. These checks are
admission estimates, not reservations: another process, filesystem quota, or
metadata overhead can consume space after the query. `statvfs` availability
may not reflect per-user quotas on every filesystem. Actual write failures
still follow the normal I/O/uncertain-commit rules.

Persistent modes reserve workspace for catalog edits and one table reader or
writer. The remaining budget covers active records, supported scan pages, WAL
frames, and transient commit allocations. Admission also limits key metadata
so the in-memory tree can fit the index and filter caps. A persistent-memory
database can return `FASTDB_LIMIT_EXCEEDED` even after checkpointing because
it retains every live record in memory. LSM supports manual flush to release
its memtable.

After selecting LSM, `fastdb_options_set_lsm_flush_threshold(options, bytes)`
enables one background flush worker when `bytes` is nonzero. The default
zero keeps manual flush available without a worker. The threshold measures
allocated active-tree node bytes. A full batch can take the tree above the
threshold. Reopen to change the setting. The worker adds a fixed state
allocation and reserves a writer and reader workspace for concurrent I/O.
Point reads that run outside the mutex additionally charge the copied table
metadata and one reader workspace against unused memory headroom. Other
point reads can use the serial workspace.

While the one frozen slot is occupied, writes into a new active tree can
continue within the memory and metadata budgets. The memory budget includes
both trees and the temporary allocations needed for the write, including its
WAL frame. Insufficient headroom returns `FASTDB_LIMIT_EXCEEDED` before
appending the frame. A write can fit after the frozen tree is published and
released. Writes return `FASTDB_BUSY`
when that tree reaches the threshold before the worker frees the slot. The
same backpressure applies if a pinned reader delays file reclamation. The
combined key metadata must also fit one replay tree, since recovery can see
both WAL ranges before the frozen table is installed. A background error
pauses automatic work and is returned by close or a write that needs the slot.
For a known failure, explicit `fastdb_flush` retries the frozen work. Reopen
after `FASTDB_COMMIT_UNCERTAIN`.

Initialize `FastDBCompactionOptions` with `fastdb_compaction_options_default`
and set them with `fastdb_options_set_compaction` after selecting LSM storage.
The settings take effect on reopen. Choose a level count that covers all
existing levels. An out-of-range file level fails catalog validation.

| Compaction setting | Default | Scope |
|---|---:|---|
| `enabled` | true | Enables leveled scheduling after flush and manual compact calls |
| `max_levels` | 7 | Includes L0 and the last level, range 2–32 |
| `level0_file_trigger` | 4 | Minimum L0 file count for automatic selection |
| `level1_bytes` | 256 MiB | L1 schedules above this size |
| `level_multiplier` | 10 | Multiplier for successive upper levels, minimum 2 |
| `max_merge_inputs` | 8 | Maximum open input tables per job, minimum 2 |
| `max_working_bytes` | 0 | Optional per-job workspace ceiling. Zero uses available engine memory. |
| `target_file_bytes` | 64 MiB | Soft compaction output target at key boundaries |

Compaction can use the reserved table workspace plus available headroom
after accounting for the active tree, copied pages and reserved catalog
space. Each input adds a reader with one block, metadata, merge bookkeeping,
and room for its catalog index and file ID.
The job also holds one writer. The effective fan-in can decrease with
available memory. Insufficient capacity for a selected job returns
`FASTDB_LIMIT_EXCEEDED`. The output count and hard metadata limits still
apply. A database can reach its catalog cap as it accumulates distinct keys
or retained history.

`fastdb_compact` promotes eligible on-disk tables through the last level.
`fastdb_compact_range` selects whole source files that intersect a range and
also processes overlapping destination files. The lower bound is inclusive
unless `start_exclusive` is set. The upper bound is exclusive. Null/zero
bounds are open and equal nonempty bounds select no files. Active data can
be flushed separately. Manual compaction remains synchronous. When the
optional flush worker is enabled, it schedules at most one automatic
compaction after L0 publication. With sufficient headroom, that worker
compaction streams outside the database mutex and allows a foreground L0
flush; otherwise it uses the serialized path. A flush can report a compaction
error after its L0 table is durable. Compaction reclaims PUTs shadowed by
newer records in the same merge. An unsplit job targeting the last level
can also reclaim a DELETE and its older versions when every unselected
table overlapping the job's key range contains only newer sequences. It
keeps the highest input revision for WAL recovery, keeps the latest PUT
regardless of TTL, and keeps DELETE masks when a snapshot already pins
tables or the job lacks workspace for one extra maximum-size key. See
[ADR 0004](adr/0004-serial-leveled-compaction.md) for the reclamation proof.
Size-tiered compaction is pending.

Eviction is disabled by default. Workspace charges describe
engine-requested allocations and reserved capacity. See
[architecture](ARCHITECTURE.md) for the accounting boundary.
