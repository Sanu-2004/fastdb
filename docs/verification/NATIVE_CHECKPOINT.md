# Native verification, 2026-11-25

Runs used the Debian Linux x86-64 environment described in
[the initial audit](../INITIAL_AUDIT.md). The checkout was mounted read-only
at `/repo` in `fastdb-local-toolchain:bookworm` and the container used
`--network none --workdir /repo`. Each run started in a fresh container
and wrote its build and fixtures under `/tmp`. All builds used project
warnings as errors. The initial baseline fixture and toolchain evidence
are V001–V002 in the ledger.

## GCC and Clang

```sh
CC=gcc cmake -S /repo -B /tmp/fastdb-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/fastdb-gcc --parallel
ctest --test-dir /tmp/fastdb-gcc --output-on-failure

CC=clang cmake -S /repo -B /tmp/fastdb-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/fastdb-clang --parallel
ctest --test-dir /tmp/fastdb-clang --output-on-failure
```

Both runs passed 7 of 7 tests with GCC 12.2.0 and Clang 14.0.6
(V003–V004). The corresponding earlier Make wrapper smoke
`make test BUILD_DIR=/tmp/fastdb-gcc` passed during the P1 and filesystem
checkpoints. The native suites include a 65,536-operation red-black tree
model check with xorshift32 seed `0x420e5b19`, 6,000 concurrent CAS
increments, 2,000 paired writes with coherent scan checks, and allocation
failure at each of the three allocation sites in the exercised batch commit.

Filesystem and recovery tests exercise cross-process directory locking,
large sparse offsets, short reads/writes, EINTR, ENOSPC, fsync/rename failures,
exclusive WAL file creation, segment capacity, sequence continuity, complete
batch replay after uncertain sync, torn headers/payloads and checksum rejection.

## Sanitizers

```sh
CC=gcc cmake -S /repo -B /tmp/fastdb-asan -DFASTDB_SANITIZER=address -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/fastdb-asan --parallel
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/fastdb-asan --output-on-failure

CC=gcc cmake -S /repo -B /tmp/fastdb-tsan -DFASTDB_SANITIZER=thread -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/fastdb-tsan --parallel
ctest --test-dir /tmp/fastdb-tsan -L concurrency --output-on-failure
```

With `-no-pie`, ASan/UBSan/LSan passed 7 of 7 tests (V005). An earlier
ASan/UBSan/LSan PIE run passed four native tests but the CLI script timed
out at 15 seconds. ThreadSanitizer compiled. Its original concurrency test
exited with `FATAL: ThreadSanitizer: unexpected memory mapping`. A retry
configured with `-DCMAKE_EXE_LINKER_FLAGS=-no-pie` ended with SIGSEGV and
provided no race-check result (V006). The GitHub Actions workflow and
release-scale verification have not yet run.

## Install and export

```sh
cmake -S /repo -B /tmp/fastdb-pkg -DBUILD_TESTING=OFF
cmake --build /tmp/fastdb-pkg --parallel
cmake --install /tmp/fastdb-pkg --prefix /tmp/fastdb-install
cmake -S /repo/examples/c -B /tmp/fastdb-example -DCMAKE_PREFIX_PATH=/tmp/fastdb-install
cmake --build /tmp/fastdb-example --parallel
/tmp/fastdb-example/fastdb-example-static
/tmp/fastdb-example/fastdb-example-shared
```

Installation and both consumers of the installed CMake package passed.
Both consumers printed `hello` (V007).

## Persistence checkpoint

The same read-only Docker setup verified the expanded suite after the
manifest, WAL set, SSTables, Bloom filters, serial flush and internal
recovery were integrated.

```sh
make test BUILD_DIR=/tmp/fastdb-gcc
ctest --test-dir /tmp/fastdb-gcc --output-on-failure

CC=clang cmake -S /repo -B /tmp/fastdb-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/fastdb-clang --parallel
ctest --test-dir /tmp/fastdb-clang --output-on-failure

CC=gcc cmake -S /repo -B /tmp/fastdb-asan -DFASTDB_SANITIZER=address -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/fastdb-asan --parallel
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/fastdb-asan --output-on-failure
```

GCC Debug, Clang Release, and GCC ASan/UBSan/LSan passed 13 of 13 tests
each (V008–V010). Storage fixtures verified the following behavior.

