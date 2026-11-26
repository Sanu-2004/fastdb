# PROGRESS.md — FastDB implementation ledger

**Specification:** [AGENTS.md](./AGENTS.md)  
**Repository:** https://github.com/Sanu-2004/fastdb  
**Initial state:** Prepared from a source review of the original repository; no agent implementation or runtime verification has been performed by the author of this ledger. The current checkout may differ.  
**Release status:** NOT VERIFIED — not approved for production use.  
**Last updated:** 2026-11-26  
**Last verified base commit / branch:** `7210a36` / `main`. V050–V053 verify P3.6; V054–V063 verify successive P3.7 slices.
**Current milestone:** P3 — compaction, concurrency, snapshots and bounded resources.  
**Next task:** P3.7 — implement configurable disk headroom checks and safe backpressure.

## 1. How to use this file

This is the durable source of progress across coding-agent sessions. `AGENTS.md` defines the engineering contract. Actual code and executed tests determine what is complete. Before every session, read both files, inspect the current branch and working tree, and verify relevant prior claims. Continue with the earliest unfinished unblocked task whose prerequisites are satisfied. Do not rewrite already-working modules just because this is a new session.

Status vocabulary (use exactly these terms in task rows):

- `TODO`: not started or not verified against the current implementation.
- `IN_PROGRESS`: implementation, verification or documentation remains incomplete.
- `BLOCKED`: an external or technical dependency prevents safe continuation; add a blocker ID and next action.
- `DONE`: implementation, relevant tests, and documentation are verified; cite concrete evidence in the row.
- `DEFERRED`: optional or owner-approved scope change only, with rationale. Cannot satisfy required release gates.

Evidence vocabulary for verification records: `PASS`, `FAIL`, `NOT RUN`, `BLOCKED`, `STALE`, `N/A` (justify N/A). Record command, date, commit/working-tree context and output reference, not just a checkmark. Mark evidence STALE when a relevant code change invalidates it. Never count pre-existing code as DONE without testing. Preserve stable task IDs; split a large task into e.g. P2.3a/P2.3b as necessary. Do not delete an uncompleted requirement.

After every meaningful batch of changes and before ending a session: update task status/evidence, gate status, blockers, session log and handoff. Use repository-relative evidence paths or CI job URLs. Keep long logs under `docs/verification/` or in CI artifacts; do not commit secrets, user data, huge binaries or generated database files. If no commit exists, say so and record dirty paths.

## 2. Historical baseline at `ca5a528`

| Existing component | Observed state (not verified by execution) |
|---|---|
| Red-black memtable | Basic insertion/update/lookup and sorted traversal exist; invariants, failure handling and memory accounting unverified. |
| WAL | Text SET records; only newest log replayed in reviewed source. Durability and multi-generation recovery not established. |
| SSTables | Experimental sorted records with embedded recursive indexes/footers; parser and index correctness unverified. |
| Merge | `mergeSSTables()` incomplete; no working streaming compaction established. |
| Delete | No complete DELETE/tombstone path in the reviewed implementation. |
| Threading | Flush/compaction are synchronous in the reviewed code; thread safety not established. |
| Configuration | Compile-time macros; no runtime mode selection in the reviewed code. |
| API / Studio | No verified native ABI, HTTP server, optional schema layer or Studio implementation. |
| Tests / build | Makefile exists; no verified automated suite or production build. |

V002 reproduced ten of the baseline defects. The [initial audit](docs/INITIAL_AUDIT.md) separates executed findings from source inspection. The replacement native implementation is tracked below.

## 3. Milestone task ledger

Dependencies are expressed by milestone ordering plus explicit IDs when critical. Each row states a minimum exit criterion; the full specification in AGENTS.md remains authoritative. Evidence `—` means no verification is recorded.

### P0 — audit and foundation

| ID | Task and minimum exit criterion | Status | Evidence / next action |
|---|---|---|---|
| P0.1 | Inspect branch, commit, dirty tree, source, license, baseline build/run in temp directory; record environment and results. | DONE | V001, V002; branch, toolchain, disposable build and smoke recorded. |
| P0.2 | Initial audit: verify static defects; identify corruption/ownership/format risks; document initial decisions and legacy data handling. | DONE | V002 and [initial audit](docs/INITIAL_AUDIT.md); checked legacy source and layout. |
| P0.3 | CMake, Make wrapper, GCC/Clang build, library/CLI test harness, temp fixtures, warning and sanitizer profiles. | DONE | V003–V007; CMake/Make, GCC/Clang, CLI/native CTest, separate sanitizer profiles and install targets. |
| P0.4 | Baseline regression tests or reproducible defect fixtures; do not mask known failures; record exact expected failures before fixes. | DONE | V002; ten reproducible baseline defects in tests/legacy. |
| P0.5 | Architecture/invariant docs, module boundaries, dependency/toolchain choices, CI native baseline. | DONE | Architecture, concurrency, durability and configuration docs; ADR 0001; native CI configured. Hosted jobs not run. |

**P0 exit gate:** Reproducible baseline and build/test entry points; audit and regression evidence; no user data touched.

### P1 — native core and memory-only mode

| ID | Task and minimum exit criterion | Status | Evidence / next action |
|---|---|---|---|
| P1.1 | Opaque stable C API, statuses, options, ownership, version, build static/shared libs. | DONE | V003–V004, V007; opaque C ABI, version, statuses, options, ownership and static/shared consumers. |
| P1.2 | Binary-safe record encoding/comparator, sequence/TTL metadata, length and overflow checks. | DONE | V003–V005; exact byte layout, binary compare, length/record/frame checks and revision overflow tests. |
| P1.3 | Refactor red-black memtable; ordering/color/black-height tests, safe allocation and accurate memory tracking. | DONE | V003–V005; LLRB insertion/replacement/deletion, 65,536-operation model test plus invariants and byte accounting. |
| P1.4 | Memory-only open/close/PUT/GET/DELETE/exists/reinsert; no disk data writes; memory limits and physical reclamation. | DONE | V003–V005; binary CRUD, physical deletion, isolation, limits and no created files across CLI processes. |
| P1.5 | Serialized writer/read safety, revisions/CAS including expected absence, condition checks and atomic multi-key batches. | DONE | V003–V005; revision/absence CAS, pre-batch conditions, atomic multi-key commits and allocation-failure rollback. |
| P1.6 | Ordered iterator/range/prefix scans with documented bounds, ownership and errors. | DONE | V003–V005; captured pages, inclusive/exclusive bounds, binary prefixes, count/byte budgets and owned lifetime. |
| P1.7 | Runtime configuration validation, independent DB instances, CLI CRUD and error handling. | DONE | V003–V005; runtime memory-mode limits and independent handles, binary hex CLI and argument/EOF tests. |
| P1.8 | Native unit/integration/model tests; verify binary zero bytes, empty values, OOM, repeated updates, absence and conflicts. | DONE | V003–V005; native unit, model, concurrency, integration and allocation-fault tests. Release-scale stress remains P8. |

**P1 exit gate:** Working standalone memory DB and native ABI; safe CRUD/CAS/batches; no memory persistence side effects.

### P2 — persistence and recovery

| ID | Task and minimum exit criterion | Status | Evidence / next action |
|---|---|---|---|
| P2.1 | Filesystem abstraction with robust I/O, 64-bit offsets, sync/rename, exclusive directory lock, failpoints. | DONE | V003–V005; relative file opens after exclusive lock, 64-bit offsets, short I/O, EINTR, ENOSPC, sync/rename and fault injection. |
| P2.2 | Versioned binary WAL/batch frames, CRC, rotation, persistent IDs, all configured sync policies (group commit may finish P4.3). | DONE | V016–V018; public writes stage and sync or asynchronously append complete frames. Bounded rotation and reopen verified. Interval and group commit are tracked in P4.3. |
| P2.3 | Durable manifest/CURRENT and atomic version edits, high-water IDs/sequences, manifest rotation. | DONE | V008–V011; checksummed bounded MANIFEST snapshot, atomic generation replacement, monotonic file reservation, high-waters and uncertainty tests. ADR 0002. |
| P2.4 | SSTable block format and writer: sorted records, offset array, checksums, metadata/footer and bounds. | DONE | V008–V010; bounded SSTable writer, sorted admission, large records, checksummed blocks, offset arrays, properties, footer and sync failure. |
| P2.5 | SSTable reader/iterator/seek/status, corruption-safe binary search and multi-version block handling. | DONE | V008–V010; positional reader, verify/seek/iteration, cross-block versions, checked offsets and corruption versus EOF. |
| P2.6 | Bloom filter v1 and sparse block index; no false negatives, tombstones included, checked metadata. | DONE | V008–V010; bounded index and per-block Bloom partitions, 2,048-key no-false-negative fixture, tombstone/TTL metadata inclusion, corruption fallback and counters. |
| P2.7 | Serial flush, durable file+manifest publication, WAL retention until commit; never discard data on failed flush. | DONE | V016–V018; public serial L0 flush, WAL retention until durable catalog publication, cleanup and reopen. |
| P2.8 | Reopen/recovery from all required WALs, atomic batch replay, incomplete tails vs corruption, orphan handling. | DONE | V016–V018; native LSM reopen validates catalog and table metadata, replays ordered WALs into a provisional tree, repairs active tails and reconciles reserved orphan files. |
| P2.9 | Persistent in-memory mode, atomic checkpoints and recovery; no silent eviction. | DONE | V016–V018; persistent memory checkpoints manually and before segment rotation, replays covered history and complete frames, preserves empty-checkpoint revisions and refuses implicit eviction. |
| P2.10 | Legacy detection; offline verified export/migration if feasible or explicit limitations; preserve originals. | DONE | V016–V018; legacy layout refusal without source writes and offline single-WAL exporter with no-clobber output. Unsupported layouts documented. |
| P2.11 | Persistence fault and subprocess crash tests; durable acknowledgments, manifest publication and recovery verified. | DONE | V016–V018; native storage I/O faults, partial frame kill, and 18 public L0/checkpoint process-kill cases. Release-scale crash matrix remains P8.4. |

