# Concurrency and ownership

The database mutex protects the memory trees, revision counter, catalog view,
and iterator accounting. There is one engine lock per database. Commits
linearize at the final revision publication inside that critical section.
Persistent commits preallocate their replacement nodes and stage the
complete WAL frame before exposing mutations. GET, exists and stats
linearize at their locked observation or the capture of a pinned catalog.
Memory-mode and persistent-memory
scan pages linearize during locked construction and own their copies.

All batch checks use the pre-batch read view, including checks listed after
mutations. Multiple mutations of one key consume separate revisions in order.
The last mutation determines that key's state. A batch containing only
conditions may be committed without advancing the revision. A conflict or
allocation failure leaves the batch reusable and the database unchanged.
Pass zero as the expected revision to require absence. A stale nonzero
revision will conflict after a delete and reinsert.

Callers retain options and batch handles. Options are copied by open and
batch creation. A batch copies every added key and value. Batches, options
and individual iterator cursors require caller synchronization if shared.
GET allocates a copy even for an empty value. Use `fastdb_free` on those
copies. Iterator slices are borrowed until close and are read-only.

Range lower bounds are inclusive by default and can be made exclusive.
Upper bounds are exclusive. A null pointer and zero length leaves a bound
open. An empty prefix matches all keys. `fastdb_iterator_next` returns
`FASTDB_NOT_FOUND` at the end of the captured page.
`fastdb_iterator_has_more` reports whether another matching row existed when
that page opened. The last key can be used as an exclusive lower bound for
the next range page. Independently opened pages see independent read views.

The owner must prevent new operations and quiesce existing operations before
calling close. A database with open iterators or an outstanding pinned read
returns `FASTDB_BUSY` and stays open. Release the iterators or let the read
finish, then retry. A close that reaches persistent
cleanup consumes the handle even if sync or cleanup returns an error.
GET copies are independent of that lifetime.

An LSM lookup checks the active tree and then the frozen tree under the
mutex. The first matching tree entry wins, including a tombstone. If both
trees lack the key, GET and exists can copy the relevant SSTable entries and
captured revision, pin file lifetime, and release the mutex during reads.
Each database allows one such reader. A second reader, insufficient
headroom, or pending cleanup uses a serialized lookup. CAS and batch
conditions always use the locked path so checks and publication remain one
operation. The reader owns the copied metadata until it reacquires the mutex
to release its pin.

Compaction swaps the catalog after durable publication. Subsequent readers
observe that catalog, while a pinned reader can still open the original
files by name. One reclamation interval may be deferred. While it is deferred
and the files are pinned, further manual maintenance may return `FASTDB_BUSY`
and the background worker waits. The next maintenance call or close reclaims
the obsolete files after the reader releases them. A manual compaction may
report `FASTDB_BUSY` after one successful catalog edit.
For keys absent from memory, the engine compares matching tables by revision.
The background worker holds the mutex for WAL rotation, catalog commits and
reclamation. It releases the mutex while it constructs and syncs the SSTable
from the immutable tree. Writes may rotate later WAL segments during this
interval. The worker reselects its WAL boundary in the current catalog and
inserts the output by file ID. The frozen tree stays visible through durable
publication. Manual compaction returns `FASTDB_BUSY` while a frozen table is
actively being flushed. When sufficient memory is available, manual
compaction opens its inputs under the mutex, copies any split key, and
releases the mutex during the streaming merge. Reads and writes can proceed.
Other flushes and compactions wait or return `FASTDB_BUSY`, and close returns
`FASTDB_BUSY` while the merge is in flight. After reacquiring the mutex,
compaction validates input IDs, retains the live WAL entries, and publishes
the replacement catalog. Automatic compaction and manual compaction with
insufficient independent workspace use the existing serialized path.

`fastdb_flush` waits for a running flush, retries frozen work after a known
failure and then flushes the active tree synchronously. Close signals the
worker and joins it before releasing storage. In-flight work finishes or
reports its error. Active records can remain recoverable in the WAL on clean
close. LSM range and prefix scans remain pending.