- File-ID reservations survive reopen. Failed catalog publication leaves
  the original WAL recoverable and the orphaned ID is skipped.
- Sealed segment damage or absence fails recovery. Only the active segment
  can repair a torn tail.
- SSTable tests check binary bounds, ordering, oversized blocks, cross-block
  revisions, malformed checksummed offsets, short files and injected I/O.
  A Bloom filter found every one of 2,048 inserted binary keys. A corrupt
  partition fell back to the actual block and exposed a corruption count.
- A failed table rename preserved the tree and recoverable WAL. A failed
  directory sync after manifest rename returned an uncertain outcome and
  retained the WAL. Recovery verified SSTables and synced the catalog before
  reclaiming reserved orphan files. A missing referenced table prevented
  reclamation.
- A child process acknowledged a sync batch, then was killed with SIGKILL
  during the next frame. Reopen replayed the complete acknowledged batch,
  discarded the partial frame, and accepted new writes. This covers one
  process-crash boundary.

```sh
cmake -S /repo -B /tmp/fastdb-pkg
cmake --build /tmp/fastdb-pkg --parallel
ctest --test-dir /tmp/fastdb-pkg -L recovery --output-on-failure
cmake --install /tmp/fastdb-pkg --prefix /tmp/fastdb-install
cmake -S /repo/examples/c -B /tmp/fastdb-example -DCMAKE_PREFIX_PATH=/tmp/fastdb-install
cmake --build /tmp/fastdb-example --parallel
/tmp/fastdb-example/fastdb-example-static
/tmp/fastdb-example/fastdb-example-shared
```

The recovery label passed 5 of 5 tests and both installed consumers again
printed `hello` (V011).

Clang 14 static analysis completed without diagnostics for the new native
core, storage and CLI sources (V012). The first invocation used a malformed
command-line version macro. The corrected analyzer run used a string-valued
built-in for that macro.

```sh
for file in src/core/*.c src/storage/*.c cli/*.c; do
  clang --analyze -Xanalyzer -analyzer-output=text -std=c17 \
    -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 \
    -DFASTDB_VERSION_STRING=__FILE__ -Iinclude -Isrc/core "$file"
done
```

## Public persistence and merge checkpoint

The next session verified the unchanged checkpoint first. In the same
Debian container with a read-only repository mount and working directory
`/repo`, `make test BUILD_DIR=/tmp/fastdb-resume` passed 13 of 13 tests
(V013). GCC and Clang each passed 15 of 15 after the initial public LSM
integration (V014–V015). A subsequent build exposed the new disk header
resolving the archived SSTable header. Qualifying the internal include
fixed the build and preserved the separation from the legacy sources.

After adding persistent memory, the exporter and the publication crash
fixtures, all three configurations passed 18 of 18 tests (V016–V018).
The final native run includes streaming merge and output splitting.
GCC Debug, Clang Release, and GCC ASan/UBSan/LSan each passed 19 of 19
tests (V019–V021).

```sh
CC=gcc cmake -S /repo -B /tmp/fastdb-storage-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/fastdb-storage-gcc --parallel
ctest --test-dir /tmp/fastdb-storage-gcc --output-on-failure

CC=clang cmake -S /repo -B /tmp/fastdb-storage-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/fastdb-storage-clang --parallel
ctest --test-dir /tmp/fastdb-storage-clang --output-on-failure

CC=gcc cmake -S /repo -B /tmp/fastdb-storage-asan -DCMAKE_BUILD_TYPE=Debug -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/fastdb-storage-asan --parallel
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/fastdb-storage-asan --output-on-failure
```

- Native LSM tests cover locks, multiple WAL segments, revision checks and
  deletes across disk and the active tree, flush, and reopen. Injected
  read errors propagate as I/O failures. A failed WAL sync poisons the
  handle and recovery sees the batch as a unit.
- Persistent-memory tests cover automatic checkpoints before rotation,
  restore plus ordered WAL replay, an empty checkpoint with a nonzero
  revision, a missing referenced checkpoint, and no implicit eviction.
- The public recovery fixture killed a child at nine publication or
  reclamation boundaries in each persistent mode. All 18 cases retained
  the acknowledged batch, preserved tombstone masking, and accepted a
  subsequent mutation with the next revision. The earlier partial-WAL
  kill fixture also ran. These process-kill cases exercise filesystem
  call boundaries and the kernel's retained state.
