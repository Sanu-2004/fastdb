# Concurrency and ownership

The database mutex protects the memory trees, revision counter, catalog view,
and iterator and snapshot accounting. There is one engine lock per database. Commits
linearize at the final revision publication inside that critical section.
Persistent commits preallocate their replacement nodes and stage the
complete WAL frame before exposing mutations. GET, exists and stats
linearize at their locked observation or the capture of a pinned catalog.
Live scan pages linearize during locked construction and own their copies.

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

`fastdb_snapshot_create` copies the active tree, any frozen tree, and LSM
table entries under the same mutex used for batch publication. It captures
the committed revision and UTC read time in that critical section. A
snapshot sees a batch entirely before or entirely after its publication.
Capture takes time proportional to the nodes, keys and catalog bytes
copied. Each database admits one snapshot and charges its allocations to
the database memory budget.

`fastdb_snapshot_get` compares any stored expiration to the captured time
and returns an owned copy and the record's original revision. A later
write, delete or checkpoint can proceed while readers use the immutable
copy. Callers may read one snapshot concurrently and must quiesce its
readers before calling `fastdb_snapshot_release`. Release returns the memory
charge under the database mutex. Snapshot point reads from copied trees
can run without the mutex. Point reads from captured SSTables use the
serialized reader workspace.

Snapshot range and prefix pages read the same captured revision and
expiration time. Pages hold independent copies and may outlive the
snapshot. LSM pages merge active, frozen and table candidates under the
mutex. The highest revision for a key wins before expiration and tombstone
filtering. Page construction uses one table reader at a time and repeats
seeks when measuring and copying the page. Wide scans can hold the mutex
longer than point reads. This keeps memory and descriptor usage bounded
while a streaming scan cursor remains a performance improvement.

The owner must prevent new operations and quiesce existing operations before
calling close. A database with open iterators, a live snapshot or an
outstanding pinned read returns `FASTDB_BUSY` and stays open. Release those
handles or let the read finish, then retry. A close that reaches persistent
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
An optional block cache uses its own mutex for recency, pins, and counters.
A serialized read can enter the cache with the database mutex held. A
detached read releases the cache mutex before returning to the database
mutex. Disk I/O runs outside the cache mutex. A cache hit pins immutable
bytes until the iterator loads another block or closes. The existing point
read and snapshot catalog lifetimes still control which files can be read.

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

When automatic compaction runs on the optional flush worker with sufficient
workspace, it uses the detached merge path. A foreground `fastdb_flush` may
then publish a new L0 table while selected inputs are streaming. The compaction
job stores stable input file IDs rather than catalog indexes, protects its
reserved temporary output ID range from orphan cleanup, and revalidates the
selected inputs and destination overlap closure against the current catalog
before publication. The catalog edit carries forward the concurrently
published L0 table and current WAL references. Compactions remain mutually
exclusive; when detached workspace is unavailable, maintenance stays
serialized. Foreground writes still apply threshold backpressure while the
worker is compacting.

`fastdb_flush` waits for a running flush, retries frozen work after a known
failure and then flushes the active tree synchronously. Close signals the
worker and joins it before releasing storage. In-flight work finishes or
reports its error. Active records can remain recoverable in the WAL on clean
close.

The optional LSM arena reserves space for a staged commit under the same
writer mutex. The reservation can reuse capacity at the end of the current
chunk or allocate a new detached chunk. The WAL append precedes moving the
chunk cursor and linking any new node. A failed commit leaves the cursor
and published tree unchanged and frees any detached chunk. Freezing
transfers ownership of all active chunks with the old tree. The worker
reads the immutable tree until it reacquires the mutex to publish the
SSTable. A failed publication keeps the frozen chunks for retry. Snapshot
memtable copies have independent storage.

Interval durability starts one WAL sync worker per database. The worker
sleeps on a monotonic condition when it sees pending frames, and waits
without a timer when the WAL is clean. The engine mutex serializes the sync
with append, rotation, and catalog publication. It is released while the
worker waits and held during the filesystem sync. Close wakes and joins
the worker before freeing storage and completes any pending sync. A
background sync error poisons the handle under the same mutex.

When the optional flush worker publishes an L0 table, it queues one automatic
compaction on the same per-database worker. The worker tracks flush and
compaction errors separately: a failed compaction does not mark a successfully
published immutable table as failed or block foreground writes on the frozen
tree error path. `fastdb_flush` waits for queued or active worker compaction
before starting a synchronous maintenance path. If a worker merge is already
detached, the caller may flush its active tree concurrently; the worker then
publishes against the updated catalog. Close cancels a queued compaction and
joins the worker; an active merge is allowed to finish before storage handles
are released. Other compactions and L0 flush jobs remain exclusive.
