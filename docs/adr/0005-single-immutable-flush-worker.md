# ADR 0005 — One immutable memtable and one flush worker

Status: Accepted

LSM can reserve one immutable memtable slot and one worker per database.
Enabling the worker reserves both one table reader and one table writer.
The worker seals the current WAL before replacing the active tree. It holds
the database mutex for that transition and the manifest edits. The worker
uses positional file I/O on the frozen tree outside the mutex. Foreground
writers may reserve more WAL IDs while it builds. At publication the worker
rechecks the sealed cut against the current catalog and inserts the table in
file-ID order. The active WAL is rebased by ID.

Before publication, recovery may replay both trees' WAL segments into one
active tree. Write admission bounds the sum of their key counts and byte
lengths by one table's metadata capacity. This conservative bound preserves
reopen with the current replay design. After publication the covered WALs
are eligible for reclamation and the frozen tree can be freed.

The worker is enabled with an explicit threshold. Zero preserves the
existing manual maintenance API and memory reservation. When the frozen
slot is full, writers that reach the threshold can retry after it clears.
Known worker errors retain readable records and can be retried with an
explicit flush. Uncertain publication keeps the old references until
reopen. The worker stops before the engine releases its storage handle.

The optional worker also schedules at most one automatic compaction after a
successful L0 flush. It uses the existing detached merge path, while retaining
the single-maintenance exclusion. A queued compaction can be cancelled during
close; a running merge is drained and joined. Flush publication errors and
compaction errors are tracked separately because a successful flush remains
authoritative even if the later compaction fails. A subsequent explicit flush
can retry maintenance. Tests verify the level transition and reopen with one
remaining L0 table and one L1 output when the L0 trigger is two.
