# ADR 0004 — Serial leveled compaction with staged overlap

Status: Accepted

Selection, ID reservation and publication run with the database mutex held.
Manual compaction and an automatic compaction running on the optional worker
can release the mutex once their inputs are open and independent workspace
has been charged. One compaction merge runs per database. Worker compaction
allows a foreground L0 flush while it streams. Stable input IDs, protected
temporary output reservations and destination-closure revalidation preserve
the selected ranges as the catalog changes. Other compactions remain
excluded. Publication carries forward live WAL and new L0 references and
high-water marks, then orders the combined catalog by file ID. A merge with
insufficient workspace remains serialized.

The planner orders source candidates by level and then ascending file ID.
L0 may overlap. Every deeper level holds disjoint closed key ranges. When
the selected source overlaps more destination files than the job can hold,
choose the leftmost destination and split at its inclusive last key. Every
record in that destination is in the promoted prefix. Every other
destination starts after the boundary. The source suffix stays at its
original level and its range is a subset of the replaced source's range.
This preserves non-overlap at both levels. Additional file splitting takes
place between key groups within either portion of the job.

Each staged or ordinary job commits one full catalog replacement after its
outputs are durable. One durable ID reservation advances the allocator by
the job's output capacity while retaining the same input references.
Streaming consumes IDs from that range without editing the catalog. Unused
IDs remain consumed. Failed jobs
leave the inputs authoritative until the committed replacement can be
confirmed. The sequence high-water, tombstones, and expiry metadata of
retained records are preserved. Manual compaction stops after all selected ranges
reach the last level. The planner may return a resource-limit error when
history for one key exceeds one output's metadata capacity.

The executor budgets input and output buffers before opening the inputs.
It bounds input and output counts and opens one output at a time. A
compaction can require additional disk space until it commits and reclaims
the inputs. Disk headroom admission and background admission are later
milestones.

## Reclaim shadowed PUTs in a merge

The compaction writer sees each key group in descending revision order.
When it encounters an older PUT after emitting a newer record for that
key, it can omit the older PUT. The newer record in the same catalog edit
masks every lower revision regardless of whether it is a value, tombstone
or expired value. Unselected files and concurrently written memory entries
cannot change this ordering. A snapshot or point read captured earlier
uses the old catalog, whose files stay pinned until release.

The output preserves the highest input sequence because that sequence
cannot have a newer record in the same input set. This preserves the
table coverage recovery uses to replay the WAL. The merge still reads and
checks every input for errors.

## Reclaim DELETE-led key groups at the bottom level

An unsplit job writing to the last level may omit a key group when its
newest record is a DELETE below the highest input sequence. Under the
database mutex, the planner checks the closed key range spanned by the
selected inputs against every unselected SSTable at every level. It keeps
the masks if any overlapping table may contain a record at or below the
highest input sequence. A table whose minimum sequence is higher cannot
contain a value that would be resurrected. The frozen and active trees and
recoverable WAL records cover the sequence suffix after the installed
SSTables. A detached job rechecks this catalog proof before publication.

The whole key group is skipped, including any older values. At least the
highest input revision is emitted so that WAL coverage and sequence
allocation remain intact after reopen. The merge reserves one maximum-key
buffer to recognize omitted groups. It keeps the masks if the buffer cannot
fit the job budget. A snapshot already pinning tables prevents the job from
removing masks. A snapshot or point read captured during a detached merge
pins the pre-publication catalog before the engine can retire its files.
Compaction retains the latest PUT even if it has expired, so a subsequent
UTC clock correction follows the same TTL read rules before and after the
merge.