**P2 exit gate:** All three modes work; checked persistence; replay, flush and failure tests establish documented guarantees before background workers.

### P3 — compaction, concurrency, snapshots and bounded resources

| ID | Task and minimum exit criterion | Status | Evidence / next action |
|---|---|---|---|
| P3.1 | Two-way streaming merge with checked iterators; newest sequence wins; preserve needed tombstones and input on errors. | DONE | V019–V021. Two immutable SSTable streams merge in unsigned key/revision order. Checked iterator failures preserve sources. Versions, tombstones and expiry metadata are retained. |
| P3.2 | K-way merge and bounded output splitting, file/metadata buffers and fan-in; tests for duplicate keys and errors. | DONE | V019–V021. Heap merge with reader/heap/writer workspace and fan-in limits, output count cap, key-group-preserving temporary output splitting and hard metadata limits. |
| P3.3 | Leveled scheduler, overlap closure, L0 and L1+ invariants, byte thresholds, cascading and bottommost behavior. | DONE | V026–V028. Serial threshold selection, cascading, closed range checks, two-input overlap staging and terminal level behavior. [ADR 0004](docs/adr/0004-serial-leveled-compaction.md). |
| P3.4 | Manual range/full compaction and disabled mode; atomic manifest publication and safe reclamation. | DONE | V026–V028. Native and CLI maintenance, active WAL continuity, bounded admission, old input retention on failure, reopen, and nine compaction process-kill cases. Full maintenance promotes eligible tables to the last level and retains all history. |
| P3.5 | Coherent read-view capture, pinned versions/files, active-to-immutable rotation and background flush worker(s). | DONE | V031, V036–V037. Optional single flush worker, bounded pinned GET/exists, coherent frozen and active reads, deferred reclamation, worker resumption, and shutdown. [ADR 0006](docs/adr/0006-pinned-sstable-reads.md). |
| P3.6 | Compaction worker(s), conflict reservations, commit revalidation, scheduling, safe failure/cancel/shutdown. | DONE | V050–V053: compiler/sanitizer matrix, 20x concurrency repeats, analyzer, Docker build/restart. Stable IDs and destination overlap closure are revalidated after detached merge; concurrent L0 flush is retained, temporary output IDs are protected, worker failure preserves inputs and retry/reopen succeeds; close drains active worker merge. Queued-before-start cancellation has code coverage by inspection but no separately gated regression. |
| P3.7 | Backpressure, queue/memory/FD/background budgets, disk headroom and safe error states. | IN_PROGRESS | V054–V060 implement optional `min_free_bytes`, injectable `statvfs` query and preflight for WAL/flush/checkpoint/compaction, plus per-database compaction-reader budgeting. V058 reproduced the six-slot undercount (10 database FDs observed with budget 8) during concurrent compaction, WAL rotation and foreground flush; the reservation is now eight slots and the LSM minimum is 10. V059 verifies single-write BUSY/retry and tombstone recovery. V060 verifies a two-operation batch returns BUSY with neither mutation visible, then the same batch handle commits after flush releases the slot; the deleted key remains absent and the other survives reopen. GCC Debug, Clang Release, GCC ASan/UBSan/LSan, and Docker GCC Release each pass 23/23. Remaining: aggregate active+frozen memory saturation and broader background-error review; `max_open_files` is not a strict all-path/process descriptor cap and disk-space estimates are advisory. |
| P3.8 | Native snapshots and history retention across memtable and disk; atomic batch visibility; coherent merged scans. | TODO | — |
| P3.9 | TTL with controlled clock, expiration masking, physical reclamation and explicitly enabled memory eviction policies. | TODO | — |
| P3.10 | Safe obsolete-version/tombstone collection with proof, active snapshot checks and no data resurrection. Depends P3.8/P3.9. | TODO | — |
| P3.11 | Multilevel integration, crash during compaction, concurrent GET/PUT/DELETE, history checks, TSan and bounded-memory merge verification. | TODO | — |

**P3 exit gate:** Concurrent LSM storage works across levels, durable reads remain correct, no resurrection, and large merge memory use is measured and bounded.

### P4 — performance and configuration hardening

| ID | Task and minimum exit criterion | Status | Evidence / next action |
|---|---|---|---|
| P4.1 | Optional arena allocator with real allocation accounting, bounded history and safe lifetime. | TODO | — |
| P4.2 | Thread-safe bounded LRU block cache, file ID/offset keys, pinned-block lifetime, metrics. | TODO | — |
| P4.3 | Group commit and interval sync worker; durable ACK ordering and sync-failure tests. | TODO | — |
| P4.4 | Partitioned/hierarchical indexes and filters; bounded construction and lookup across large SSTables. | TODO | — |
| P4.5 | Optional LZ4/Zstd codecs, encoded per file; mixed-format/corruption tests. | TODO | — |
| P4.6 | Size-tiered compaction and validated storage profiles; consistent search semantics across overlapping runs. | TODO | — |
| P4.7 | Native stats/metrics and benchmark harness; repeatable baseline measurements before optimization. | TODO | — |
| P4.8 | Runtime config introspection, live/reopen/migrate classification, low-memory and resource-limit tests. | TODO | — |

**P4 exit gate:** Configurations are real, bounded and tested; performance improvements backed by measurements, not claims.

### P5 — multi-database and document functionality

| ID | Task and minimum exit criterion | Status | Evidence / next action |
|---|---|---|---|
| P5.1 | Reusable cgo bridge, memory ownership, errors, handle lifecycle, real native tests. | TODO | — |
| P5.2 | Multi-DB registry and crash-recoverable lifecycle; independent modes/limits; path confinement. | TODO | — |
| P5.3 | Collision-free raw/document/system namespaces, collections and safe deletion/recreation with new IDs. | TODO | — |
| P5.4 | JSON document CRUD, generated/supplied IDs, revisions, strict parsing and numeric precision. | TODO | — |
| P5.5 | Optional schemaless/strict/warn JSON Schema, persistent versions, dry-run and coordinated schema migration. | TODO | — |
| P5.6 | Atomic JSON Patch and typed operators; nested/array edits, schema validation and conditional native batches. Depends P1.5/P5.5. | TODO | — |
| P5.7 | Atomic conditional updates and document batches, concurrent increments and stock reservation tests. | TODO | — |
| P5.8 | Query DSL, bounded scans, filter/projection/sort, stable cursors and explain. | TODO | — |
| P5.9 | Single-field/unique/compound indexes, atomic document/index updates, lifecycle/rebuild/TTL and scan parity tests. | TODO | — |
| P5.10 | Document-layer integration, concurrent index uniqueness and schema transition tests, restart verification. | TODO | — |

**P5 exit gate:** Real document storage, optional schema and atomic partial updates work with no lost updates; indexed queries agree with scans.

### P6 — REST server, security and operations

| ID | Task and minimum exit criterion | Status | Evidence / next action |
|---|---|---|---|
| P6.1 | HTTP server lifecycle/config/shutdown, native handle pinning, validated requests and consistent errors. | TODO | — |
| P6.2 | Versioned database, KV CRUD/batch/scan/CAS and maintenance APIs; binary-safe encoding. | TODO | — |
| P6.3 | Collection/document/schema/query/index routes, ETags/If-Match, conflict and validation responses. | TODO | — |
| P6.4 | Authentication/bootstrap/API keys and database-scoped RBAC; explicit loopback-only dev bypass, TLS/CORS/CSRF as applicable. | TODO | — |
| P6.5 | Request/query limits, rate limits, cancellation, safe retries/idempotency semantics, job handling. | TODO | — |
| P6.6 | Real metrics/health/readiness/storage catalog and compaction/job metadata, structured redacted logs. | TODO | — |
| P6.7 | Consistent backup/checkpoint, restore to new DB, streaming import/export, verification and restore tests. | TODO | — |
| P6.8 | OpenAPI 3.1, interactive docs and executable C/Go/Python/Java/JS integration examples. | TODO | — |
| P6.9 | API integration, cgo checks, Go race/vet, authorization/security tests, restart and restore validation. | TODO | — |

**P6 exit gate:** Authenticated versioned REST API with real storage, tested lifecycle, permissions, document patches and operational workflows.

### P7 — FastDB Studio and deployment

| ID | Task and minimum exit criterion | Status | Evidence / next action |
|---|---|---|---|
| P7.1 | React/TS/Vite shell, API client/auth, accessible responsive light/dark UI and error/loading states. | TODO | — |
| P7.2 | DB dashboard and creation wizard for each mode/profile, config and lifecycle actions. | TODO | — |
| P7.3 | KV explorer with prefix/pagination and reversible binary/text/JSON editing, TTL/revision. | TODO | — |
| P7.4 | Collection/document explorer with nested edits, JSON Patch/operator editing, schema errors and revision conflicts. | TODO | — |
| P7.5 | Schema editor, version/dry-run/migration UI. | TODO | — |
| P7.6 | Query workspace, results, explain, index management and real job status. | TODO | — |
| P7.7 | Real LSM visualizer, actual file ranges, memtable/WAL/compaction stats; memory mode display. | TODO | — |
| P7.8 | Admin flush/compact/backup/restore/import/export, permissions and confirmations. | TODO | — |
| P7.9 | Playwright against real backend, mode creation, CRUD, nested edit, schema errors, conflicts, maintenance/deletion. | TODO | — |
| P7.10 | Docker/Compose, persistent volume, non-root runtime, secure defaults, health checks; restart and smoke tests. | IN_PROGRESS | V040 verifies both persistent modes across containers. V042 verifies the rebuilt non-root image and compaction/reopen smoke. Compose, health and service shutdown depend on the server and Studio. |

**P7 exit gate:** Real browser client and documented deployment verified end-to-end, not a mockup.