- The exporter round-trips a canonical single-WAL fixture into a fresh
  database, checks the source bytes, and refuses multi-WAL, table-based,
  malformed, and incomplete input without publishing an output.
- Two-way and three-way merge tests retain key history, tombstones and
  expiration timestamps, deduplicate matching revisions, and reject a
  conflicting revision or read failure. Split outputs keep all versions
  of a key together. Tests exercise exact workspace and fan-in limits,
  an output-count limit, and a group that exceeds hard metadata capacity.

```sh
for file in /repo/src/core/*.c /repo/src/storage/*.c /repo/cli/*.c; do
  clang --analyze -Xanalyzer -analyzer-output=text -std=c17 \
    -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 \
    -DFASTDB_VERSION_STRING=__FILE__ -I/repo/include -I/repo/src \
    -I/repo/src/core -I/repo/src/storage "$file"
done

cmake -S /repo -B /tmp/fastdb-native-pkg -DBUILD_TESTING=OFF
cmake --build /tmp/fastdb-native-pkg --parallel
cmake --install /tmp/fastdb-native-pkg --prefix /tmp/fastdb-install
cmake -S /repo/examples/c -B /tmp/fastdb-example -DCMAKE_PREFIX_PATH=/tmp/fastdb-install
cmake --build /tmp/fastdb-example --parallel
/tmp/fastdb-example/fastdb-example-static
/tmp/fastdb-example/fastdb-example-shared
```

The analyzer's first run flagged the null backing pointer for an empty
checkpoint's key bounds. Allocating a minimum one-byte backing removed
that path. The final analyzer completed without diagnostics (V022).
Both installed CMake consumers printed `hello` (V023). The existing local
ThreadSanitizer runtime failure remains open.

## Serial compaction, WAL prefix flush, and the native Docker image

GCC reran the starting 19-test checkpoint in `/tmp/resume` (V024).
After compaction was wired into the native library and CLI, the intermediate
GCC suite passed 21 of 21 tests (V025). GCC Debug, Clang Release, and GCC
ASan/UBSan/LSan each passed 21 of 21 with the compaction failure and process
kill cases (V026–V028). The read-only Debian toolchain container used
`/tmp/fastdb-compact-$profile` for those builds. The final rerun, including the
sealed WAL prefix, used the same profiles in `/tmp/fastdb-final-$profile`
and passed 21 of 21 in each case (V031). The V022 Clang static analysis
command completed on the final sources with no diagnostics (V033).

```sh
# In the Debian toolchain container, with /repo mounted read-only.
export ASAN_OPTIONS=detect_leaks=1
for profile in gcc clang asan; do
  compiler=gcc
  type=Debug
  set --
  if test "$profile" = clang; then compiler=clang; type=Release; fi
  if test "$profile" = asan; then
    set -- -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
  fi
  CC="$compiler" cmake -S /repo -B "/tmp/fastdb-final-$profile" -DCMAKE_BUILD_TYPE="$type" "$@"
  cmake --build "/tmp/fastdb-final-$profile" --parallel
  ctest --test-dir "/tmp/fastdb-final-$profile" --output-on-failure
done
```

The compaction planner and integration tests exercise closed binary key
ranges, a two-input staged closure across multiple destination files,
range selection, auto-cascading, disabled mode and workspace exhaustion.
Recovery checks include active WAL continuity through a compaction catalog
edit and tombstone masking after reopen. Fault injection before manifest
rename and at its final directory sync preserved the inputs. A child process
was killed before nine filesystem call boundaries in each of flush,
checkpoint and compaction. All 27 recoveries retained the acknowledged
batch, including its tombstone, and accepted the next revision. These tests
exercise process failure and the kernel's retained state.

The WAL prefix test rotates after revisions 2 and 4, writes revision 5 into
the newer active segment, and flushes only through revision 2. It checks a
failed rotation retry, refusal of an interior segment cut, successful
publication and an uncertain final directory sync. Both successful and
uncertain publication cases reopen with two newer WALs, keep the catalog
high-water, and accept revision 6. The resulting table contains the frozen
prefix and the replay tree contains the later overwrite and tombstone.

