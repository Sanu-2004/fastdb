# ADR 0004 — Serial leveled compaction with staged overlap

Status: Accepted

Selection, ID reservation and publication run with the database mutex held.
Manual compaction can release the mutex once its inputs are open and
its independent workspace has been charged. One such merge can run per
database. Other maintenance is excluded while it runs, preserving the
selected SSTable ranges and protecting temporary outputs from reclamation.
WAL rotation remains available. Publication revalidates selected file IDs
and the level invariant, carries forward the live WAL references and
high-water marks, and orders the combined catalog by file ID. The next
executor can use this separation to support worker scheduling and
cancellation.

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
confirmed. Sequence high-water, distinct versions, tombstones and expiry
metadata are preserved. Manual compaction stops after all selected ranges
reach the last level. The planner may return a resource-limit error when
history for one key exceeds one output's metadata capacity.

The executor budgets input and output buffers before opening the inputs.
It bounds input and output counts and opens one output at a time. A
compaction can require additional disk space until it commits and reclaims
the inputs. Disk headroom admission and background admission are later
milestones.
