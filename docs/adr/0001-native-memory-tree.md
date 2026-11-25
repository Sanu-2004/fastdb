# Native memory tree and storage boundary

Accepted for the memory-mode implementation on 2026-11-25.

FastDB uses a C17 library with one private memtable and native revision
allocation per open database. POSIX threads provide the initial serialization
boundary. The document and server layers will enter through the public ABI and
own their JSON and HTTP concerns.

The prototype's red-black ordering is retained in a new left-leaning red-black
tree. Physical deletion and replacement are required for memory-only use. One
allocation carries a node's links, record metadata, key, and value. Replacement
allocates the new node before releasing the old allocation. The internal
memtable accepts externally allocated nodes so a durable commit path can
prepare its memory before issuing I/O.

Range and prefix scans copy one bounded page while holding the database mutex.
This provides a coherent page independent of overwrite and deletion. The
approach bounds pinned memory and gives simple lifetimes. Each page pays for a
copy and performs repeated tree searches. A future snapshot spanning multiple
pages needs its own lifetime and history strategy.

The legacy prototype remains in the source tree for reference and its
reproduction harness builds the original revision independently. Native
memory-mode operations make no filesystem calls. Persistent modes need their own
format, directory lock and verified publication protocol.