### P8 — release hardening and integration evidence

| ID | Task and minimum exit criterion | Status | Evidence / next action |
|---|---|---|---|
| P8.1 | Complete regression/unit/integration suite on GCC and Clang. | TODO | — |
| P8.2 | Million-operation differential run, reproducible seed and result; snapshot/TTL/batch coverage. | TODO | — |
| P8.3 | Concurrent stress and bounded-memory compaction larger than configured budget; record peak RSS, engine-accounted bytes and FD counts. | TODO | — |
| P8.4 | Crash/recovery matrix and filesystem fault matrix with actual durable-state checks. | TODO | — |
| P8.5 | ASan/UBSan/LSan where supported, TSan, static analysis, Go race/vet, fuzz corpus/runs and fixes. | TODO | — |
| P8.6 | Security review, malicious inputs, auth isolation, limits, log redaction, secure deploy guidance. | TODO | — |
| P8.7 | Reproducible benchmarks and result artifacts, limitations and fair durability comparisons. | TODO | — |
| P8.8 | Native install/package, CLI/server/Studio builds, integration examples, complete docs and migration/backup drills. | TODO | — |
| P8.9 | CI workflows, scheduled stress/fuzz, versioned release artifacts and complete test matrix. | TODO | — |
| P8.10 | Engineering report, unresolved risk review, independent-review recommendations and final acceptance audit. | TODO | — |

**P8 exit gate:** Every required task verified or explicitly reported unmet; no false production-ready claim. Owner decides whether to deploy.

## 4. Release verification gates

Gate status is independent of individual task status. PASS requires actual recorded verification on a relevant code version. If evidence becomes stale, mark STALE and rerun. NOT RUN is not failure, but does not establish readiness.

| Gate | Required evidence | Status | Evidence ID |
|---|---|---|---|
| G01 | Reproducible GCC + Clang build, C API/CLI and warnings check | PASS | V023, V041 |
| G02 | Native memory CRUD, binary values and mode isolation/no accidental persistence | PASS | V019–V021 |
| G03 | Persistent-memory checkpoint and reopen; LSM WAL replay and sequence recovery | PASS | V019–V021 |
| G04 | SSTable block/index/filter correctness and corrupt input safety | PASS | V019–V021 |
| G05 | Multi-level streaming compaction, latest sequence, safe tombstones/TTL, reopen | NOT RUN | V026–V028 cover the serial leveled path and tombstone masking. Full gate still needs TTL, snapshots and remaining compaction integration. |
| G06 | Atomic batches/CAS/snapshots/scans, concurrent read/write correctness | NOT RUN | — |
| G07 | Fault injection and process-kill recovery; acknowledged durable state retained | PASS | V041 for current native paths. 28 process-kill scenarios across flush, checkpoint and compaction. |
| G08 | 1M-operation differential run with seed and reference model | NOT RUN | — |
| G09 | Concurrent stress, backpressure, measured large-dataset bounded-memory merge | NOT RUN | — |
| G10 | ASan/UBSan/LSan (supported), TSan, static analysis and fuzz results | NOT RUN | V041 sanitizer pass, V043 static analysis, and V062 TSan concurrency pass 3/3. Full gate remains open for fuzz verification and revalidation across the complete supported matrix. |
| G11 | Schema modes, nested atomic patches, increment and conditional-stock correctness | NOT RUN | — |
| G12 | Query/secondary-index consistency, unique races and persistence | NOT RUN | — |
| G13 | REST/cgo integration, HTTP errors, API docs and actual client examples | NOT RUN | — |
| G14 | Authentication/authorization and security/resource-limit tests | NOT RUN | — |
| G15 | Backup/restore and import/export drill; restart and continued writes | NOT RUN | — |
| G16 | Studio Playwright real-backend tests, build/lint/typecheck | NOT RUN | — |
| G17 | Docker Compose end-to-end, volume persistence, health and shutdown | NOT RUN | Native image and restart checks passed in V040. |
| G18 | Actual reproducible benchmarks and documented resource/latency results | NOT RUN | — |
| G19 | CI mandatory jobs, versioned build deliverables, docs and engineering report | NOT RUN | — |
| G20 | Final risk/limitation audit; independent review and deployment caveats documented | NOT RUN | — |

## 5. Executed command and evidence log

Record actual local and CI runs here. Add one row per meaningful verification; group related commands only when exact results are recorded. Use `V001`, `V002`, etc. Evidence may refer to a concise committed report in `docs/verification/` or CI artifact.

