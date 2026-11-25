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

Persistent modes reserve workspace for catalog edits and one table reader or
writer. The remaining budget covers active records, supported scan pages, WAL
frames, and transient commit allocations. Admission also limits key metadata
so the in-memory tree can fit the index and filter caps. A persistent-memory
database can return `FASTDB_LIMIT_EXCEEDED` even after checkpointing because
it retains every live record in memory. LSM supports manual flush to release
its memtable.

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
space. Each input adds a reader with one block, metadata and heap slots.
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
be flushed separately. Compaction remains synchronous. A flush can report a
compaction error after its L0 table is durable. Size-tiered compaction,
background scheduling and history garbage collection are pending.

The engine uses no automatic eviction. Workspace charges describe
engine-requested allocations and reserved capacity. See
[architecture](ARCHITECTURE.md) for the accounting boundary.
