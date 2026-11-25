# PROGRESS.md — FastDB implementation ledger

**Specification:** [AGENTS.md](./AGENTS.md)  
**Repository:** https://github.com/Sanu-2004/fastdb  
**Initial state:** Prepared from a source review of the original repository; no agent implementation or runtime verification has been performed by the author of this ledger. The current checkout may differ.  
**Release status:** NOT VERIFIED — not approved for production use.  
**Last updated:** 2026-11-25  
**Last verified commit / branch:** `ca5a528bb4fe577a789667c48ed8670962f5e82a` / `main` with uncommitted native P3 and Docker changes (V026–V033). The archived baseline is V001–V002.
**Current milestone:** P3 — compaction, concurrency, snapshots and bounded resources.  
**Next task:** P3.5 — capture coherent read views and add immutable memtable rotation and background flush.

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
| P3.5 | Coherent read-view capture, pinned versions/files, active-to-immutable rotation and background flush worker(s). | IN_PROGRESS | V031 verifies the storage-level WAL prefix cut with newer segments retained. Native read-view pinning, tree rotation, the queue and workers remain open. |
| P3.6 | Compaction worker(s), conflict reservations, commit revalidation, scheduling, safe failure/cancel/shutdown. | TODO | — |
| P3.7 | Backpressure, queue/memory/FD/background budgets, disk headroom and safe error states. | TODO | — |
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
| P7.10 | Docker/Compose, persistent volume, non-root runtime, secure defaults, health checks; restart and smoke tests. | IN_PROGRESS | V029–V030, V032. Native image builds, runs with UID/GID 10001 and recovers both persistent modes across containers. Compose, health and service shutdown depend on the server and Studio. |

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
| G01 | Reproducible GCC + Clang build, C API/CLI and warnings check | PASS | V023, V031–V032 |
| G02 | Native memory CRUD, binary values and mode isolation/no accidental persistence | PASS | V019–V021 |
| G03 | Persistent-memory checkpoint and reopen; LSM WAL replay and sequence recovery | PASS | V019–V021 |
| G04 | SSTable block/index/filter correctness and corrupt input safety | PASS | V019–V021 |
| G05 | Multi-level streaming compaction, latest sequence, safe tombstones/TTL, reopen | NOT RUN | V026–V028 cover the serial leveled path and tombstone masking. Full gate still needs TTL, snapshots and remaining compaction integration. |
| G06 | Atomic batches/CAS/snapshots/scans, concurrent read/write correctness | NOT RUN | — |
| G07 | Fault injection and process-kill recovery; acknowledged durable state retained | PASS | V031 for current native paths. 27 process-kill scenarios across flush, checkpoint and compaction. |
| G08 | 1M-operation differential run with seed and reference model | NOT RUN | — |
| G09 | Concurrent stress, backpressure, measured large-dataset bounded-memory merge | NOT RUN | — |
| G10 | ASan/UBSan/LSan (supported), TSan, static analysis and fuzz results | NOT RUN | V021–V022 partial. Local TSan error V006. |
| G11 | Schema modes, nested atomic patches, increment and conditional-stock correctness | NOT RUN | — |
| G12 | Query/secondary-index consistency, unique races and persistence | NOT RUN | — |
| G13 | REST/cgo integration, HTTP errors, API docs and actual client examples | NOT RUN | — |
| G14 | Authentication/authorization and security/resource-limit tests | NOT RUN | — |
| G15 | Backup/restore and import/export drill; restart and continued writes | NOT RUN | — |
| G16 | Studio Playwright real-backend tests, build/lint/typecheck | NOT RUN | — |
| G17 | Docker Compose end-to-end, volume persistence, health and shutdown | NOT RUN | Native image and restart checks passed in V032. |
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

Add new ADR links (`docs/adr/NNNN-title.md`) for consequential changes. An agent may refine implementation details but may not silently reduce required guarantees.

### Blockers and owner decisions

| ID | Issue | State | Resolution / owner action |
|---|---|---|---|
| B001 | Initial checkout, historical build, sample data and Linux toolchain inspection. | RESOLVED | V001–V002. Use Docker for Linux verification in this environment. |
| B002 | No license file exists in the inspected checkout. | OPEN | Owner decision before selecting a license or redistributing. Local implementation can continue. |
| B003 | Whether real legacy database files require migration is unknown. | OPEN | Canonical single-WAL export verified. Any owner data in multiple WALs or legacy tables needs separate analysis before migration. |
| B004 | Local GCC ThreadSanitizer executable fails in the Docker/WSL2 environment. | OPEN | V006. Run the native workflow or another Linux host with a compatible TSan runtime. Normal concurrency and ASan results do not establish this gate. |

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

## 9. Current handoff

- **Current milestone:** P3 — compaction, concurrency, snapshots and bounded resources.
- **Next task ID:** P3.5.
- **Next action:** In `src/core/engine.c` and `src/core/disk.c`, introduce a bounded immutable memtable slot and capture active and immutable tables plus a pinned SSTable catalog under `FastDB.mutex`. With injected I/O failure, a failed WAL rotation must keep the current tree active. On a successful rotation, preserve the frozen tree through durable `fdb_flush_l0` publication. Start with one flush worker, reorder output I/O and catalog publication so the mutex is held only for shared state transitions, and defer `fdb_storage_reclaim_orphans` until readers release the retired view. Test a read held across freeze/publication and newer writes masking frozen entries. The sealed-prefix publisher and WAL descriptor rebasing are already verified in `tests/native/flush.c`.
- **Next commands:** Run `make test` and `make test-recovery` with disposable directories after the first engine-level freeze change. Use the compiler and sanitizer commands in [native verification](docs/verification/NATIVE_CHECKPOINT.md). Keep pinned reader and rotation failure tests with the other native recovery fixtures.
- **Last verified good checkpoint:** All three public storage modes, offline canonical-WAL export, serial leveled compaction, internal sealed WAL prefix cuts and the native Docker image. GCC, Clang and ASan/UBSan/LSan each passed 21/21. Docker Release also passed 21/21 and both persistent volume checks passed.
- **Working-tree state:** `main` remains at the original commit. Uncommitted work includes CMake, CLI, native core/storage, tests, documentation and contracts. No owner data was opened in tests.
- **Unresolved blockers:** B002 license decision, B003 uncertainty about real legacy sources, B004 local TSan. P3.5 can proceed locally with normal builds and other sanitizers.
- **Critical limits:** LSM flush and compaction run under the database mutex. A flush can report a compaction failure after the flush is durable. Workers, size-tiered compaction, LSM scans, snapshots, TTL writes, history reclamation, power-loss tests and release-scale verification remain open.

**Completion rule:** If every required task is DONE and every applicable release gate has current PASS evidence, produce the engineering report and give the owner an evidence-based readiness assessment. Otherwise report exactly what remains and continue from the next task in a future requested coding session.