| ID | Date / commit or working-tree context | Exact command and config/seed | Result | Evidence / failures |
|---|---|---|---|---|
| V001 | 2026-11-25 / baseline `ca5a528` | Windows Git: `git status --short --branch`, `git rev-parse HEAD`, `git ls-files`; Docker toolchain versions listed in the audit | PASS | Initial untracked contracts preserved. Linux GCC, Clang, CMake and Make available through Docker. |
| V002 | 2026-11-25 / archived baseline `ca5a528` and fixture working tree | `docker run --rm --network none --mount 'type=bind,source=D:\codex\fastdb,target=/repo,readonly' fastdb-local-toolchain:bookworm bash /repo/scripts/audit-baseline.sh` | PASS | Original Makefile and smoke passed. Ten expected defects observed. [Initial audit](docs/INITIAL_AUDIT.md). |
| V003 | 2026-11-25 / native working tree | GCC Debug CMake/CTest in Docker; exact commands in [native verification](docs/verification/NATIVE_CHECKPOINT.md) | PASS | 7/7, warnings as errors. |
| V004 | 2026-11-25 / native working tree | Clang Release CMake/CTest in Docker; see native verification | PASS | 7/7, warnings as errors. |
| V005 | 2026-11-25 / native working tree | GCC ASan/UBSan/LSan, non-PIE test executables; see native verification | PASS | 7/7. Earlier PIE run had CLI timeout. |
| V006 | 2026-11-25 / native working tree | GCC TSan build and CTest `-L concurrency` in Docker, with a separate non-PIE retry; see native verification | FAIL | Initial TSan runtime reported unexpected memory mapping; non-PIE retry exited SIGSEGV. No race-check result. |
| V007 | 2026-11-25 / native working tree | CMake install to temporary prefix and two independent package consumers; see native verification | PASS | Both static/shared consumers printed `hello`. |
| V008 | 2026-11-25 / persistence working tree | GCC Debug `make test BUILD_DIR=/tmp/fastdb-gcc`; full commands in [native verification](docs/verification/NATIVE_CHECKPOINT.md) | PASS | 13/13. |
| V009 | 2026-11-25 / persistence working tree | Clang Release CMake/CTest; see native verification | PASS | 13/13. |
| V010 | 2026-11-25 / persistence working tree | GCC ASan/UBSan/LSan with non-PIE test executables; see native verification | PASS | 13/13. |
| V011 | 2026-11-25 / persistence working tree | `ctest --test-dir /tmp/fastdb-pkg -L recovery --output-on-failure`, CMake install and both installed example targets; see native verification | PASS | 5/5 recovery tests, including one deterministic SIGKILL fixture. Static and shared consumers ran. |
| V012 | 2026-11-25 / persistence working tree | Clang 14 static analyzer over native core, storage and CLI; see native verification | PASS | No diagnostics on corrected command. |
| V013 | 2026-11-25 / start of Session 002 | GCC Debug `make test BUILD_DIR=/tmp/fastdb-resume` in the read-only Debian Docker fixture | PASS | 13/13, prior checkpoint independently rerun. |
| V014 | 2026-11-25 / public LSM working tree | `CC=gcc cmake -S /repo -B /tmp/fastdb-lsm-gcc -DCMAKE_BUILD_TYPE=Debug`, `cmake --build /tmp/fastdb-lsm-gcc --parallel`, `ctest --test-dir /tmp/fastdb-lsm-gcc --output-on-failure` | PASS | 15/15, including public LSM and sync/flush failure cases. |
| V015 | 2026-11-25 / public LSM working tree | `CC=clang cmake -S /repo -B /tmp/fastdb-lsm-clang -DCMAKE_BUILD_TYPE=Release`, `cmake --build /tmp/fastdb-lsm-clang --parallel`, `ctest --test-dir /tmp/fastdb-lsm-clang --output-on-failure` | PASS | 15/15. |
| V016 | 2026-11-25 / native persistent modes, exporter and crash fixtures | GCC 12.2 Debug CMake/CTest in Debian Docker, build directory `/tmp/fastdb-storage-gcc` | PASS | 18/18. |
| V017 | 2026-11-25 / native persistent modes, exporter and crash fixtures | Clang 14 Release CMake/CTest in Debian Docker, build directory `/tmp/fastdb-storage-clang` | PASS | 18/18. |
| V018 | 2026-11-25 / native persistent modes, exporter and crash fixtures | GCC Debug CMake/CTest, `-DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie`, `ASAN_OPTIONS=detect_leaks=1`, build directory `/tmp/fastdb-storage-asan` | PASS | 18/18. |
| V019 | 2026-11-25 / final native P2 and streaming merge | GCC 12.2 Debug CMake/CTest in `/tmp/fastdb-storage-gcc`. Exact commands in [native verification](docs/verification/NATIVE_CHECKPOINT.md). | PASS | 19/19. Nine recovery tests, including 18 public process-kill scenarios within one test. |
| V020 | 2026-11-25 / final native P2 and streaming merge | Clang 14 Release CMake/CTest in `/tmp/fastdb-storage-clang`. See native verification. | PASS | 19/19. |
| V021 | 2026-11-25 / final native P2 and streaming merge | GCC ASan/UBSan/LSan with non-PIE executables in `/tmp/fastdb-storage-asan`. See native verification. | PASS | 19/19. |
| V022 | 2026-11-25 / final native P2 and streaming merge | Clang static analyzer on all replacement C library and CLI sources. See native verification. | PASS | No diagnostics after fixing the empty-checkpoint backing allocation path flagged in the initial run. |
| V023 | 2026-11-25 / final native P2 and streaming merge | CMake build and install in `/tmp/fastdb-install`, then compile and run independent static and shared consumers. See native verification. | PASS | Both consumers printed `hello`. |
| V024 | 2026-11-25 / start of Session 003 | GCC default CMake configuration, build and CTest in `/tmp/resume` in the read-only Debian Docker fixture. | PASS | 19/19, verifying the unchanged P2/P3 checkpoint. |
| V025 | 2026-11-25 / leveled planner, serial publication and public API | GCC default CMake configuration, build and CTest in `/tmp/native` in the same fixture. | PASS | 21/21. Includes range selection, staged overlap, cascading, WAL continuity, tombstone masking and CLI compaction. Later fault and kill cases were added after this run. |
| V026 | 2026-11-25 / serial compaction and recovery | GCC 12.2 Debug CMake/CTest in `/tmp/fastdb-compact-gcc`; see [native verification](docs/verification/NATIVE_CHECKPOINT.md). | PASS | 21/21. Ten recovery suites include 27 process-kill scenarios and injected compaction manifest errors. |
| V027 | 2026-11-25 / serial compaction and recovery | Clang 14 Release CMake/CTest in `/tmp/fastdb-compact-clang`; see native verification. | PASS | 21/21. |
| V028 | 2026-11-25 / serial compaction and recovery | GCC ASan/UBSan/LSan CMake/CTest in `/tmp/fastdb-compact-asan` with non-PIE executables and leak detection; see native verification. | PASS | 21/21. |
| V029 | 2026-11-25 / native Docker image | `docker build --progress=plain -t fastdb-native:local .` in Docker Desktop, linux/amd64. | PASS | 21/21 in the Release builder. Installed CLI, exporter, SDK headers, libraries and CMake package in the non-root runtime image. |
| V030 | 2026-11-25 / native Docker runtime | Non-root and artifact checks plus two `docker run --rm` rounds on each of two fresh named volumes; see native verification. | PASS | UID and GID 10001. LSM recovered `VALUE 1 76616c7565`. Persistent memory recovered `VALUE 1 00ff` and `VALUE 2 -`. The first smoke invocation had a Windows-to-shell quoting error; the corrected run passed and both runs removed their named volumes. |
| V031 | 2026-11-25 / serial compaction and sealed WAL prefix | Native GCC Debug, Clang Release and GCC ASan/UBSan/LSan CMake/CTest in `/tmp/fastdb-final-$profile`. Exact commands in native verification. | PASS | 21/21 per configuration. The two prefix cases recover the frozen table and two newer WALs after successful or uncertain publication. |
| V032 | 2026-11-25 / final native image from V031 sources | `docker build --progress=plain -t fastdb-native:local .` plus non-root, install and separate-container volume smoke in native verification. | PASS | 21/21 Release tests. Both volume-based results matched V030. Image ID `sha256:88dbd09963626778bad1e7dc242d0ee588c4f92b302baaac467a93012ffd7d66`, Linux amd64. |
| V033 | 2026-11-25 / final native P3 checkpoint | Clang `--analyze` over `src/core/*.c`, `src/storage/*.c`, and `cli/*.c` with the V022 command in native verification. | PASS | Exit 0 with no diagnostics. |
| V034 | 2026-11-25 / Session 004 starting checkpoint | `cmake -S . -B /tmp/fastdb-resume -DCMAKE_BUILD_TYPE=Debug`, `cmake --build /tmp/fastdb-resume --parallel`, `ctest --test-dir /tmp/fastdb-resume --output-on-failure` in the read-only Debian toolchain container. | PASS | 21/21 before P3.5 engine changes. |
| V035 | 2026-11-25 / first worker implementation | GCC Debug CMake build and CTest in `/tmp/fastdb-bg` in the same container. | FAIL | 20/21. The worker state embedded in each handle exhausted a previously supported 512-byte memory-mode budget. The state now uses a separate, charged allocation only for background LSM handles. |
| V036 | 2026-11-25 / background-flush increment | GCC Debug, Clang Release and GCC ASan/UBSan/LSan, `/tmp/fastdb-bg-$profile`; see [native verification](docs/verification/NATIVE_CHECKPOINT.md). | PASS | 22/22 in each configuration. The worker test covers a later WAL rotation before publication, frozen lookup, CAS, tombstone masking, saturation, ENOSPC retry, failed-rotation rollback, close during work and recovery after SIGKILL. |
| V037 | 2026-11-26 / P3.5 pinned SSTable reads | GCC Debug, Clang Release and GCC ASan/UBSan/LSan in `/tmp/fastdb-pin-$profile`; see native verification. | PASS | 23/23 per profile. Tests pin a table before opening it across compaction for PUT and tombstone cases. A separate case verifies background flush publication, deferred work and resumption after pin release. |
| V038 | 2026-11-26 / pinned-read concurrency repeat | `ctest --test-dir /tmp/fastdb-pin-gcc -L concurrency --repeat until-fail:20 --output-on-failure` | PASS | Three concurrency suites completed 20 repetitions each. |
| V039 | 2026-11-26 / pinned-read and worker TSan retry | GCC Debug `-DFASTDB_SANITIZER=thread`; `ctest --test-dir /tmp/fastdb-pin-tsan -L concurrency --output-on-failure`. | FAIL | All three binaries stopped before test code with `ThreadSanitizer: unexpected memory mapping`. No race-check result. |
| V040 | 2026-11-26 / P3.5 native Docker image | `docker build --progress=plain -t fastdb-native:local .` and the two-volume restart smoke in native verification. | PASS | 23/23 Release tests. Both persistent modes recovered from another container. Image ID `sha256:246bb725ec848d19e00979251e6ac8d6a19799ee931e46c618d3bdd2980c79ee`, Linux amd64, UID/GID 10001. |
| V041 | 2026-11-26 / P3.6 compaction ID reservation | GCC Debug, Clang Release and GCC ASan/UBSan/LSan in `/tmp/fastdb-reserve-$profile`; see native verification. | PASS | 23/23 per profile. Checks error before reservation rename, uncertain directory sync, no reuse, ID overflow and three compaction outputs committed with only two manifest edits. |
| V042 | 2026-11-26 / P3.6 native Docker image | `docker build --progress=plain -t fastdb-native:local .` and a two-container LSM compaction/reopen check. | PASS | 23/23 Release tests, then `VALUE 2 6e6577` on a fresh container. Image ID `sha256:3a6740f8a31ac07f682e9d689cd92e0bf9b4786bc54b0b8a21c750623e60b614`, Linux amd64, UID/GID 10001. |
| V043 | 2026-11-26 / final native sources with P3.6 reservation | Clang `--analyze` with the V022 command over `src/core/*.c`, `src/storage/*.c` and `cli/*.c`. | PASS | Exit 0, no diagnostics. |
| V044 | 2026-11-26 / starting `d8e16dd` checkpoint | GCC default configuration, parallel CMake build, and CTest with `--output-on-failure` in `/tmp/resume` inside the read-only Debian toolchain Docker fixture. | PASS | 23/23. |
| V045 | 2026-11-26 / detached manual compaction merge | GCC Debug, Clang Release and GCC ASan/UBSan/LSan CMake/CTest matrix in `/tmp/p36-$p`; see native verification. | PASS | 23/23 per profile. The gated test rotates the WAL during a stalled merge, verifies reads and maintenance exclusion, then reopens to verify foreground data, a tombstone, and a write after publication. |
| V046 | 2026-11-26 / starting `b8b3a71` checkpoint | GCC 12.2 Debug CMake configure/build and `ctest --test-dir /tmp/resume --output-on-failure` in `fastdb-local-toolchain:bookworm`, read-only repository bind. | PASS | 23/23 before this session's worker scheduling changes. |
| V047 | 2026-11-26 / background compaction scheduling working tree | GCC 12.2 Debug, Clang 14 Release, GCC ASan/UBSan/LSan CMake builds and full CTest in one `fastdb-local-toolchain:bookworm` run; exact commands in [native verification](docs/verification/NATIVE_CHECKPOINT.md). | PASS | 23/23 in each profile. New fixture waits for the worker's sealed sequence, verifies one L0 and one L1 table after the trigger, checks reads and reopens. |
| V048 | 2026-11-26 / background compaction scheduling working tree | `ctest --test-dir /tmp/p36-repeat -L concurrency --repeat until-fail:20 --output-on-failure` after GCC Debug build in the isolated Debian container. | PASS | Three concurrency-labeled suites passed 20 repetitions each, including the background flush/compaction case. |
| V049 | 2026-11-26 / final background compaction scheduling working tree | GCC 12.2 Debug CMake configure/build and full CTest in `/tmp/p36-final`, read-only source bind in the isolated Debian container. | PASS | 23/23 after the final test-barrier guard update. |
| V050 | 2026-11-26 / P3.6 concurrent L0 publication working tree | GCC 12.2 Debug, Clang 14 Release, and GCC ASan/UBSan/LSan CMake build plus full CTest; commands and build directories recorded in [native verification](docs/verification/NATIVE_CHECKPOINT.md). | PASS | 23/23 in each profile. Covers stable compaction IDs, concurrent L0 publication, overlap revalidation, close during active worker merge, compaction EIO with retained inputs, explicit retry and reopen. |
| V051 | 2026-11-26 / P3.6 concurrency regression working tree | `ctest --test-dir /tmp/p36-repeat -L concurrency --repeat until-fail:20 --output-on-failure`. | PASS | All three concurrency-labeled suites passed 20 repetitions each (60 suite executions). |
| V052 | 2026-11-26 / P3.6 final native sources | Clang static analyzer over core, storage and CLI C sources using project include paths and C17/POSIX/64-bit-offset defines. | PASS | Exit 0; no diagnostics. Exact invocation recorded in [native verification](docs/verification/NATIVE_CHECKPOINT.md). |
| V053 | 2026-11-26 / P3.6 Docker image and persistent-volume smoke | `docker build --progress=plain -t fastdb-native:local .`; separate containers sharing a temporary named volume performed put, flush, compact, and get. | PASS | Image built; Release CTest 23/23. Reopened value matched (`VALUE 1 76616c7565`); container UID/GID 10001. Temporary volume removed. Image ID `sha256:5599f0b9eb81ca2d942324a3eebd525b17468954d78c1d2b889fc6b2184ab77c` (linux/amd64). |
| V054 | 2026-11-26 / initial P3.7 free-space admission working tree | `docker build -t fastdb-native:local .` using the legacy Docker builder; image build runs Release CMake build and full CTest. | PASS | Final GCC 12.2 Release build and 23/23 CTests passed. New cases cover mocked `statvfs` value/error, query-error write rejection, low-space WAL rejection without revision change, flush rejection with value retained, and successful retry after headroom restoration. An earlier compile caught test initializer errors; the first CTest then caught a fault fixture that did not trigger EIO. Both were corrected before the passing rerun. Final local image ID prefix `e9a8b4f19efa`. |
| V055 | 2026-11-26 / P3.7 headroom boundary and compaction tests working tree | Linux toolchain container: GCC Debug, Clang Release and GCC ASan/UBSan/LSan CMake build/full CTest; plus `docker build -t fastdb-native:local .` GCC Release image build/full CTest. Exact matrix commands are in [native verification](docs/verification/NATIVE_CHECKPOINT.md). | PASS | All four profiles passed 23/23. Added low-space compaction refusal and confirms the SSTable file set remains unchanged, then raises headroom and compacts; `min_free_bytes=UINT64_MAX` rejects a write without revision change. Found/fixed an uninitialized local on frame-size error path. A first draft tested compaction before a table existed and correctly returned OK; moved the assertion after a durable flush. |
| V056 | 2026-11-26 / P3.7 descriptor budget working tree based on `2963f5c` | GCC Debug `/tmp/p37fd-gcc`, Clang Release `/tmp/p37fd-clang`, GCC ASan/UBSan/LSan `/tmp/p37fd-asan` in `fastdb-local-toolchain:bookworm`; plus final `docker build -t fastdb-native:local .`. | PASS | All three native profiles and Docker GCC 12.2 Release passed 23/23. Added per-database `max_open_files` option minimum checks and six-descriptor compaction fan-in arithmetic coverage. Docker only warned that git.exe was unavailable for image build metadata. Committed as `33fe653`. |
| V057 | 2026-11-26 / P3.7 background compaction close-error regression working tree at `a30609d` | Focused GCC Debug `fastdb_background_flush`; full GCC Debug `/tmp/p37c-gcc`, Clang Release `/tmp/p37c-clang`, ASan/UBSan/LSan `/tmp/p37c-asan` in `fastdb-local-toolchain:bookworm`; and `docker build -t fastdb-native:local .`. | PASS | Focused background flush case passed 1/1. Full GCC, Clang and ASan/UBSan/LSan suites passed 23/23 each; final Docker GCC 12.2 Release suite passed 23/23. Added injected compaction EIO: close returns `FASTDB_IO_ERROR`, consumes the handle, then reopen reads values from retained L0 files. No code failure found; this verifies the documented shutdown error contract. `git diff --check` run after documentation update. |
| V058 | 2026-11-26 / P3.7 concurrent compaction/flush descriptor-budget working tree based on `2de7e5c` | Focused GCC Debug `fastdb_background_flush` and `fastdb_lsm_fault`; full GCC Debug `/tmp/p37fd2-gcc`, Clang Release `/tmp/p37fd2-clang`, ASan/UBSan/LSan `/tmp/p37fd2-asan` in `fastdb-local-toolchain:bookworm`; Docker GCC 12.2 Release via `docker build -t fastdb-native:local .`. | PASS (after correction) | The first measured regression run found 10 database FDs with configured budget 8 while background compaction overlapped WAL rotation and a foreground flush. Increased reserved slots from 6 to 8 and LSM minimum from 8 to 10; the two-reader merge now passes. Focused suites passed 1/1 each; full GCC, Clang, ASan/UBSan/LSan and Docker suites passed 23/23 each. Linux `/proc/self/fd` accounting excludes the enumeration FD and subtracts the post-open baseline, then adds the three persistent DB-owned base FDs. |
| V059 | 2026-11-26 / P3.7 frozen-slot backpressure retry working tree based on `0950ae1` | Focused GCC Debug `fastdb_background_flush`; full GCC Debug `/tmp/p37q-gcc`, Clang Release `/tmp/p37q-clang`, and GCC ASan/UBSan/LSan `/tmp/p37q-asan` in `fastdb-local-toolchain:bookworm`; Docker GCC 12.2 Release via `docker build -t fastdb-native:local .`. Exact commands and behavioral scope in [native verification](docs/verification/NATIVE_CHECKPOINT.md). | PASS | Focused background-flush test passed 1/1. Full CTest passed 23/23 in GCC Debug, Clang Release, ASan/UBSan/LSan and Docker Release. `concurrent_build` now writes key `q` after the frozen slot has been released, checks revision 8, deletes it and verifies absence after reopen; the existing gated saturation path verifies the preceding BUSY and retry-after-flush boundary. No engine failure found. |
| V060 | 2026-11-26 / P3.7 atomic batch backpressure retry working tree based on `2f3025d` | Focused GCC Debug `fastdb_background_flush`; full GCC Debug `/tmp/p37b-gcc`, Clang Release `/tmp/p37b-clang`, and GCC ASan/UBSan/LSan `/tmp/p37b-asan` in `fastdb-local-toolchain:bookworm`; Docker GCC 12.2 Release via `docker build -t fastdb-native:local .`. Exact commands in [native verification](docs/verification/NATIVE_CHECKPOINT.md). | PASS | Focused case passed 1/1. Each full profile passed 23/23; Docker GCC 12.2 Release passed 23/23. A two-mutation batch returns BUSY while frozen-slot saturation is gated; neither key is visible. The same batch object commits after `fastdb_flush`, and reopen verifies the delete and retained value. Both normal worker and ENOSPC-retry paths run the assertion. `git diff --check` passed after docs update. |
| V061 | 2026-11-26 / P3.7 live active+frozen accounting working tree based on `2c68dd8` | GCC Debug CMake/CTest in `fastdb-local-toolchain:bookworm`, source mounted read-only, build `/tmp/p37mem-gcc`; focused `fastdb_background_flush` and full CTest. | PASS | Focused case passed 1/1; full suite passed 23/23. During gated flush with active and frozen data, `FastDBStats.memory_bytes` equals the visible active/frozen tree bytes, handle, iterator bytes, and reserved disk workspace, and remains within 128 KiB. This verifies accounting consistency, not RSS or an actual configured-budget saturation boundary. Clang, sanitizers, Docker, TSan NOT RUN on this source change; details in [native verification](docs/verification/NATIVE_CHECKPOINT.md). |
| V062 | 2026-11-26 / P3.7 TSan runtime workaround, source at `9402e0a` | GCC 12.2 Debug `FASTDB_SANITIZER=thread` full build in Docker with `--security-opt seccomp=unconfined`; `setarch x86_64 -R ctest --test-dir /tmp/tsan-p37 -L concurrency --output-on-failure`. Exact invocation in [native verification](docs/verification/NATIVE_CHECKPOINT.md). | PASS | TSan concurrency suite passed 3/3 (`fastdb_concurrency`, `fastdb_background_flush`, `fastdb_read_view`). Default profile reproduced unexpected mapping; default seccomp denied `setarch`; disabling seccomp for disposable container allowed per-process ASLR disable. No race reports. This resolves B004 for this environment, not all-host/platform TSan coverage. |
| V063 | 2026-11-26 / P3.7 TSan concurrency repeat, source at `1fcf0f4` | GCC 12.2 Debug TSan build in Docker with `--security-opt seccomp=unconfined`; `setarch x86_64 -R ctest --test-dir /tmp/tsan-repeat -L concurrency --repeat until-fail:20 --output-on-failure`. Exact command in [native verification](docs/verification/NATIVE_CHECKPOINT.md). | PASS | All three concurrency suites passed 20 repetitions each (60 suite runs total) without TSan diagnostics. Same Linux x86-64 host/runtime; this does not establish cross-platform behavior. |