Docker Desktop built `fastdb-native:local` for Linux amd64 and passed 21 of
21 Release tests in the image build (V029 and V032). At runtime the
image used UID and GID 10001 and contained the CLI, legacy exporter, header,
static and shared libraries, and CMake package. These are the POSIX shell
equivalents of the Windows smoke commands, using short-lived containers and
two disposable named volumes.

```sh
docker build --progress=plain -t fastdb-native:local .
set -eu
image=fastdb-native:local
lsm=$(docker volume create)
persistent=$(docker volume create)
trap 'docker volume rm "$lsm" "$persistent"' EXIT
test "$(docker run --rm --network none --entrypoint /usr/bin/id "$image" -u)" = 10001
test "$(docker run --rm --network none --entrypoint /usr/bin/id "$image" -g)" = 10001
docker run --rm --network none --entrypoint /bin/sh "$image" -ec 'test -w /data && test -x /usr/local/bin/fastdb-cli && test -x /usr/local/bin/fastdb-legacy-export && test -f /usr/local/include/fastdb/fastdb.h && test -f /usr/local/lib/libfastdb.a && test -f /usr/local/lib/libfastdb.so && test -f /usr/local/lib/cmake/FastDB/FastDBConfig.cmake'
test "$(printf 'put 6b 76616c7565\nflush\ncompact\n' | docker run --rm -i --network none -v "$lsm:/data" "$image")" = "$(printf 'OK\nOK\nOK')"
test "$(printf 'get 6b\n' | docker run --rm -i --network none -v "$lsm:/data" "$image")" = 'VALUE 1 76616c7565'
test "$(printf 'put 6b 00ff\ncheckpoint\nput 77616c -\n' | docker run --rm -i --network none -v "$persistent:/data" "$image" --memory-persistent /data)" = "$(printf 'OK\nOK\nOK')"
test "$(printf 'get 6b\nget 77616c\n' | docker run --rm -i --network none -v "$persistent:/data" "$image" --memory-persistent /data)" = "$(printf 'VALUE 1 00ff\nVALUE 2 -')"
```

## Background flush worker

V034 reran the starting checkpoint in `/tmp/fastdb-resume` with GCC Debug
and passed 21 of 21. V035 initially failed the memory-mode 512-byte budget
test because every handle carried the worker state. A separate allocation
on enabled LSM handles restored the previous test behavior without changing
the assertions. V036 used the same GCC, Clang and ASan/UBSan/LSan commands
above with `fastdb-bg-$profile` as the build directory. Each profile passed
22 of 22.

`fastdb_background_flush` pauses the worker at SSTable construction while
the foreground checks a frozen revision with CAS, writes a tombstone, and
rotates the newer WAL. It also checks saturation at one frozen tree,
explicit retry after ENOSPC, safe active-tree retention after a failed WAL
rotation, and clean close during output construction. A subprocess case
acknowledges both WAL ranges, kills the process during construction and
reopens with all seven revisions and the tombstone intact.

## Pinned SSTable reads

V037 used the same compiler and sanitizer matrix in `/tmp/fastdb-pin-$profile`.
Each profile passed 23 of 23. The pinned reads paused before opening the
referenced table. Compaction replaced the catalog, but obsolete names
remained available until the read released its view and maintenance resumed.
Both an old value and a masking tombstone were covered. A separate case
verified that a background flush publishes while a reader is pinned and
that subsequent background work waits for the pin to release.

```sh
ctest --test-dir /tmp/fastdb-pin-gcc -L concurrency --repeat until-fail:20 --output-on-failure
CC=gcc cmake -S /repo -B /tmp/fastdb-pin-tsan -DCMAKE_BUILD_TYPE=Debug -DFASTDB_SANITIZER=thread
cmake --build /tmp/fastdb-pin-tsan --parallel
ctest --test-dir /tmp/fastdb-pin-tsan -L concurrency --output-on-failure
```

V038 completed 20 repetitions of each of the three concurrency suites.
V039 built the TSan binaries, but the runtime aborted before test code with
`unexpected memory mapping` for every suite. The first invocation of the
matrix and TSan commands used a regular expression that Windows argument
marshalling split. The corrected commands use the CTest label.

V040 rebuilt the native image and reran the volume restart checks above.
The image ran as UID/GID 10001. Release CTest passed 23 of 23. Both LSM and
persistent memory recovered the expected values from fresh containers.

