# Optional memory eviction and write admission

Status: Accepted for memory-only caches.

Use a live-tree byte capacity inside the hard memory limit. The cache
reserves policy state at open and charges policy metadata on each node.
The default memory and persistent handles retain their original allocation
sizes. Snapshot clones contain records without live cache metadata.

First verify that all final PUTs of an atomic batch fit together within the
live capacity. Then stage all new nodes within remaining hard memory
headroom. No victim is removed before allocation and checks complete.
Under the commit mutex, apply the explicit writes and evict only entries
with revisions at or before the previous commit boundary until the live
tree fits. The admitted PUT set guarantees a victim exists whenever the
tree exceeds capacity. Readers and snapshots see the resulting batch and
its evictions together.

The initial FIFO, LRU and LFU implementations traverse the live tree once
per victim. This avoids separate priority indexes and makes failure
rollback allocation-free. Eviction remains disabled by default and
unavailable to persistent modes.