Suggested commands to establish or adapt in P0 (not executed here):

```bash
git status --short --branch
git rev-parse HEAD
# Run the original baseline build using its existing documented commands.
# Once CMake/test infrastructure exists:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
# As modules appear, record actual native, server, Studio and Docker commands.
```

For stress/fuzz/benchmarks, log seed, operation counts, workload, duration, dataset size, memory/FD limits, hardware/OS/compiler, build flags, and output location. For failures, log error or failing test and reproduction steps. No invented results.

## 6. Architecture decisions and blockers

### Decision ledger

| ID | Decision | State / evidence |
|---|---|---|
| D001 | C17 engine; Go/cgo REST/document layer; React/TypeScript Studio | Accepted architecture. C17 memory mode verified; remaining layers follow the milestone dependencies. |
| D002 | New versioned binary SSTable/WAL/manifest formats; legacy files untouched | Accepted. Segment, manifest, table and Bloom formats verified through public persistence and component tests. Legacy inputs use the separate guarded exporter. |
| D003 | Three independent storage modes; safe persistent defaults and explicit memory eviction | Three modes verified through public native and CLI entry points. Sync is the persistent default. Interval and group commit remain P4.3. |
| D004 | Leveled compaction first; sequence-based visibility; conservative tombstone GC | Serial leveled jobs retain every distinct revision and tombstone. Garbage collection remains P3.10. V026–V028. |
| D005 | Left-leaning red-black tree, one allocation per node, immutable bounded page copies | Accepted in [ADR 0001](docs/adr/0001-native-memory-tree.md). V003–V005. |
| D006 | Complete atomic MANIFEST snapshots plus durable reservations before file creation | Accepted in [ADR 0002](docs/adr/0002-atomic-manifest.md). V008–V011. |
| D007 | Native serial storage and persistent-memory checkpoint coverage separate from the newest live revision | Accepted in [ADR 0003](docs/adr/0003-serial-native-persistence.md). V019–V021. |
| D008 | Oversized overlap closure promotes a bounded prefix while the suffix stays at its source level | Accepted in [ADR 0004](docs/adr/0004-serial-leveled-compaction.md). Each job commits one catalog edit under the existing mutex. V026–V028. |
| D009 | One optional background flush worker and one immutable slot per LSM database | Accepted in [ADR 0005](docs/adr/0005-single-immutable-flush-worker.md). Foreground writes can continue while the SSTable is built. A combined active/frozen metadata bound keeps WAL replay admissible. |
| D010 | One bounded pinned SSTable read per LSM database with one deferred reclamation interval | Accepted in [ADR 0006](docs/adr/0006-pinned-sstable-reads.md). Conditional writes retain mutex serialization. V037–V038. |
| D011 | Reserve a compaction job's bounded output ID range in one durable manifest edit | Accepted in [ADR 0004](docs/adr/0004-serial-leveled-compaction.md). A job consumes IDs from its range without touching the manifest during streaming. V041. |