## Compaction output ID reservation

V041 reran the GCC Debug, Clang Release and GCC ASan/UBSan/LSan matrix in
`/tmp/fastdb-reserve-$profile`. All three passed 23 of 23. The manifest
fixture checks refusal of an empty reservation, failure before rename,
uncertainty after rename, the next allocation after reopen and exhaustion
at the unsigned 64-bit boundary. A native compaction with a one-byte soft
output target creates three tables, advances the manifest by one reservation
and one installation, and recovers all three values across reopen.

V042 rebuilt the native image with 23 of 23 tests passing. The same
temporary LSM volume was mounted by separate `docker run --rm -i --network
none` invocations. The first ran `put 6b 6f6c64`, `flush`, `put 6b 6e6577`,
`flush`, and `compact`. The second ran `get 6b` and returned
`VALUE 2 6e6577`. The image ran as UID/GID 10001.

V043 ran the V022 Clang analysis command over the final core, storage and
CLI sources. It exited successfully with no diagnostics.

## Detached manual compaction merge

V044 ran GCC and CTest from the previous checkpoint in `/tmp/resume` and
passed 23 of 23. V045 reran the compiler and sanitizer matrix above with
`/tmp/p36-$profile` as the build directory and passed 23 of 23 per profile.
The new concurrency case stops before the first output-table write,
performs a disk lookup, writes enough to rotate the WAL, and verifies
`FASTDB_BUSY` for competing flush, compaction and close. It resumes the
merge, writes again, and reopens to verify the new WAL data and masking
tombstone.

## Background compaction scheduling

V046 rebuilt the clean `b8b3a71` starting checkpoint with GCC Debug and passed
23 of 23 tests. V047 ran the full GCC Debug, Clang Release, and GCC
ASan/UBSan/LSan matrix in a single disposable
`fastdb-local-toolchain:bookworm` container with the repository bind-mounted
read-only. Each configuration passed 23 of 23 tests. The expanded
`fastdb_background_flush` case waits for the worker's completed sealed
sequence, triggers compaction with an L0 threshold of two, checks one L0 and
one L1 table after the single automatic pass, then reads the same values after
reopen. The existing shutdown fixture checks close during flush output
construction. At V047, direct close during queued/running compaction had not
yet been tested; V050 adds a gated close during an active detached merge.
Queued-before-start cancellation still lacks a dedicated deterministic test.
TSan was not rerun because B004 remains active; this result is not a
race-detector result.

V048 ran `ctest --test-dir /tmp/p36-repeat -L concurrency --repeat
until-fail:20 --output-on-failure` after a GCC Debug build. Each of the three
concurrency-labeled suites passed 20 consecutive repetitions.

V049 reran the full GCC Debug build and CTest after a final guard adjustment
in the new fixture. All 23 tests passed.

## Concurrent L0 publication during background compaction

V050 ran the full native matrix from a disposable Linux container with the
source tree mounted read-only. GCC 12.2 Debug, Clang 14 Release, and GCC
ASan/UBSan/LSan each configured and built with the project CMake files, then
passed all 23 CTest cases. The expanded background fixture gates the worker
after it releases the database mutex, publishes a new L0 table while the
merge is detached, and checks catalog/data after close and reopen. It also
injects output EIO, checks that source tables remain authoritative, retries
maintenance, and verifies recovery after reopen. The test exercises the
close path while an active background merge drains. It does not separately
gate cancellation while a compaction is queued but has not started.

V051 ran
`ctest --test-dir /tmp/p36-repeat -L concurrency --repeat until-fail:20 --output-on-failure`.
All three concurrency-labeled suites passed 20 consecutive repetitions
(60 suite executions total).

V052 ran Clang static analysis over `src/core/*.c`, `src/storage/*.c`, and
`cli/*.c` with `-std=c17`, the POSIX and 64-bit-offset defines, and project
include paths (`include`, `src`, `src/core`, `src/storage`). It exited 0 with
no diagnostics.

V053 built `fastdb-native:local` with
`docker build --progress=plain -t fastdb-native:local .`. The image build's
Release CTest passed 23/23. A temporary named volume was shared by separate
containers: one performed `put 6b 76616c7565`, `flush`, and `compact`; the
next returned `VALUE 1 76616c7565` for `get 6b`. The image ran as UID/GID
10001. The volume was removed after the check. Image ID:
`sha256:5599f0b9eb81ca2d942324a3eebd525b17468954d78c1d2b889fc6b2184ab77c`
(linux/amd64).

