# LSM snapshot pinning and bounded ordered pages

Status: Accepted.

Capture one snapshot under the commit mutex. Clone the active and frozen
trees and copy the current SSTable entries and key bounds. The snapshot
owns the copies and fixes both the revision and the expiration read time.
Its catalog keeps only those SSTables alive, including when a concurrent
compaction retires them. WAL retirement and cleanup of files outside that
catalog can continue. Drop the pin on release and perform durable cleanup
on a subsequent maintenance operation or close.

Use a serialized two-pass scan for both live and snapshot pages. For each
next key, compare the lower-bound candidates from the active tree, frozen
tree, and relevant tables. Choose the greatest revision among equal keys
before filtering deletion and expiration. The first pass calculates the
output allocation and checks for a further visible key. The second copies
only the requested rows. A table error aborts the page without returning
partial rows.

The page builder retains at most two key buffers and one table reader at
a time. The immutable returned page has a separate charged lifetime from
the snapshot. Repeated table seeks and holding the database mutex favor
resource bounds over scan throughput. One snapshot per database bounds
retained catalogs. Callers release snapshots when finished.
