# Atomic manifest snapshots and durable file IDs

Accepted for the persistence layer on 2026-11-25.

FastDB uses one authoritative `MANIFEST` name and replaces its complete
checksummed contents on each edit. The directory lock excludes other
processes. The owning process serializes commits and checks the prior
generation, database identity and nondecreasing high-water values before
replacement. Complete replacement bounds log replay and avoids a second
mutable pointer file. Each update reads and writes catalog metadata with a
cost proportional to the configured manifest size.

File creation takes a reserved positive ID. A reservation commits a catalog
generation before the new file is created. If publication fails, any output
with that ID remains outside the catalog. Future reservations advance past
it. Only after the owner establishes the authoritative manifest can a
separate reclamation step clean up unreferenced files.

The WAL-set implementation uses a second catalog edit to seal the old WAL
and add the active WAL after the new header and directory are synced.
Directory-sync failure after a manifest rename requires recovery before
another edit or reclamation. See [durability](../DURABILITY.md) and
[file format](../FILE_FORMAT.md) for the sequence and error contracts.