Add new ADR links (`docs/adr/NNNN-title.md`) for consequential changes. An agent may refine implementation details but may not silently reduce required guarantees.

### Blockers and owner decisions

| ID | Issue | State | Resolution / owner action |
|---|---|---|---|
| B001 | Initial checkout, historical build, sample data and Linux toolchain inspection. | RESOLVED | V001–V002. Use Docker for Linux verification in this environment. |
| B002 | No license file exists in the inspected checkout. | OPEN | Owner decision before selecting a license or redistributing. Local implementation can continue. |
| B003 | Whether real legacy database files require migration is unknown. | OPEN | Canonical single-WAL export verified. Any owner data in multiple WALs or legacy tables needs separate analysis before migration. |
| B004 | GCC TSan runtime mapping failure under default Docker/WSL2 layout. | RESOLVED (local workaround) | V062. Default seccomp blocked `setarch`; run the disposable container with `--security-opt seccomp=unconfined` and execute the concurrency suite through `setarch x86_64 -R`. TSan passed 3/3 with no race reports. This is environment-specific and does not prove platform-independent TSan coverage. |

Add blocker details, affected task IDs, reproduction, attempted solutions, and next action. Close only when resolved. External constraints (missing Docker, TSan support, etc.) must be recorded as NOT RUN/BLOCKED rather than PASS.

## 7. Deferred scope and known limitations

These are deliberate boundaries, not completed features:

- Distributed consensus, replication, automatic failover, sharding, full SQL and full MongoDB/Redis protocol compatibility are outside required v1 scope.
- Native C library and HTTP clients are required; additional direct language bindings or RESP may be proposed after the gates.
- Linux is the primary supported OS. Do not claim Windows/macOS/ARM compatibility without relevant builds/tests.
- No claim of production safety for unreviewed, untested or irreplaceable workloads. Process-kill tests are not equivalent to physical power-loss testing.
- Update this section with real bugs/limitations as they become known. Keep optional deferrals separate from unmet required tasks.

## 8. Session log

### Session 000 — specification preparation (2026-11-25)

- Created the initial engineering contract and progress ledger from a prior static source review and the owner's requested feature scope.
- No repository code changed, no build or runtime tests executed, no benchmark results collected.
- All implementation tasks remain TODO and release gates NOT RUN. Initial source observations are hypotheses to reproduce against the current checkout.
- Next: P0.1, then P0.2/P0.3/P0.4, using disposable data directories.

### Session 001 — 2026-11-25 — Baseline, native memory mode, persistence components

- Started on `main` at `ca5a528bb4fe577a789667c48ed8670962f5e82a` with the untracked contracts. The checked-in legacy sources and any existing data were preserved. No commit or remote change was made.
- P0 and P1 verified locally. Added CMake/Make, native ABI and engine, CLI and C package example, native CI config, audit fixtures and documentation. V001–V002 establish the historical build and ten reproduced defects.
- P2.1 and P2.3–P2.6 verified. Implemented and exercised the locked filesystem, checksummed WAL and manifest, rotated WAL catalog, SSTable writer and reader, sparse index, and Bloom partitions. D005–D006 record the tree and manifest decisions.
- P2.2, P2.7, P2.8 and P2.11 are in progress. Integrated internal serial L0 flush and recovery, including conservative file retention and one subprocess crash boundary. The public database engine still opens memory mode only.
- V008–V010 passed 13/13 CTests each. V011 passed 5/5 recovery tests and both installed C consumers ran. TSan failed to provide race-check evidence (V006). Clang static analysis completed with no diagnostics (V012). Hosted CI was not run. Exact commands are in [native verification](docs/verification/NATIVE_CHECKPOINT.md).
- Next integrate `src/storage` with the staged commit and public lifecycle in `src/core/engine.c`; see the current handoff.

### Session 002 — 2026-11-25 — Public persistence and streaming merge

- Started on `main` at `ca5a528bb4fe577a789667c48ed8670962f5e82a` with the previous uncommitted native checkpoint. Reverified it with GCC Debug in V013. The archived sources and owner data were preserved.
- Completed P2 native wiring, including sync/async WAL batches, serial L0 reads and flush, persistent-memory checkpoints and replay, mode refusal, and a separate offline exporter for canonical single-WAL logs. Corrected a build collision with the legacy SSTable header and fixed the analyzer-flagged backing allocation path for empty checkpoints.
- Completed P3.1 and P3.2 with an internal two-way/K-way SSTable merge and bounded temporary output splitting. The merge retains tombstones and history. Leveled selection and publication remain pending.
- V019–V021 passed 19/19 CTests each. The public recovery test exercised 18 SIGKILL cases across the two persistent modes. V022 completed with no analyzer diagnostics. V023 passed both installed C consumers. TSan remains open under B004. Exact commands and scope are in [native verification](docs/verification/NATIVE_CHECKPOINT.md).
- ADR 0003 records serial persistence and the checkpoint coverage decision. No commit or remote change was made.

### Template for every implementation session (append a new dated entry)

```markdown
### Session NNN — YYYY-MM-DD — short milestone title
- Starting branch / commit / dirty paths:
- Tasks worked on and resulting statuses:
- Changes (repository-relative files):
- Decisions / ADRs:
- Exact commands executed and evidence IDs:
- Tests PASS / FAIL / NOT RUN (with counts only when measured):
- Reproduced defects, blockers, remaining risks:
- Ending branch / commit / dirty paths:
- Next task ID and exact next action:
```

### Session 003 — 2026-11-25 — Serial leveled compaction, WAL prefix flush and Docker

- Reverified the prior 19-test native checkpoint in V024 and resumed P3.3 on `main` at `ca5a528`. The leveled planner and serial executor now run through flush and the public manual APIs. Both keep the installed catalog sequence and the active WAL intact. See V025–V028 and D008.
- V026–V028 each passed 21/21. The injected failures before manifest rename and at final directory sync preserved both original tables until reopen. The crash fixture checks nine boundaries in each maintenance path, for 27 process kills. These checks exercise process failure under Docker Desktop and leave power-loss behavior unverified.
- Began P3.5. `fdb_flush_l0` now accepts an explicit coverage revision. The sealed-cut path retires the oldest complete WAL prefix while preserving later segments and the live WAL descriptor. V031 covers rotation failure and retry, an interior-cut rejection, publication and uncertain directory sync with sealed and active suffixes. Engine flush remains synchronous.
- Built the multi-stage native image in V029 and rebuilt it from the final sources in V032. V030 and V032 verified UID/GID 10001, the SDK files, an LSM reopen after flush and compact, and a persistent-memory reopen with both a checkpointed record and WAL records. All smoke data lived in temporary named volumes. Server and Studio have no runnable components yet. Their container stages, Compose and health checks remain within P7.10.
- No commit was made. New work is in the root Docker files and README, native CLI/API/core/storage code, CMake tests and native documentation. The archived source and owner data were preserved.

### Session 004 — 2026-11-25 to 2026-11-26 — Background flush and read views

- The uncommitted native checkpoint passed 21/21 on resume (V034). Committed and pushed `0415f7d` to `origin/main` before modifying the engine for P3.5.
- The first worker build exposed a memory-mode budget regression (V035). Moved worker state out of handles that use synchronous maintenance and reran the existing budget test with its original assertions.
- V036 verified the optional worker with the full GCC, Clang and sanitizer suites. The gate-controlled worker test demonstrates writes during table construction, active WAL rotation, backpressure, retry, and clean or interrupted shutdown. See D009.
- Committed and pushed the worker as `fd45626`. Completed P3.5 by charging a bounded pinned point read and deferring reclamation through publication. V037–V038 verify old-value and tombstone reads crossing compaction, retained name lifetimes, and a flush worker that resumes when the pin releases. V039 records the local TSan startup failure. V040 rebuilt the Docker image and verified both persistent-volume restart paths.
- Pushed P3.5 as `61736b0` and started P3.6. The serial executor now takes output IDs from one durable reservation. V041–V043 verify the final native tests, Docker rebuild and compaction/reopen smoke, and static analysis.

### Session 005 — 2026-11-26 — Detached manual compaction merge

- Reverified `d8e16dd` (V044). Opened compaction inputs under the database mutex and charged independent workspace before unlocking the manual streaming merge. Other maintenance remains excluded during that merge. After reacquiring the mutex, publication checks input IDs and level bounds and incorporates the live WAL entries and allocator high-water mark. V045 verifies the new interleaving and the existing recovery paths.

