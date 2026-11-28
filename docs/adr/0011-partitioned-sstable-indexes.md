# ADR 0011 — Partitioned SSTable indexes

Status: Accepted

Keep v1 readable and select v2 for new output through an explicit
persistent option. V2 interleaves bounded groups of data blocks with
their filters and sparse index leaves. This lets the existing output
file carry the metadata as it is finished without extra descriptors,
temporary sidecars, or a second publication transaction.

The writer splits its configured index allocation between its current
leaf and the directory. It reserves room for the current leaf's final
key as it admits records. An output at its directory limit can finish
cleanly and compaction can split at a user-key boundary. The reader
checks that the directory plus any leaf fits the configured index
budget. It searches the directory and one leaf with lower-bound
comparisons, then follows sequential leaves when a key has more history.
Recovery validates the entire ordered stream and its Bloom partitions.
The format and workspace let compaction mix v1 and v2 inputs while
preserving one atomic catalog publication.

### Optional compression

V2 reuses the properties codec field for LZ4 and Zstd. Each index entry addresses
the stored block, and a block stays in the raw representation when its
compression savings are small. A compressed envelope checks the exact
input and decoded lengths. LZ4 uses its documented in-place margin. Zstd
uses separate input/output buffers and statically linked fixed-size
contexts supplied by the engine. Recovery accounts for decoder workspace
from the actual files before walking them. The reservation remains in
effect until that handle closes, so later reads and compaction can reuse
the same bound. Cache entries retain verified decoded data and the stored length
needed to match a future index entry. Uncompressed v1 and v2 formats can
be read with or without either codec dependency.