These tests establish the exercised interleavings and process-level restart
behavior only. They do not establish TSan, physical power-loss behavior,
quota-aware free-space reservation, or release-scale resource bounds.

## Configurable free-space admission (initial P3.7 slice)

V054 ran `docker build -t fastdb-native:local .` with the legacy Docker
builder. GCC 12.2 configured and compiled the Release image, and all 23 CTest
cases passed. The filesystem fixture exercises an injected `statvfs`
availability value and EIO. The public persistent LSM fixture sets a 1 KiB
`min_free_bytes`, verifies a failed query returns `FASTDB_IO_ERROR` without
advancing the revision, verifies insufficient headroom returns
`FASTDB_LIMIT_EXCEEDED` without a mutation, verifies the write succeeds once
headroom is restored, then verifies flush refusal preserves the value and
flush retry succeeds. The estimate includes frame bytes, rotation segment
header and max manifest image for WAL admission; record-based table estimate
and max manifest image for flush/checkpoint; twice selected input bytes and
max manifest image for compaction.

During development, one Docker build failed under warnings-as-errors because
test fault structs had partial positional initializers after being extended.
The next CTest run exposed an EIO fixture that had not set its failure call
count. Both fixture defects were corrected; the final complete build/test
command above passed. Clang, ASan/UBSan/LSan, TSan, and headroom arithmetic
boundary tests were not run for V054. The query is advisory and does not
reserve blocks or account for all quotas/metadata or concurrent outside
writers.

## Headroom boundary and compaction admission

V055 ran the GCC Debug, Clang Release, and GCC ASan/UBSan/LSan matrix in the
local `fastdb-local-toolchain:bookworm` container, with the repository mounted
read-only at `/repo`; each build and complete CTest passed 23/23. The same
working tree was built into `fastdb-native:local`; its GCC 12.2 Release CTest
also passed 23/23.

The expanded `fastdb_lsm_fault` case verifies that low-space compaction
returns `FASTDB_LIMIT_EXCEEDED` while keeping the published SSTable file count
unchanged, and that compaction succeeds after the injected available space is
restored. It also configures `min_free_bytes=UINT64_MAX` and confirms a write
is rejected without advancing the sequence. The test was first placed before
the initial SSTable flush, where the engine correctly had no compaction work;
the assertion was moved after flush. Source review also found an uninitialized
reservation local when `fdb_wal_frame_size` returned an error; it is now
initialized only from a successful result.

Exact matrix commands (all build directories were inside the disposable
container):

```sh
CC=gcc cmake -S /repo -B /tmp/p37-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p37-gcc --parallel
ctest --test-dir /tmp/p37-gcc --output-on-failure

CC=clang cmake -S /repo -B /tmp/p37-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/p37-clang --parallel
ctest --test-dir /tmp/p37-clang --output-on-failure

CC=gcc cmake -S /repo -B /tmp/p37-asan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p37-asan --parallel
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/p37-asan --output-on-failure
```

## Per-database compaction descriptor budget

V056 began from commit `2963f5c`. The source tree was mounted read-only at
`/repo` in `fastdb-local-toolchain:bookworm`; GCC 12.2 Debug, Clang 14
Release, and GCC ASan/UBSan/LSan each built and passed the full 23-test
CTest suite. Build directories were `/tmp/p37fd-gcc`, `/tmp/p37fd-clang`,
and `/tmp/p37fd-asan` respectively. The `fastdb_lsm_fault` regression checks
that persistent-memory `max_open_files` 3 is rejected and 4 accepted, LSM 7
is rejected and 8 accepted, and fan-in is clamped correctly after six
reserved descriptors.

The final source tree was also built with `docker build -t
fastdb-native:local .`. The Dockerfile's GCC 12.2 Release CTest passed 23/23
before the image was installed. The build succeeded; Docker reported a
non-fatal warning that `git.exe` was unavailable for build metadata. The
six-slot reservation is documented in `docs/CONFIGURATION.md`; the option
limits compaction readers per database and is not a process-wide descriptor
limit. TSan remains unavailable as recorded under blocker B004.

