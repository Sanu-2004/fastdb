# ADR 0012 — Size-tiered compaction with the existing catalog

## Context

Leveled LSM storage allows overlapping L0 tables and requires disjoint
ranges in deeper levels. Native reads and scans already compare revisions
across every intersecting table in the captured catalog. Size-tiered
compaction can reuse the merge and publication protocol.

## Decision

Treat each SSTable as one run and classify it by the highest set bit of
its stored file size. A fixed array of 64 counts gives a deterministic
selection pass without allocation proportional to data size. Choose the
smallest eligible tier and use increasing file IDs to break ties. IDs
carry no record ordering semantics. Manual operations can pick across
tiers when no tier has two range-intersecting files.

Write outputs to level 0. The persisted format stays compatible with
leveled compaction. Existing tables in other levels remain eligible
inputs and preserve the disjoint-range invariant until they are
retired. During a detached merge, validate that the selected IDs are
still live. Concurrent overlapping additions can remain in the catalog.

The merge retains a latest expired PUT and all DELETE masks. Snapshot and
point-read pins retain their original inputs. The output byte target
grows with the total selected stored bytes. Key groups stay together
through a hard metadata split. Cap output count below input count, so
every published job reduces the number of runs. Cap jobs per scheduling
call by catalog count at entry to bound work under continuing writes.

## Consequences

A job can hit the metadata or fan-in limit before producing fewer files.
The executor returns a limit error through the normal maintenance path.
It leaves the input references intact and uses the existing orphan
cleanup. IDs reserved by a failed attempt remain consumed.

This layout permits reopening with either strategy using unchanged
manifest validation. It can accumulate overlapping L0 ranges and point
reads may need more table probes. Measure that cost before selecting
size-tiered as a workload default.
