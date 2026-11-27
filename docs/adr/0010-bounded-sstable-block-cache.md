# ADR 0010 — Bounded SSTable data-block cache

Status: Accepted

An opted-in LSM handle owns one cache. Each entry represents a verified
data block at an immutable file ID and offset. Separate handles have
separate cache namespaces. Cached data is an in-memory copy of the
on-disk block. Indexes, properties, and Bloom filters continue to load
from disk. Startup validation and compaction read data directly from
their inputs.

The cache uses a hash table with a fixed bucket allocation and an LRU list.
An iterator pins a cache hit through record access. Eviction skips pinned
entries. The iterator keeps a budgeted scratch buffer for each cold or
bypassed block, so low capacity and cache-entry allocation failure still
allow normal reads. Cache insertion follows CRC, record, ordering, and
index-bound validation. A hit uses the already checked block and the reader
still checks file metadata against its captured catalog.

The database reserves the configured capacity up front. The cache charges
the bucket table, cache bookkeeping, entry headers and data bytes within
that capacity. A separate stats API reports actual requested bytes and
the reserved capacity. File IDs are never reused within the cache's
lifetime. Entries for retired files remain eligible for ordinary eviction,
and callers can find them only through a catalog view that still names
those files. Closing the handle frees the cache after readers have drained.

The storage contract requires exclusive ownership of published files.
External modification while a database is open can leave a cached copy
until eviction. Reopen starts with a cold cache and validates from disk.
