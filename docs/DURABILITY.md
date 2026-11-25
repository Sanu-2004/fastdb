# Durability and storage availability

The native API and CLI support memory mode, persistent memory, and serial
LSM. Memory mode creates no database files and loses its records and revision
state when its handle closes or the process exits. Persistent modes open an
existing dedicated directory. New databases initialize an empty directory.
An unrecognized directory is refused before new metadata is published.
See [migration](MIGRATION.md) for the supported legacy export path.

## Verified internal persistence primitives

The storage layer opens an existing directory, then takes a nonblocking
exclusive Linux `flock` on its persistent `.fastdb.lock` file. Use a trusted
parent path and retain the lock through all file operations. Do not unlink or
replace the lock file. Database filenames are single path components and data
file opens refuse final symlinks and non-regular files. This assumes a trusted
local filesystem and cooperating processes. Directory confinement across
untrusted parent paths and network-filesystem durability remain outside the
current implementation.

The WAL segment writer syncs the new file and directory before making the
segment available. A synchronous append acknowledges after successful file
sync. An asynchronous append acknowledges once the whole frame is written
and can lose recent data if the system fails. An append or sync error poisons
the open segment and returns `FASTDB_COMMIT_UNCERTAIN`. Reopen determines
whether the affected batch is present. Callers must preserve original files
and their catalog references through an uncertain outcome.

The native persistent handle rejects further reads and writes after a
commit or manifest outcome becomes uncertain. Close releases the handle.
Reopen to determine whether the mutation is present. Clean close syncs
asynchronous WAL writes and reports failure. Group commit and periodic WAL
sync policies remain pending.

A recognized short final frame header, or a valid final header followed by a
short payload, is omitted from recovery. The reader truncates that tail and
syncs the truncation. A complete invalid header or payload fails recovery
without truncating the corresponding segment. A segment selected by the
caller is assumed authoritative. The visitor sees one whole validated frame
at a time. The open caller must discard its provisional replay state if a
later frame fails.

The manifest is a checksummed, bounded complete catalog. To publish one
generation, write and sync a uniquely named temporary image, close it, atomically
rename it over `MANIFEST`, then sync the directory. A failure before rename
leaves the earlier catalog authoritative. A failure syncing the directory after
rename returns `FASTDB_COMMIT_UNCERTAIN`; the owner must reopen and confirm the
authoritative catalog before reclaiming any previously referenced file.

The WAL set synchronizes an old active segment before sealing it and
publishing a new one. Reopen visits only the IDs listed in the catalog.
Sealed segments must match their catalog lengths and ending sequences.
Only the final active segment can repair a recognized torn tail. Failed
publication can leave an orphan. Its reservation remains consumed, and the
orphan is excluded from replay.

Serial L0 flush reserves a file ID, writes and syncs a temporary SSTable,
renames it without replacing an existing target and syncs the directory.
One manifest generation installs its metadata and retires the covered WALs.
Only then does the in-memory tree clear. Errors before manifest publication
leave both the tree and old WAL references available. A directory sync
failure after manifest rename leaves the outcome uncertain. The owner
keeps the old files and reopens before attempting further writes.

The L0 storage publisher can also install a table for the oldest sealed
WAL prefix through a specific revision. The cut must end at a segment
boundary. The edit retires only covered generations and keeps the newer
WAL entries and active descriptor. Recovery starts at the verified table
coverage and replays the newer WALs in order, even when their segment
boundaries advanced the catalog revision high-water during rotation. When
enabled, the flush worker uses this path and holds the frozen tree through
durable publication. The synchronous flush path handles manual requests and
retries after a known background error.

Serial compaction keeps read access to the original SSTables while building
its outputs. It reserves durable IDs, writes and syncs every temporary output,
closes the inputs, renames the outputs and syncs the directory. One manifest
replacement installs all outputs and removes the selected input references.
That edit preserves the WAL references and covered revision. Once durable,
the engine swaps its catalog and reclaims obsolete files. A failed edit
keeps the original inputs. Uncertain publication poisons the handle so
reopen can select the authoritative state before any deletion. A retry after
a certain error cleans orphans from the prior attempt using the active catalog.

LSM recovery validates every referenced SSTable and compares its metadata
with the catalog, including the closed range invariant for deeper levels.
It replays the remaining WALs in catalog order before
syncing the selected directory state. Only after that sync does recovery
remove unreferenced regular WAL, SSTable, checkpoint, and temporary files
with recognized reserved names. Directory enumeration is used for cleanup.
A missing referenced file causes corruption. An error during cleanup
after durable publication can be returned after a flush, checkpoint or
compaction job has already committed. A later job failure may also follow
one or more successful jobs in the same maintenance call. Reopen uses the
committed catalog.

Persistent-memory checkpoints use the same durable publication order. Each
checkpoint copies the live tree and advances the covered sequence to include
deletions. An empty database can therefore carry a nonzero revision. The
manifest installs the new checkpoint and retires the old checkpoint and
covered WALs in one edit. The live tree stays in RAM. Checkpoints run on
request and before rotating beyond the configured WAL segment count.

See the [implementation ledger](../PROGRESS.md) for executed fault and
process-kill tests. Background compaction and power-loss validation remain
pending.