### Session 006 — 2026-11-26 — Background compaction scheduling

- Started on `main` at `b8b3a71` with a clean working tree and read the complete root contract and ledger. Reproduced the recorded starting build in V046 (23/23).
- Extended the existing bounded per-database flush worker with an explicit one-job compaction queue. A successful frozen-table publication queues automatic compaction; the detached merge path performs work outside the engine mutex while existing maintenance exclusion remains in force. Flush and compaction outcomes are now independent. `fastdb_flush` waits for queued/running worker compaction. Close cancels queued compaction and joins the worker, allowing an active merge to drain before storage teardown.
- Added a worker-driven L0 trigger regression that verifies one L0 plus one L1 table, reads before close, and the same values after reopen. Updated [concurrency docs](docs/CONCURRENCY.md) and ADR 0005. The first test draft exposed a bad test barrier and an incorrect assumption that one automatic pass drains all eligible L0 files; corrected the barrier and assertion based on actual catalog state. Close cancellation/drain was implemented, but still needs a gated test while the worker is queued or inside the detached merge.
- V047 passed GCC Debug, Clang Release, and GCC ASan/UBSan/LSan: 23/23 each. V048 repeated all three concurrency-labeled suites 20 times each. The local TSan runtime limitation remains B004; no TSan claim was made. No Docker image rebuild, static analysis, or release-scale tests were run.
- Committed the verified implementation as `8bfba02` (`Schedule background LSM compaction safely`).

### Session 007 — 2026-11-26 — Concurrent L0 publication during background compaction

- Started on `main` at `45c8e0c` with the previously reported P3.6 implementation changes in the working tree; no pre-existing user edits were present.
- Completed P3.6 detached worker concurrency: foreground L0 flush can publish during a background merge; compaction plans use stable file IDs, protect reserved temporary output IDs from orphan cleanup, and revalidate input membership plus destination overlap closure against the latest catalog before publication. Catalog carry-forward preserves concurrently flushed tables and WAL state.
- Added deterministic gated coverage for close during an active merge, a concurrent fresh L0 table surviving close/reopen, injected worker output EIO with inputs/data retained, and maintenance retry followed by reopen. Updated concurrency/configuration docs and ADRs 0004/0005.
- V050 passed 23/23 tests in GCC Debug, Clang Release, and GCC ASan/UBSan/LSan profiles. V051 passed all three concurrency suites 20 repetitions each. V052 Clang analyzer exited 0 without diagnostics. V053 Docker build passed 23/23 Release tests and separate-container persistent-volume readback.
- TSan remains NOT RUN due to B004 (`unexpected memory mapping` runtime abort). No power-loss guarantee or queued-before-start cancellation regression is claimed. Active worker shutdown drain, failure/retry, and concurrent L0 publication paths are tested.
- Committed and pushed the verified P3.6 progression as `a2b870c` (`Allow L0 flushes during background compaction`); branch is clean and tracks `origin/main`.
- Next task P3.7: inspect native persistent options and filesystem abstraction; add configurable minimum free-space headroom with safe preflight for WAL append and maintenance output; inject space-query errors/low-space cases and verify no partial mutation/publication, then restore headroom and retry. Document `statvfs` limitations (quotas and concurrent external consumption).

### Session 008 — 2026-11-26 — Initial configurable disk headroom admission

- Started from pushed `a2b870c` on `main`. Added `FastDBPersistentOptions.min_free_bytes`, the fault-injectable `FDB_FS_SPACE` hook, and a `statvfs` query against the locked database directory.
- Persistent WAL appends preflight encoded frame bytes plus configured headroom; rotation adds a segment header and maximum catalog image. Flush/checkpoint estimate table bytes from records plus maximum catalog image; compaction budgets twice selected input bytes plus maximum catalog image. Low-space admission returns `FASTDB_LIMIT_EXCEEDED`; space query failures propagate. Added docs describing estimates and TOCTOU/quota limits.
- Docker first compile caught test-only issues: fault fixture omitted the newly added override fields and warning-as-error rejected old partial initializers. First CTest then exposed a fixture that failed to trigger EIO. Both were fixed. Final V054 Docker Linux Release build passed all 23/23 tests.
- Committed and pushed this tested slice as `a1a7b53` (`Add configurable persistent disk headroom checks`).
- V054 is the latest run: `docker build -t fastdb-native:local .` completed and its Linux GCC 12.2 Release CTest passed 23/23. The low-space fixture verifies query-error and limit failures leave revision/data unchanged; successful retry and flush preserve data. An earlier compile and a first fixture run failed during development and were corrected; only the final full run is PASS. Exact command/result scope is in [native verification](docs/verification/NATIVE_CHECKPOINT.md).
- Next: add arithmetic-boundary and compaction-admission coverage, run Clang and ASan/UBSan/LSan matrices. Continue bounded queue/memory/FD budgets; do not mark P3.7 DONE yet.

### Session 009 — 2026-11-26 — Headroom bounds and P3.7 verification

- Started from pushed `9365535` (clean `main`). Continued P3.7 in dependency order; did not work ahead to later Docker/Compose, server, or Studio tasks.
- Added tests for compaction refusal before any output/catalog change when headroom is insufficient, successful retry after restoring headroom, and the `UINT64_MAX` configured boundary without revision change. Code review caught and fixed use of `frame_bytes` when frame-size calculation returned an error.
- The first compaction assertion ran before a table existed and the engine correctly had nothing to compact; moved it after a verified flush. V055 passed GCC Debug, Clang Release, GCC ASan/UBSan/LSan (23/23 each), and Docker GCC Release (23/23).
- V055 commands/results are recorded in [native verification](docs/verification/NATIVE_CHECKPOINT.md). Committed and pushed as `e812fc5` (`Verify disk headroom compaction admission`).
- Next within P3.7: define and enforce a per-database open-file cap against compaction reader fan-in and fixed directory/lock/WAL/output descriptors; add deterministic admission coverage; continue bounded worker/memory checks. Keep P3.7 IN_PROGRESS until queue, memory, FD, disk headroom and safe error states are all verified.

### Session 010 — 2026-11-26 — Per-database descriptor budget slice

- Started at pushed `2963f5c` on `main`; inspected the prior P3.7 implementation and ran the final native matrix against this working tree.
- Added `FastDBPersistentOptions.max_open_files` (default 256) with minimum validation of 4 for persistent memory and 8 for LSM. Compaction fan-in is clamped to `min(configured max_merge_inputs, max_open_files - 6)`, reserving six descriptors for the database directory, lock, WAL, compaction output, concurrent foreground flush output and one pinned point read. This is a per-database compaction budget, not a process-wide FD limit.
- Added deterministic option-validation and fan-in unit coverage, including LSM minimum 7 rejected/8 accepted and persistent-memory minimum 3 rejected/4 accepted. Updated configuration documentation.
- V056 passed GCC Debug, Clang Release and GCC ASan/UBSan/LSan complete CTest suites, 23/23 each, in `fastdb-local-toolchain:bookworm` with source mounted read-only. `docker build -t fastdb-native:local .` then built the final source and its embedded GCC 12.2 Release CTest passed 23/23. `git diff --check` passed. Docker emitted only the non-fatal warning that git.exe was unavailable for build metadata.
- Initial fixture iteration failed because test disk settings were too small for the default workspace requirements; corrected the fixture to use valid minimal disk settings. Final focused test and all three full matrices passed.
- TSan remains blocked by B004. P3.7 remains IN_PROGRESS: the descriptor cap limits compaction readers but does not impose a process-wide or strict handle-wide cap. Next inspect queue, memory and background-work budgeting/error paths; do not start P3.8 until all P3.7 requirements have evidence.
- Committed and pushed as `33fe653` (`Bound compaction fan-in by descriptor budget`) to `origin/main`.

### Session 011 — 2026-11-26 — Background compaction shutdown error verification

- Started at pushed `a30609d` on `main` with a clean working tree; reread the engineering contract and progress ledger and checked the P3.7 handoff.
- Added `close_reports_unretried_compaction_error` to `tests/native/background_flush.c`. A fault-injected background compaction EIO is left unretried; close must return `FASTDB_IO_ERROR`, consume the handle, and allow reopen to read the retained L0 data.
- The focused GCC Debug command passed the `fastdb_background_flush` case 1/1. Full GCC Debug, Clang Release, and ASan/UBSan/LSan CTest each passed 23/23. Rebuilt the Docker image; its GCC 12.2 Release CTest passed 23/23. Commands and outcome are recorded in V057 and [native verification](docs/verification/NATIVE_CHECKPOINT.md). TSan remains blocked by B004.
- No engine behavior change was needed; this verifies error propagation and recovery already implemented. P3.7 remains IN_PROGRESS.
- Next action: audit `src/core/engine.c` admission/accounting (`fdb_memory_bytes`, `write_headroom`, `prepare_nodes`, `flush_worker`) against `docs/CONFIGURATION.md`; identify an untested queue or memory bound and add a deterministic limit/retry regression. Continue P3.7; do not start P3.8.
- Committed and pushed as `f69cd49` (`Verify background compaction close errors`) to `origin/main`.

### Session 012 — 2026-11-26 — Correct concurrent descriptor reservation