## Background compaction error on close

V057 added a deterministic fault-injection case to
`tests/native/background_flush.c`. It creates two L0 tables, blocks the
background merge at output write, injects EIO, then leaves the error
unretried. `fastdb_close` must return `FASTDB_IO_ERROR`; a subsequent open
must read values from the original L0 files. This validates the documented
consuming-close/error-reporting behavior and recovery of authoritative
inputs.

The focused GCC Debug run of `fastdb_background_flush` passed 1/1. The full
GCC 12.2 Debug, Clang 14 Release, and GCC ASan/UBSan/LSan suites passed 23/23
each in the read-only `fastdb-local-toolchain:bookworm` container. The same
working tree built with `docker build -t fastdb-native:local .`; the
Dockerfile's GCC 12.2 Release CTest passed 23/23. Build paths inside the
Linux container were `/tmp/p37-close-error`, `/tmp/p37c-gcc`,
`/tmp/p37c-clang`, and `/tmp/p37c-asan`. No engine code change was needed;
this adds evidence for the existing error and recovery contract. TSan was
not rerun because B004 remains open.

## Concurrent compaction and foreground-flush descriptor budget

V058 added descriptor counting to the gated overlap in
`tests/native/background_flush.c`. The test counts `/proc/self/fd` entries,
excludes the enumeration handle, subtracts the post-open baseline, and adds
the three base descriptors owned by the database (directory, lock and active
WAL). It runs background compaction while publishing a foreground L0 flush
with `max_open_files=10` and asserts the observed per-database high-water
stays within that budget.

The first run used a budget of 8 and measured 10 descriptors. The missing
slots were a replacement WAL during rotation and a temporary manifest during
catalog publication, which can overlap compaction readers/output and the
foreground flush writer. The reservation is now eight descriptors, and LSM
rejects budgets below 10 so a two-reader merge remains possible. The setting
is still a compaction fan-in budget, not a hard cap across every recovery or
open path and not a process-wide cap.

Focused GCC Debug runs of `fastdb_background_flush` and `fastdb_lsm_fault`
passed 1/1 each. GCC 12.2 Debug, Clang 14 Release and GCC ASan/UBSan/LSan
full CTest passed 23/23 each using `/tmp/p37fd2-gcc`, `/tmp/p37fd2-clang`,
and `/tmp/p37fd2-asan` in the read-only toolchain container. The Docker
Release build also passed 23/23. The failed 8-FD assertion was reproduced
before the correction; all post-correction checks passed.

## Frozen-slot backpressure retry

V059 extends `tests/native/background_flush.c`: while the immutable slot is
occupied, the existing gated writer observes `FASTDB_BUSY`; after flush
publication releases the slot, `concurrent_build` successfully writes key
`q` at revision 8, deletes it, and confirms that it remains absent after
close/reopen. The fixture's injected ENOSPC mode additionally exercises the
successful retry route after the failed maintenance attempt. This adds
verification for the admission boundary; it did not require an engine code
change.

Focused command (Linux toolchain image, source mounted read-only at `/repo`):

```sh
CC=gcc cmake -S /repo -B /tmp/p37-queue-retry -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p37-queue-retry --parallel
ctest --test-dir /tmp/p37-queue-retry -R fastdb_background_flush --output-on-failure
```

Focused result: 1/1 passed. Full GCC Debug (`/tmp/p37q-gcc`), Clang Release
(`/tmp/p37q-clang`), and GCC ASan/UBSan/LSan (`/tmp/p37q-asan`, leak
detection enabled) each passed 23/23. These were run in
`fastdb-local-toolchain:bookworm` with:

```sh
set -e
CC=gcc cmake -S /repo -B /tmp/p37q-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p37q-gcc --parallel
ctest --test-dir /tmp/p37q-gcc --output-on-failure
CC=clang cmake -S /repo -B /tmp/p37q-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/p37q-clang --parallel
ctest --test-dir /tmp/p37q-clang --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p37q-asan -DCMAKE_BUILD_TYPE=Debug -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p37q-asan --parallel
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/p37q-asan --output-on-failure
```

Finally, `docker build -t fastdb-native:local .` succeeded and the Dockerfile's
GCC 12.2 Release CTest passed 23/23. TSan was not run; see blocker B004 in
`PROGRESS.md`.
