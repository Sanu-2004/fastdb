# Native configuration

Create a `FastDBOptions` handle with `fastdb_options_create`, initialize a
`FastDBLimits` using `fastdb_limits_default`, and set limits through
`fastdb_options_set_limits`. Open copies the options. Options apply at open
and can be freed afterward. Pass `NULL` to open or batch creation to use defaults.

| Limit | Default | Scope |
|---|---:|---|
| `max_key_bytes` | 65,536 | One nonempty key |
| `max_value_bytes` | 16 MiB | One value |
| `max_memory_bytes` | 64 MiB | DB handle, tree nodes, copied pages and transient commit allocations |
| `max_batch_operations` | 10,000 | Mutations plus conditions in one batch |
| `max_batch_bytes` | 32 MiB | One batch handle, copied inputs and per-entry metadata |
| `max_scan_records` | 10,000 | Requested record count per scan page |
| `max_scan_bytes` | 32 MiB | One copied page, including metadata |
| `max_open_iterators` | 16 | Simultaneous pages per database |

Key and value length limits fit unsigned 32-bit fields. Other options must
leave room for their own minimum metadata. A valid length is also subject to
the operation's aggregate budgets. The implementation can refuse a large
record or a replacement that would exhaust temporary headroom when the
current value and its staged replacement must coexist. Delete releases a
node without allocating. When a page exceeds its byte budget, the scan
returns `FASTDB_LIMIT_EXCEEDED`; retrying with a smaller record count may help.
A record larger than a whole page remains accessible through GET.

To configure persistence, initialize `FastDBPersistentOptions` with
`fastdb_persistent_options_default`, then pass it to `fastdb_options_set_storage`
with an existing directory and mode. A null settings pointer selects the
defaults. The setter copies the directory path and validates the combination
with the native limits. Storage settings apply at open. Reopening a directory
in the other persistent mode returns `FASTDB_UNSUPPORTED`.

| Persistent setting | Default | Scope |
|---|---:|---|
| `durability` | sync | WAL acknowledgement policy |
| `max_wal_segment_bytes` | 64 MiB | One WAL segment, including header |
| `max_wal_frame_bytes` | 32 MiB | One atomic WAL frame, including header |
| `target_block_bytes` | 16 KiB | Target SSTable or checkpoint block size |
| `max_index_bytes` | 1 MiB | One table's sparse index |
| `max_filter_bytes` | 256 KiB | One table's combined Bloom partitions |
| `max_catalog_files` | 128 | Files referenced by one catalog |
| `max_catalog_bytes` | 512 KiB | Encoded catalog |
| `bloom_bits_per_key` | 10 | Bit budget per record, zero disables writing filters |
| `checkpoint_wal_segments` | 0 | Persistent memory uses up to eight segments, bounded by the catalog limit. A nonzero value configures the threshold. LSM requires zero. |
| `min_free_bytes` | 0 | Minimum filesystem-available bytes to preserve after a WAL append or estimated flush, checkpoint, or compaction output. Zero disables the check. |
| `max_open_files` | 256 | Per-database descriptor budget used to cap compaction readers. Persistent memory requires at least 5 and LSM at least 10. |

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
maximum catalog image. Compaction conservatively budgets twice the input file
bytes plus one maximum catalog image. A low estimate returns
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
error after its L0 table is durable. Size-tiered compaction and history
garbage collection are pending.

The engine uses no automatic eviction. Workspace charges describe
engine-requested allocations and reserved capacity. See
[architecture](ARCHITECTURE.md) for the accounting boundary.