- Started at pushed `2de7e5c` on clean `main`; reread `AGENTS.md` and `PROGRESS.md`, checked status and verified the V057 handoff. Existing workspace and queue tests were inspected to avoid duplicating completed work.
- Added an FD-count probe to the deterministic gated background-compaction/foreground-flush interleaving. Initial execution failed: with `max_open_files=8`, 10 per-database descriptors were observed. This showed the six-slot reserve omitted the replacement WAL and temporary manifest file used during foreground rotation/publication.
- Increased the compaction reserve to eight descriptors and LSM minimum from 8 to 10. The eight slots now account for directory, lock, active WAL, compaction output, foreground flush output, one pinned read, replacement WAL, and manifest publication. Fan-in remains at least two at the new minimum. Updated API/config documentation and regression limits.
- Focused `fastdb_background_flush` and `fastdb_lsm_fault` each passed 1/1. GCC Debug, Clang Release and GCC ASan/UBSan/LSan full CTest each passed 23/23. Docker GCC 12.2 Release build/CTest passed 23/23. Commands/results are V058 and [native verification](docs/verification/NATIVE_CHECKPOINT.md). The initial FD test failure is recorded above; no failures remain.
- `max_open_files` remains a compaction-reader descriptor budget, not a strict cap across all open/recovery paths or a process-wide cap. P3.7 remains IN_PROGRESS; next review the queue/memory accounting paths and add deterministic evidence for an unverified boundary. TSan remains blocked by B004.

### Session 013 — 2026-11-26 — Verify frozen-slot backpressure retry

- Started from pushed `0950ae1` on `main`; reviewed the P3.7 handoff and existing gated immutable-slot saturation test. No preexisting dirty paths were present.
- Added a post-release write/delete/reopen assertion to `tests/native/background_flush.c`, exercising the recovered admission path in both successful worker publication and injected ENOSPC retry flows. The prior gate in the same fixture returns BUSY while the frozen slot is occupied.
- Focused `fastdb_background_flush` passed 1/1. GCC Debug, Clang Release and GCC ASan/UBSan/LSan each passed 23/23; `docker build -t fastdb-native:local .` built and passed its GCC 12.2 Release suite 23/23. Exact commands are in [native verification](docs/verification/NATIVE_CHECKPOINT.md). `git diff --check` is recorded after ledger edits. TSan not run because B004 remains open.
- This is regression coverage; no engine code change was necessary. P3.7 remains IN_PROGRESS. `max_open_files` remains a compaction-reader budget and `min_free_bytes` is advisory against external concurrent space use.
- Next: audit memory admission/accounting in `src/core/engine.c` (`fdb_memory_bytes`, `write_headroom`, `prepare_nodes`, `flush_worker`) against `docs/CONFIGURATION.md`; identify and test the next concrete unverified saturation or recovery boundary. Do not start P3.8.
- Committed and pushed as `3e88947` (`Verify frozen-slot backpressure retry`) to `origin/main`; post-push `git status --short --branch` reported `## main...origin/main` with no dirty paths.
- Committed and pushed as `c008a31` (`Correct overlapping compaction FD budget`) to `origin/main`.

### Session 014 — 2026-11-26 — Atomic batch backpressure and retry

- Started from pushed `2f3025d` on clean `main`; reread `AGENTS.md` and `PROGRESS.md`, checked the current handoff, and audited worker-state/workspace charging plus live frozen-tree headroom in `src/core/engine.c` and `src/core/disk.c`.
- Added a deterministic two-mutation batch while the immutable slot is occupied. The batch returns `FASTDB_BUSY`, both keys stay absent, and the exact same batch object succeeds after `fastdb_flush` releases the slot. The test then deletes one key and verifies after reopen that its tombstone masks it while the second value persists. Runs through successful worker publication and injected ENOSPC recovery.
- Focused GCC Debug `fastdb_background_flush`: PASS 1/1. GCC Debug, Clang Release, GCC ASan/UBSan/LSan full CTest: PASS 23/23 each. Docker GCC 12.2 Release build/CTest: PASS 23/23. Exact commands and results in V060 and [native verification](docs/verification/NATIVE_CHECKPOINT.md). Docker emitted the known non-fatal `git.exe` metadata warning. TSan not run due B004.
- No engine failure was observed; this progression adds the missing atomic backpressure regression. P3.7 remains IN_PROGRESS.
- Next: test aggregate active+frozen memory saturation under a gated worker; assert accounting never exceeds the configured database budget, a rejected commit has no visibility, and retry works after immutable publication/release. Starting files: `src/core/engine.c`, `src/core/disk.c`, `tests/native/background_flush.c`, `tests/native/read_view.c`; do not start P3.8.
- Committed and pushed as `f12e0ab` (`Verify atomic batch backpressure retry`) to `origin/main`; after push, `git status --short --branch` reported a clean `main...origin/main` tree.

### Session 015 — 2026-11-26 — Verify aggregate live memory accounting

- Started from pushed `2c68dd8` on clean `main`; reread the P3.7 handoff and checked the working tree. Reviewed `fdb_memory_bytes`, `write_headroom`, worker workspace reservation, and the active/frozen accounting while the flush worker was gated.
- Added an exact `FastDBStats.memory_bytes` consistency assertion in the active+frozen gated test: handle + active tree + frozen tree + iterator bytes + reserved disk workspace. Existing upper-bound assertion still enforces the configured 128 KiB budget in this fixture.
- In `fastdb-local-toolchain:bookworm`, GCC Debug `/tmp/p37mem-gcc` built successfully; focused `fastdb_background_flush` passed 1/1 and full CTest passed 23/23. Two initial compile attempts exposed test include/declaration issues and were corrected before final PASS. `git diff --check` passed.
- This does not yet force actual `max_memory_bytes` saturation or establish RSS bounds. Clang, ASan/UBSan/LSan, and Docker were NOT RUN on this final source change. P3.7 remains IN_PROGRESS.
- Next: derive a deterministic small-memory LSM configuration that reaches aggregate active+frozen admission limits under a gated worker; verify the rejected batch is invisible, `memory_bytes` never exceeds the configured maximum, and retry works once the frozen slot is released. Starting points: `tests/native/background_flush.c`, `src/core/engine.c`, `src/core/disk.c`, `docs/CONFIGURATION.md`.
- Committed and pushed as `9402e0a` (`Verify active and frozen memory accounting`) to `origin/main`; post-push status was clean `main...origin/main`.

### Session 016 — 2026-11-26 — Resolve local TSan runtime blocker

- Started at pushed `d0182d6` on clean `main`; reread the runtime blocker and current handoff. No project source changes were made.
- Reproduced the default Docker TSan `unexpected memory mapping` failure. Default seccomp also denied `setarch x86_64 -R`. With `--security-opt seccomp=unconfined`, rebuilt TSan binaries and ran the concurrency CTest group under `setarch x86_64 -R`.
- V062 passed all three concurrency tests (3/3) with no TSan race reports. This resolves B004 for this Docker/Linux environment only. G10 remains NOT RUN because fuzz evidence and full-gate coverage remain outstanding.
- Next task remains P3.7 actual active+frozen memory saturation. Exact command and scope in [native verification](docs/verification/NATIVE_CHECKPOINT.md). `git diff --check` to be run before commit.
- Committed and pushed as `7f982df` (`Resolve Docker TSan runtime mapping issue`) to `origin/main`; post-push status was clean `main...origin/main`.

### Session 017 — 2026-11-26 — Repeat TSan concurrency verification

- Started at pushed `1fcf0f4` on clean `main`; verified B004's workaround and P3.7 memory-saturation handoff. No project source changes.
- Rebuilt GCC TSan binaries in Docker with seccomp unconfined and ran all three concurrency-labeled suites under `setarch x86_64 -R` with `--repeat until-fail:20`.
- V063 passed 3 suites × 20 repetitions = 60 suite runs with no TSan diagnostics. Build succeeded. Exact command and scope are in [native verification](docs/verification/NATIVE_CHECKPOINT.md).
- B004 remains resolved for this local setup. P3.7 memory saturation is still the next implementation task; no task status was advanced. `git diff --check` passed before publication.
- Committed and pushed as `7210a36` (`Record repeated TSan concurrency verification`) to `origin/main`; post-push working tree was clean. No code changed. Tests used disposable directories.

## 9. Current handoff

- **Current milestone:** P3 — compaction, concurrency, snapshots and bounded resources.
- **Next task ID:** P3.7.
- **Next action:** Continue P3.7 by testing actual aggregate active+frozen memory saturation, using V061 only as accounting evidence, not proof of saturation. Do not start P3.8 until P3.7 is verified.
- **Next commands:** Derive minimal valid LSM limits and threshold from `fdb_disk_validate` in `src/core/disk.c`; gate worker I/O in `tests/native/background_flush.c`, fill active memory to rejection, assert exact budget and no partial commit, release/publish frozen tree, and retry. Then run all applicable compiler/sanitizer/Docker checks for the final code.
- **Last verified good checkpoint:** P3.7 commit `7210a36` on `origin/main`; V060 passed focused backpressure (1/1), GCC Debug, Clang Release, GCC ASan/UBSan/LSan, and Docker GCC Release (23/23 each). V061 GCC Debug focused/full passed 1/1 and 23/23. V062 TSan concurrency passed 3/3; V063 repeated those suites 20 times each (60/60) using the documented workaround.
- **Working-tree state:** Clean at pushed commit `7210a36` on `origin/main`. No code changed this session. Tests used disposable data directories; no owner data was opened.
- **Unresolved blockers:** B002 license decision and B003 uncertainty about real legacy sources. B004's local runtime blocker is resolved by V062; broader platform coverage remains unverified.
- **Critical limits:** Background compaction shares the optional flush worker and queues at most one job. It starts after L0 flush and excludes other SSTable maintenance while merging; a foreground L0 flush may publish concurrently. Queued-before-start cancellation lacks its own deterministic regression. Close now has a regression proving it reports an unretried background compaction error. `max_open_files` reserves eight slots for compaction fan-in but is not a strict cap during every open/recovery path or process-wide across database instances. Disk headroom is advisory and checks estimated WAL/table bytes; quota awareness and concurrent external consumption are not guaranteed. Queue/memory/backpressure accounting, size-tiered compaction, LSM scans, snapshots, TTL writes, history reclamation, power-loss tests and release-scale verification remain open.

**Completion rule:** If every required task is DONE and every applicable release gate has current PASS evidence, produce the engineering report and give the owner an evidence-based readiness assessment. Otherwise report exactly what remains and continue from the next task in a future requested coding session.
