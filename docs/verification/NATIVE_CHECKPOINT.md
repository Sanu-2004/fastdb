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

## Frozen-slot atomic batch backpressure

V060 extends `tests/native/background_flush.c`. While the immutable slot is
occupied and the active tree is at the flush threshold, a two-mutation batch
(put `q`, put `r`) returns `FASTDB_BUSY`; both keys are checked absent and the
existing stats assertion confirms the prior seven-record/revision-7 view is
unchanged. The fixture retains that same batch handle across `fastdb_flush`,
retries it successfully after the frozen slot is released, verifies revisions
8 and 9, deletes `q`, and checks after reopen that `q` remains absent and `r`
remains at revision 9. Both normal worker publication and the injected ENOSPC
retry path execute this sequence. No engine change was needed; no partial
batch commit was observed.

Focused GCC Debug test: `fastdb_background_flush`, 1/1 passed. Full GCC Debug,
Clang Release, and GCC ASan/UBSan/LSan CMake builds and CTest each passed 23/23
in `fastdb-local-toolchain:bookworm`, using source mounted read-only at `/repo`
and build directories `/tmp/p37b-gcc`, `/tmp/p37b-clang`, `/tmp/p37b-asan`.
The command sequence was:

```sh
set -e
CC=gcc cmake -S /repo -B /tmp/p37b-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p37b-gcc --parallel
ctest --test-dir /tmp/p37b-gcc --output-on-failure
CC=clang cmake -S /repo -B /tmp/p37b-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/p37b-clang --parallel
ctest --test-dir /tmp/p37b-clang --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p37b-asan -DCMAKE_BUILD_TYPE=Debug -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p37b-asan --parallel
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/p37b-asan --output-on-failure
```

`docker build -t fastdb-native:local .` succeeded. Its GCC 12.2 Release build
and CTest passed 23/23. Docker emitted only the known warning that `git.exe`
was unavailable for build metadata. TSan was not run due to B004.

## Aggregate active/frozen memory accounting assertion

V061 strengthens `tests/native/background_flush.c` during the gated worker flush.
While the immutable and active trees both hold records, it checks exact reported
charged bytes against the handle, active tree, frozen tree, iterator allocation,
and reserved disk workspace. The existing budget assertion also requires total
charged bytes to remain at or below 128 KiB. This is an accounting consistency
check, not a proof of process RSS or an actual max-budget saturation test.

Using `fastdb-local-toolchain:bookworm` with the repository mounted read-only:
GCC Debug configure/build in `/tmp/p37mem-gcc` succeeded; focused
`fastdb_background_flush` passed 1/1 and full CTest passed 23/23. Two initial
compile attempts failed while constructing the internal accounting expression:
one lacked the `disk.h` declaration and the next could not resolve that header's
storage-relative include. The test was simplified to the active/frozen/workspace
components directly observable through its existing headers; the final build
and both test runs passed. Clang, sanitizers, Docker, and TSan were not run on
this V061 source change. TSan remains blocked by B004.

## ThreadSanitizer Docker ASLR workaround (V062)

The default Docker invocation reproduced `FATAL: ThreadSanitizer: unexpected
memory mapping`. `setarch x86_64 -R` was denied with Docker's default seccomp
profile (`Operation not permitted`). Rebuilding/running in a disposable
container with `--security-opt seccomp=unconfined` allowed the process
personality change; with ASLR disabled for the process, the TSan test ran
successfully. No project source or compiler flags were changed to obtain this
result.

Exact command (GCC 12.2, read-only source mount):

```sh
docker run --rm --network none --security-opt seccomp=unconfined \
  -v D:/codex/fastdb:/repo:ro --workdir /repo fastdb-local-toolchain:bookworm \
  sh -lc 'set -e; CC=gcc cmake -S /repo -B /tmp/tsan-p37 -DCMAKE_BUILD_TYPE=Debug -DFASTDB_SANITIZER=thread -DCMAKE_EXE_LINKER_FLAGS=-no-pie; cmake --build /tmp/tsan-p37 --parallel; setarch x86_64 -R ctest --test-dir /tmp/tsan-p37 -L concurrency --output-on-failure'
```

Result: build passed; `fastdb_concurrency`, `fastdb_background_flush`, and
`fastdb_read_view` passed under TSan, 3/3. Initial baseline behavior and the
seccomp restriction were also reproduced. This resolves the local runtime
blocker for this Linux x86-64 Docker setup. It does not constitute a full TSan
run of every non-concurrency test or coverage on other hosts/platforms.

V063 repeated the same three concurrency-labeled CTests 20 times each under
the workaround using `/tmp/tsan-repeat`:

```sh
docker run --rm --network none --security-opt seccomp=unconfined \
  -v D:/codex/fastdb:/repo:ro --workdir /repo fastdb-local-toolchain:bookworm \
  sh -lc 'set -e; CC=gcc cmake -S /repo -B /tmp/tsan-repeat -DCMAKE_BUILD_TYPE=Debug -DFASTDB_SANITIZER=thread -DCMAKE_EXE_LINKER_FLAGS=-no-pie >/dev/null; cmake --build /tmp/tsan-repeat --parallel >/dev/null; setarch x86_64 -R ctest --test-dir /tmp/tsan-repeat -L concurrency --repeat until-fail:20 --output-on-failure'
```

Build passed; all three suites completed 20 repetitions (60/60 suite runs)
with no TSan diagnostics. This is repeatability evidence for the same host and
runtime setup, not cross-platform race-safety proof.

## Live memory saturation and compaction fan-in (V064)

`aggregate_memory_limit` gates the worker with three frozen records and sets
the memory limit to the empty handle's accounted bytes plus four nodes and
one WAL frame. The first active write fits that boundary. A second write and
a two-operation batch return `FASTDB_LIMIT_EXCEEDED` while the active tree is
below its flush threshold. The revision, record count, charged bytes and
visibility stay unchanged on refusal. Publication releases three nodes, the
same batch succeeds, and reopen verifies the committed keys and revisions.

`workspace_limits_fan_in` opens one L0 table overlapping two L1 tables with
`max_working_bytes` one byte below the budget for three inputs and their
selection arrays. The original planner chose three inputs and
`fastdb_compact` returned `FASTDB_LIMIT_EXCEEDED` in
`/tmp/p37-plan-repro`. Charging the index and file ID per input before
planning lets the two-input stages publish and reopen with the new value
and the tombstone intact.

The Linux commands below ran inside `fastdb-local-toolchain:bookworm` with
`--network none` and the D: workspace bind mounted read-only at `/repo`.

```sh
CC=gcc cmake -S /repo -B /tmp/p37-plan-repro -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p37-plan-repro --target fastdb_compaction --parallel
ctest --test-dir /tmp/p37-plan-repro -R '^fastdb_compaction$' --output-on-failure

CC=gcc cmake -S /repo -B /tmp/p37-plan-fixed -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p37-plan-fixed --parallel
ctest --test-dir /tmp/p37-plan-fixed -R '^fastdb_compaction$' --output-on-failure
ctest --test-dir /tmp/p37-plan-fixed -R '^fastdb_background_flush$' --output-on-failure

CC=gcc cmake -S /repo -B /tmp/p37-final-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p37-final-gcc --parallel
ctest --test-dir /tmp/p37-final-gcc --output-on-failure

CC=clang cmake -S /repo -B /tmp/p37-final-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/p37-final-clang --parallel
ctest --test-dir /tmp/p37-final-clang --output-on-failure

CC=gcc cmake -S /repo -B /tmp/p37-final-asan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p37-final-asan --parallel
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/p37-final-asan --output-on-failure
```

The two focused tests passed 1/1 each after the fix. GCC Debug, Clang Release,
and GCC ASan/UBSan with leak detection each passed 23/23. The first combined
focused regex was rejected by the container shell after Windows argument
quoting. The two separate CTest invocations above passed.

Using the disposable container and V062's `--security-opt seccomp=unconfined`
setting, GCC TSan also passed the concurrency label 3/3:

```sh
CC=gcc cmake -S /repo -B /tmp/p37-final-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=thread -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p37-final-tsan --parallel
setarch x86_64 -R ctest --test-dir /tmp/p37-final-tsan -L concurrency --output-on-failure
```

`docker build -t fastdb-native:local .` succeeded with GCC Release CTest
23/23. Docker warned that `git.exe` was unavailable for build metadata.

## Persistent memory descriptor minimum (V065)

The fault fixture samples Linux `/proc/self/fd` from filesystem callbacks,
subtracting the pre-open baseline and excluding the enumeration descriptor.
With a 410-byte segment limit, two 181-byte frames fit after the 40-byte
header. The third write rotates the WAL. The fixture observed five
descriptors at rotation, then the original validation accepted a
four-descriptor configuration when the regression expected rejection.
An earlier fixture with 128-byte values rotated on the second write.
Changing to 120-byte values aligned the rotation with the probe reset.

The corrected setting rejects 3 and 4 and accepts 5. The probe checks that
checkpoint, new WAL append and reopen stay within five descriptors, and
that close returns to the pre-open count. Reopen checks both checkpoint
values and the latest WAL write. The fixture covers persistent memory on
Linux and samples the filesystem callback boundaries.

These commands ran in the V064 toolchain container with the source mounted
read-only:

```sh
CC=gcc cmake -S /repo -B /tmp/p37-fd-repro -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p37-fd-repro --target fastdb_lsm_fault --parallel
ctest --test-dir /tmp/p37-fd-repro -R '^fastdb_lsm_fault$' --output-on-failure

CC=gcc cmake -S /repo -B /tmp/p37-fd-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p37-fd-gcc --parallel
ctest --test-dir /tmp/p37-fd-gcc -R '^fastdb_lsm_fault$' --output-on-failure
ctest --test-dir /tmp/p37-fd-gcc --output-on-failure

CC=clang cmake -S /repo -B /tmp/p37-fd-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/p37-fd-clang --parallel
ctest --test-dir /tmp/p37-fd-clang --output-on-failure

CC=gcc cmake -S /repo -B /tmp/p37-fd-asan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p37-fd-asan --parallel
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/p37-fd-asan --output-on-failure
```

The focused test passed 1/1 with the fix. GCC Debug, Clang Release and
GCC ASan/UBSan with leak detection passed 23/23 each.
`docker build -t fastdb-native:local .` succeeded and its Release CTest
passed 23/23. GCC TSan passed the concurrency label 3/3 with the V062
container settings:

```sh
CC=gcc cmake -S /repo -B /tmp/p37-fd-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=thread -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p37-fd-tsan --parallel
setarch x86_64 -R ctest --test-dir /tmp/p37-fd-tsan -L concurrency --output-on-failure
```

## P3.7 completion: LSM descriptor peaks (V066)

The recovery test reopens a table plus sealed and active WALs with an orphan
temporary SSTable. The filesystem callback samples four database descriptors
with the directory stream open during cleanup. Reads find the table and all
WAL records at their original revisions. Close returns to the FD baseline.

The overlap test seeds an L1 table and two L0 tables so automatic compaction
holds two inputs and one output open. A point read pauses after opening its
table. Foreground puts then rotate the WAL and flush a fresh L0. The probe
samples at least nine and at most ten descriptors. While the read is pinned,
a second flush returns BUSY because reclamation is pending. Close drains the
compaction after the reader is released, and reopen verifies the newest
values. An earlier fixture selected a merge with one input and failed the
overlap lower-bound assertion. The two-input fixture passed both focused
tests 1/1 each.

Commands ran with the V064 Docker toolchain and read-only source mount:

```sh
CC=gcc cmake -S /repo -B /tmp/p37-close -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p37-close --parallel
ctest --test-dir /tmp/p37-close -R '^fastdb_background_flush$' --output-on-failure
ctest --test-dir /tmp/p37-close -R '^fastdb_lsm_fault$' --output-on-failure

CC=gcc cmake -S /repo -B /tmp/p37-done-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p37-done-gcc --parallel
ctest --test-dir /tmp/p37-done-gcc --output-on-failure
CC=clang cmake -S /repo -B /tmp/p37-done-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/p37-done-clang --parallel
ctest --test-dir /tmp/p37-done-clang --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p37-done-asan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p37-done-asan --parallel
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/p37-done-asan --output-on-failure
```

Each full suite passed 23/23. `docker build -t fastdb-native:local .`
passed its Release tests 23/23. With the V062 TSan setup, the three concurrency
tests passed five repetitions each (15 executions):

```sh
CC=gcc cmake -S /repo -B /tmp/p37-done-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=thread -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p37-done-tsan --parallel
setarch x86_64 -R ctest --test-dir /tmp/p37-done-tsan -L concurrency --repeat until-fail:5 --output-on-failure
```

## First P3.8 snapshot checkpoint (V067)

The shared-library integration test captures point-read snapshots in both
memory modes. It checks binary keys and empty values across a subsequent
atomic batch and checkpoint, then reopens to verify current data. In memory
mode it checks capture at the exact memory boundary and refusal one byte
below it, slot exhaustion, write backpressure, release accounting and 512
atomic pair captures alongside a batch writer. LSM capture explicitly
reports `FASTDB_UNSUPPORTED`. The wrapped-allocation test injects failure
at the snapshot handle, each of two copied nodes, and the owned GET result.

The commands ran in the V064 toolchain container with a read-only bind.

```sh
CC=gcc cmake -S /repo -B /tmp/p38-first -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p38-first --parallel
ctest --test-dir /tmp/p38-first -R '^fastdb_snapshot$' --output-on-failure
ctest --test-dir /tmp/p38-first -R '^fastdb_alloc_failure$' --output-on-failure
ctest --test-dir /tmp/p38-first --output-on-failure
CC=clang cmake -S /repo -B /tmp/p38-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/p38-clang --parallel
ctest --test-dir /tmp/p38-clang --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p38-asan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p38-asan --parallel
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/p38-asan --output-on-failure
```

Both focused tests passed 1/1. Each full suite passed 24/24.
`docker build -t fastdb-native:local .` passed its Release tests 24/24.
With the V062 TSan setup, all four concurrency tests passed five repetitions
each (20 executions).

```sh
CC=gcc cmake -S /repo -B /tmp/p38-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=thread -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p38-tsan --parallel
setarch x86_64 -R ctest --test-dir /tmp/p38-tsan -L concurrency --repeat until-fail:5 --output-on-failure
```

## P3.9 TTL and purge checkpoint (V069)

`fastdb_ttl` failed to link against the pre-TTL engine in `/tmp/p39-red`.
The new case passed in `/tmp/p39-green`, but the initial full run failed
`fastdb_memory` because test clock fields increased every handle's size.
Moved the controlled clock to the test binary via linker wrapping and
preserved the existing 512-byte fixture. Also corrected a test symbol
collision with the C library's `clock` and updated the fault fixture to check
the WAL's conservative uncertain result after an append error.

The final suite verifies the exact expiration boundary, no resurrection of
older LSM values, retained snapshot time, batch deadline calculation and
overflow rollback, clock errors, WAL replay and checkpoint preservation.
An injected failure on the second purge append reports one confirmed
deletion. Reopen checks the first deletion and the retained second record
before retrying. The test uses disposable data directories.

```sh
CC=gcc cmake -S /repo -B /tmp/p39-red -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p39-red --target fastdb_ttl --parallel

CC=gcc cmake -S /repo -B /tmp/p39-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p39-gcc --parallel
ctest --test-dir /tmp/p39-gcc -R '^fastdb_ttl$' --output-on-failure
ctest --test-dir /tmp/p39-gcc --output-on-failure
CC=clang cmake -S /repo -B /tmp/p39-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/p39-clang --parallel
ctest --test-dir /tmp/p39-clang --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p39-asan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p39-asan --parallel
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/p39-asan --output-on-failure
```

The commands ran inside the V064 toolchain container. The focused TTL
test passed 1/1 and the final full profiles passed 25/25 each. `docker build
-t fastdb-native:local .` succeeded with Release CTest 25/25. The V062
TSan setup ran all four concurrency tests five times each (20 executions).

```sh
CC=gcc cmake -S /repo -B /tmp/p39-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=thread -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p39-tsan --parallel
setarch x86_64 -R ctest --test-dir /tmp/p39-tsan -L concurrency --repeat until-fail:5 --output-on-failure
```

## P3.9 completion with opt-in FIFO, LRU and LFU (V070)

The eviction tests were added before implementing the engine API. Building
`fastdb_eviction` in `/tmp/p39-eviction-red` compiled the new fixture and
failed to link the missing `fastdb_options_set_eviction` symbol.

The final cases verify policy order, replacement behavior, persistent-mode
rejection, snapshot retention, live capacity limits, hard staging memory
admission and allocation failure on the second replacement node. All
existing assertions, including the 512-byte default memory fixture, pass.
The commands ran in the V064 Docker toolchain with a read-only source bind.

```sh
CC=gcc cmake -S /repo -B /tmp/p39-eviction-red -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p39-eviction-red --target fastdb_ttl fastdb_eviction --parallel
CC=gcc cmake -S /repo -B /tmp/p39-eviction-green -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p39-eviction-green --parallel
ctest --test-dir /tmp/p39-eviction-green -R '^fastdb_eviction$' --output-on-failure
ctest --test-dir /tmp/p39-eviction-green -R '^fastdb_alloc_failure$' --output-on-failure
ctest --test-dir /tmp/p39-eviction-green -R '^fastdb_ttl$' --output-on-failure
ctest --test-dir /tmp/p39-eviction-green --output-on-failure
CC=clang cmake -S /repo -B /tmp/p39-eviction-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/p39-eviction-clang --parallel
ctest --test-dir /tmp/p39-eviction-clang --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p39-eviction-asan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p39-eviction-asan --parallel
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/p39-eviction-asan --output-on-failure
```

Each focused case passed 1/1 and each full profile passed 26/26. The
Docker image rebuilt with `docker build -t fastdb-native:local .` and its
Release suite passed 26/26. GCC TSan with the V062 Docker workaround passed
four concurrency suites five times each.

```sh
CC=gcc cmake -S /repo -B /tmp/p39-eviction-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=thread -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p39-eviction-tsan --parallel
setarch x86_64 -R ctest --test-dir /tmp/p39-eviction-tsan -L concurrency --repeat until-fail:5 --output-on-failure
```

## First P3.10 checkpoint with shadowed PUT collection (V071)

The new regression held a pre-update snapshot through compaction, checked
the live tombstone, released the snapshot, and inspected the finished
table. The original engine failed the assertion for three output records.
The revised output contains the newest PUT, the unaffected key and the
tombstone. It retains the highest input sequence of 5. Reopen still
masks the deleted key and reinsertion receives revision 6.

The first implementation build required a designated initializer for the
new internal merge option. The existing merge fixture still checks its
original history-retaining behavior. These commands ran in the V064
toolchain container with the source mounted read-only.

```sh
CC=gcc cmake -S /repo -B /tmp/p310-red -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p310-red --target fastdb_compaction --parallel
ctest --test-dir /tmp/p310-red -R '^fastdb_compaction$' --output-on-failure

CC=gcc cmake -S /repo -B /tmp/p310-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p310-gcc --parallel
ctest --test-dir /tmp/p310-gcc -R '^fastdb_compaction$' --output-on-failure
ctest --test-dir /tmp/p310-gcc --output-on-failure
CC=clang cmake -S /repo -B /tmp/p310-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/p310-clang --parallel
ctest --test-dir /tmp/p310-clang --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p310-asan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p310-asan --parallel
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/p310-asan --output-on-failure
```

The final focus test passed 1/1. Each full profile passed 26/26, as did
the Release tests in `docker build -t fastdb-native:local .`. GCC TSan using
the V062 Docker workaround passed four concurrency suites five times each.

```sh
CC=gcc cmake -S /repo -B /tmp/p310-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=thread -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p310-tsan --parallel
setarch x86_64 -R ctest --test-dir /tmp/p310-tsan -L concurrency --repeat until-fail:5 --output-on-failure
```

## P3.10 completion with safe bottom-level DELETE reclamation (V072)

Starting at `8eeaee2`, the test-first GCC Debug compaction run failed
the exact output-count assertion. A full merge containing the older PUT
still kept its eligible DELETE. The final cases verify that the range job
keeps a DELETE while an unselected SSTable can contain the older value,
and that the full merge reclaims the whole key group. A three-DELETE
input leaves the highest revision as the sole output record and allocates
the next revision after reopen. Tests also hold a snapshot before the job
and capture another during detached output. Those snapshots preserve the
original visibility and the latter snapshot keeps the original files
through publication.

The focused `fastdb_compaction`, `fastdb_read_view`, and
`fastdb_public_crash` targets each passed 1/1. The public crash target
exercised 27 process-kill cases across flush, checkpoint, and compaction,
including a compaction with a reclaimable DELETE. This exercises process
termination and restart on the Docker filesystem.

The following full profiles ran in the V064 toolchain container with the
source mounted read-only. GCC Debug, Clang Release and ASan/UBSan with leak
detection each passed 26/26. The native Dockerfile Release build passed
26/26 and produced `fastdb-native:local`. TSan with the V062 Docker
workaround passed four concurrency targets five times each.

```sh
CC=gcc cmake -S /repo -B /tmp/p310-mask-red -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p310-mask-red --target fastdb_compaction --parallel
ctest --test-dir /tmp/p310-mask-red -R '^fastdb_compaction$' --output-on-failure

CC=gcc cmake -S /repo -B /tmp/p310-mask-green -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p310-mask-green --parallel
ctest --test-dir /tmp/p310-mask-green -R '^fastdb_compaction$' --output-on-failure
ctest --test-dir /tmp/p310-mask-green -R '^fastdb_read_view$' --output-on-failure
ctest --test-dir /tmp/p310-mask-green -R '^fastdb_public_crash$' --output-on-failure

CC=gcc cmake -S /repo -B /tmp/p310-mask-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p310-mask-gcc --parallel 6
ctest --test-dir /tmp/p310-mask-gcc --output-on-failure
CC=clang cmake -S /repo -B /tmp/p310-mask-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/p310-mask-clang --parallel 6
ctest --test-dir /tmp/p310-mask-clang --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p310-mask-asan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p310-mask-asan --parallel 6
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/p310-mask-asan --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p310-mask-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=thread -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p310-mask-tsan --parallel 6
setarch x86_64 -R ctest --test-dir /tmp/p310-mask-tsan -L concurrency --repeat until-fail:5 --output-on-failure
```

On the host, `docker build -t fastdb-native:local .` ran the release
suite in the Dockerfile.

## P3.11 multilevel compaction under a fixed engine memory cap (V073)

Starting at `281405e`, `merge_larger_than_memory` wrote 140 distinct
binary keys with 2 KiB values and one overlapping marker key. It flushed
ten runs to L0 and promoted them through L1 into a growing L2 run. An
additional PUT and DELETE built an overlapping L1 run. The fixture checked
the installed catalog, then paused the final L1/L2 merge on an output write
while a separate thread read, overwrote a key, and deleted another.

The selected inputs totalled 300,067 bytes and the final L2 file contained
295,577 bytes. Against a 262,144-byte engine memory limit, the GCC Debug
fixture measured 81,963 charged bytes before merge, 253,327 during merge,
and 253,497 with both foreground mutations staged in the active tree. These
are samples of the engine's reserved and allocated bytes. Process RSS and
release-scale measurement are tracked separately in P8.3. The pre-merge
snapshot retained the old update and deleted value. Live reads reflected
the foreground changes. Reopen validated all keys and the final revision.
`fastdb_public_crash` reran the existing process-kill publication fixtures.

```sh
CC=gcc cmake -S /repo -B /tmp/p311-focus -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p311-focus --parallel 6
ctest --test-dir /tmp/p311-focus -R '^fastdb_read_view$' --timeout 30 -V
CC=gcc cmake -S /repo -B /tmp/p311-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p311-gcc --parallel 6
ctest --test-dir /tmp/p311-gcc -R '^fastdb_read_view$' -V
ctest --test-dir /tmp/p311-gcc -R '^fastdb_compaction$' --output-on-failure
ctest --test-dir /tmp/p311-gcc -R '^fastdb_public_crash$' --output-on-failure
ctest --test-dir /tmp/p311-gcc --output-on-failure
CC=clang cmake -S /repo -B /tmp/p311-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/p311-clang --parallel 6
ctest --test-dir /tmp/p311-clang --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p311-asan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p311-asan --parallel 6
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/p311-asan --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p311-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=thread -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p311-tsan --parallel 6
setarch x86_64 -R ctest --test-dir /tmp/p311-tsan -L concurrency --repeat until-fail:5 --output-on-failure
```

The focused cases passed 1/1 each. The full profiles and the host's
`docker build -t fastdb-native:local .` passed 26/26 each. The TSan profile
ran four concurrency targets five times each with the V062 workaround.

## P4.1 optional LSM memtable arena (V074)

Starting at `6d16f88`, the test-first GCC Debug build failed on the
missing `fastdb_options_set_lsm_arena` declaration. The first implementation
passed focused LSM, allocation-fault, internal tree, and snapshot tests.
The first full profiles found that enlarging the common database handle
made an existing 230-byte value fail admission under a 512-byte memory
limit. The corrected design restored the original fixed handle charge and
moved persistent allocation policy and key-byte accounting into disk state.
The 512-byte fixture then passed with its original assertions.

The final LSM cases verify full chunk charges, reuse after overwrites,
charged-memory admission, retained snapshot reads after arena release and
reopen with the arena disabled. Injected allocation failures leave both a
new chunk and a reused chunk's cursor unpublished. The retry fits in the
same chunk and persists the next revision. The worker cases charge active
and frozen chunks together, validate a snapshot through an ENOSPC flush
retry, and recover both trees' WAL records after a SIGKILL during output.
The fixed-cap compaction fixture also passed with 300,067 input bytes and
253,521 charged bytes observed after foreground writes under its
262,144-byte cap.

```sh
CC=gcc cmake -S /repo -B /tmp/p41-red -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p41-red --target fastdb_lsm fastdb_alloc_failure --parallel 6
CC=gcc cmake -S /repo -B /tmp/p41-compat -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p41-compat --parallel 6
ctest --test-dir /tmp/p41-compat -R '^fastdb_memory$' --output-on-failure
ctest --test-dir /tmp/p41-compat -R '^fastdb_lsm$' --output-on-failure
ctest --test-dir /tmp/p41-compat -R '^fastdb_alloc_failure$' --output-on-failure
ctest --test-dir /tmp/p41-compat -R '^fastdb_background_flush$' --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p41-final-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p41-final-gcc --parallel 6
ctest --test-dir /tmp/p41-final-gcc -R '^fastdb_read_view$' -V
ctest --test-dir /tmp/p41-final-gcc --output-on-failure
CC=clang cmake -S /repo -B /tmp/p41-final-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/p41-final-clang --parallel 6
ctest --test-dir /tmp/p41-final-clang --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p41-final-asan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p41-final-asan --parallel 6
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/p41-final-asan --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p41-final-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=thread -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p41-final-tsan --parallel 6
setarch x86_64 -R ctest --test-dir /tmp/p41-final-tsan -L concurrency --repeat until-fail:5 --output-on-failure
```

The corrected focused cases passed 1/1 each. GCC Debug, Clang Release,
ASan/UBSan with leak detection, and `docker build -t fastdb-native:local .`
passed 26/26 each. The V062 TSan workaround ran four concurrency targets
five times each. Arena throughput and release-scale memory remain for
benchmark runs.

## P4.2 bounded LSM block cache (V075)

At `85b18ce`, GCC Debug passed all 26 baseline tests. Adding the public
read-view tests before implementation caused the expected build failure
at `fastdb_options_set_lsm_block_cache` and `fastdb_block_cache_stats`.

After implementation, a filesystem probe counted one data-block read on
a cold GET. A warm GET and a prefix scan completed with data-block reads
disabled. The first failed cold read propagated `FASTDB_IO_ERROR` and
left the cache empty. The SSTable cases verify checksum and row-offset
corruption leave the cache empty, and a cache hit stays valid while a
second iterator reads a different block with a full cache. The unit
fixture verifies LRU ordering and byte accounting, repeated pins, bypass
when all entries are pinned, and allocation failure. Two readers of one
database accounted for 200 hits after its first miss. A second database
stored a different value at the matching key and file offset. The pinned
snapshot retained its historical value through compaction and cold reopen
served the latest value. The bounded compaction test observed 253,601
charged bytes with foreground mutations under its 262,144-byte cap.

```sh
CC=gcc cmake -S /repo -B /tmp/p42-base -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p42-base --parallel 6
ctest --test-dir /tmp/p42-base --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p42-red -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p42-red --target fastdb_read_view --parallel 6
CC=gcc cmake -S /repo -B /tmp/p42-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p42-gcc --parallel 6
ctest --test-dir /tmp/p42-gcc -R '^fastdb_sstable_reader$' --output-on-failure
ctest --test-dir /tmp/p42-gcc -R '^fastdb_read_view$' -V
ctest --test-dir /tmp/p42-gcc -R '^fastdb_block_cache$' --output-on-failure
ctest --test-dir /tmp/p42-gcc --output-on-failure
CC=clang cmake -S /repo -B /tmp/p42-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/p42-clang --parallel 6
ctest --test-dir /tmp/p42-clang --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p42-asan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p42-asan --parallel 6
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/p42-asan --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p42-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=thread -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p42-tsan --parallel 6
setarch x86_64 -R ctest --test-dir /tmp/p42-tsan -L concurrency --repeat until-fail:5 --output-on-failure
```

The focused tests passed 1/1 per target. All final full profiles passed
27/27, including `docker build -t fastdb-native:local .`. Four TSan
concurrency targets passed five runs each with the V062 workaround.
The read probe measures data-block operations within the fixture. Cache
throughput and latency benchmarking remain open.

## P4.3 interval WAL sync checkpoint (V076)

At `7d84d05`, the test-first interval target failed to compile because
the interval durability policy and option setter were missing. The
corrected fixture gates the worker's filesystem sync after the caller
has received its write acknowledgement. It checks the custom 20 ms
interval in LSM and the default 100 ms interval in persistent memory.
An injected worker sync failure causes `FASTDB_COMMIT_UNCERTAIN` on the
next read, write and close. Reopen recovers the written frame on the
test filesystem and confirms the rejected follow-up write is absent.

A long configured interval lets the test stop the worker before its
deadline. Close syncs the pending frame, including a case that injects
failure into the close sync. A three-write fixture forces WAL rotation
before the deadline, observes foreground sync during rotation, confirms
one final sync on close, and checks all three revisions after reopen.
These fault tests exercise observed I/O and recovery behavior on Docker's
filesystem. They do not establish a power-loss window.

```sh
CC=gcc cmake -S /repo -B /tmp/p43-interval-red -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p43-interval-red --target fastdb_interval_sync --parallel 6
CC=gcc cmake -S /repo -B /tmp/p43-interval-green -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p43-interval-green --parallel 6
ctest --test-dir /tmp/p43-interval-green -R '^fastdb_interval_sync$' --repeat until-fail:10 --output-on-failure
ctest --test-dir /tmp/p43-interval-green -R '^fastdb_wal' --output-on-failure
ctest --test-dir /tmp/p43-interval-green -R '^fastdb_lsm_fault$' --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p43-interval-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p43-interval-gcc --parallel 6
ctest --test-dir /tmp/p43-interval-gcc -R '^fastdb_interval_sync$' --repeat until-fail:10 --output-on-failure
ctest --test-dir /tmp/p43-interval-gcc --output-on-failure
CC=clang cmake -S /repo -B /tmp/p43-interval-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/p43-interval-clang --parallel 6
ctest --test-dir /tmp/p43-interval-clang --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p43-interval-asan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p43-interval-asan --parallel 6
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/p43-interval-asan --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p43-interval-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=thread -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p43-interval-tsan --parallel 6
setarch x86_64 -R ctest --test-dir /tmp/p43-interval-tsan -L concurrency --repeat until-fail:5 --output-on-failure
```

The focused interval target passed ten final executions. WAL and WAL set
passed 2/2 and LSM fault passed 1/1. Each full profile passed 28/28,
including the Release tests in `docker build -t fastdb-native:local .`.
The V062 TSan workaround passed five concurrency targets five times each.

## P4.3 bounded group commit (V077)

At `2836a4c`, all 28 GCC Debug baseline tests passed. The new fixture
first exposed a missing test include path, then failed to compile with
the group implementation absent. The initial implementation run reached
the CTest timeout because the gate intercepted startup directory sync.
Arming the gate after recovery isolated the intended WAL sync.

The final group fixture placed a PUT, dependent CAS, and dependent
atomic batch on one WAL in each persistent mode and observed one sync.
While sync was blocked, none of the callers had acknowledged, the engine
mutex still excluded live readers, and the queue rejected another writer.
Separate cases covered byte-cap admission, a no-sync failed condition,
rotation through a group, and recovery after killing the child after both
writers acknowledged. An injected sync failure protected the provisional
suffix, including a condition evaluated after its first provisional write.
The expired-key purge fixture verified that an uncertain deletion does not
increment the confirmed deletion count.

```sh
CC=gcc cmake -S /repo -B /tmp/p43-group-base -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p43-group-base --parallel 6
ctest --test-dir /tmp/p43-group-base --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p43-group-red -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p43-group-red --target fastdb_group_commit --parallel 6
CC=gcc cmake -S /repo -B /tmp/p43-group-green -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p43-group-green --parallel 6
ctest --test-dir /tmp/p43-group-green -R '^fastdb_group_commit$' --output-on-failure
ctest --test-dir /tmp/p43-group-green -R '^fastdb_interval_sync$' --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p43-group-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p43-group-gcc --parallel 6
ctest --test-dir /tmp/p43-group-gcc -R '^fastdb_group_commit$' --repeat until-fail:10 --output-on-failure
ctest --test-dir /tmp/p43-group-gcc -R '^fastdb_ttl$' --output-on-failure
ctest --test-dir /tmp/p43-group-gcc --output-on-failure
CC=clang cmake -S /repo -B /tmp/p43-group-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/p43-group-clang --parallel 6
ctest --test-dir /tmp/p43-group-clang --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p43-group-asan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p43-group-asan --parallel 6
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/p43-group-asan --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p43-group-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=thread -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p43-group-tsan --parallel 6
setarch x86_64 -R ctest --test-dir /tmp/p43-group-tsan -L concurrency --repeat until-fail:5 --output-on-failure
```

The final group target passed ten consecutive executions. TTL passed 1/1.
Each full profile passed 29/29, including `docker build -t
fastdb-native:local .`. All six TSan concurrency targets passed five
executions each with the V062 workaround. The process-kill and injected
sync cases exercise recovery on the test filesystem.

## P4.4 bounded v1 Bloom reads (V078)

The archived `3572974` GCC Debug baseline passed 29/29. With the new
reader fixture added to that archive, it returned `FASTDB_LIMIT_EXCEEDED`
on a table whose four 20-byte Bloom partitions exceeded the 20-byte
aggregate reader cap. The updated reader holds one partition per iterator
in that case. The fixture still checks seeks across block boundaries and
older revisions. It also verifies error propagation and retry on injected
filter-read failure, and rejects a partition larger than the cap.

A checked table with a damaged partition falls back to its records and
reports one corrupt partition. The concurrent case shares the reader
between two iterators and sees the same tombstone and one diagnostic.
The public LSM fixture reopened an existing table under a 20-byte Bloom
cap, checked GET and scan results and the tombstone, compacted into four
bounded v1 outputs, and reopened to verify all values and revision 5 on
the next write. New writes still use the v1 layout and combined caps.

```sh
mkdir /tmp/p44-source
git -c safe.directory=/repo -C /repo archive 3572974 | tar -x -C /tmp/p44-source
CC=gcc cmake -S /tmp/p44-source -B /tmp/p44-archive -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p44-archive --parallel 6
ctest --test-dir /tmp/p44-archive --output-on-failure
cp /repo/tests/native/sstable_reader.c /repo/tests/native/compaction.c /tmp/p44-source/tests/native/
cmake --build /tmp/p44-archive --target fastdb_sstable_reader fastdb_compaction --parallel 6
ctest --test-dir /tmp/p44-archive -R '^fastdb_sstable_reader$' --output-on-failure

CC=gcc cmake -S /repo -B /tmp/p44-final-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p44-final-gcc --parallel 6
ctest --test-dir /tmp/p44-final-gcc -R '^fastdb_sstable_reader$' --repeat until-fail:10 --output-on-failure
ctest --test-dir /tmp/p44-final-gcc -R '^fastdb_compaction$' --output-on-failure
ctest --test-dir /tmp/p44-final-gcc --output-on-failure
CC=clang cmake -S /repo -B /tmp/p44-final-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/p44-final-clang --parallel 6
ctest --test-dir /tmp/p44-final-clang --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p44-final-asan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p44-final-asan --parallel 6
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/p44-final-asan --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p44-final-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=thread -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p44-final-tsan --parallel 6
setarch x86_64 -R ctest --test-dir /tmp/p44-final-tsan -L concurrency --repeat until-fail:5 --output-on-failure
```

The final focused reader passed ten executions and the compaction target
passed 1/1. GCC Debug, Clang Release, ASan/UBSan with leak detection, and
the Release suite in `docker build -t fastdb-native:local .` each passed
29/29. Seven concurrency targets passed five TSan executions each using
the V062 Docker workaround. The first Docker launch was rejected because
automatic review had reached its usage limit. A later approved launch
restored Docker verification during the session.

## P4.4 partitioned SSTable v2 (V079)

The Docker GCC Debug baseline at `f1e3b7e` passed 29/29. The test-first
build stopped because `FDBSSTLimits` had no partitioned writer setting.
The final reader test writes repeated revisions across leaves and checks
lower-bound seeks, a second simultaneous cursor, corrupt leaf CRC, a
bad root offset with a new checksum, and Bloom fallback with one
reported corrupt partition. A separate 2,048-row table exceeds 2 MiB
with under 64 KiB of configured reader workspace. The writer test fills
its root directory, rejects another row and finishes a readable file.
The public compaction fixture publishes v1 and v2 inputs, compacts them
together, and reopens with v1 selected for new output.

```sh
cmake -S /repo -B /tmp/p44b-base -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p44b-base --parallel 6
ctest --test-dir /tmp/p44b-base --output-on-failure
cmake -S /repo -B /tmp/p44b-red -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p44b-red --target fastdb_sstable_reader fastdb_compaction --parallel 6
CC=gcc cmake -S /repo -B /tmp/p44b-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p44b-gcc --parallel 6
ctest --test-dir /tmp/p44b-gcc -R '^fastdb_sstable_reader$' --repeat until-fail:5 --output-on-failure
ctest --test-dir /tmp/p44b-gcc --output-on-failure
CC=clang cmake -S /repo -B /tmp/p44b-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/p44b-clang --parallel 6
ctest --test-dir /tmp/p44b-clang --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p44b-asan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p44b-asan --parallel 6
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/p44b-asan --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p44b-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=thread -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p44b-tsan --parallel 6
setarch x86_64 -R ctest --test-dir /tmp/p44b-tsan -L concurrency --repeat until-fail:5 --output-on-failure
```

The focused writer, reader and compaction targets passed in
`/tmp/p44b-focused`. The final reader target passed five focused runs.
All full profiles passed 29/29, including `docker build -t
fastdb-native:local .`. The V062 TSan workaround ran seven targets five
times each. A temporary Docker test image derived from
`fastdb-local-toolchain:bookworm` installed `liblz4-dev` 1.9.4-1 and
`pkg-config` for the next task.

## P3.8 completion with retained catalog and merged scans (V068)

The new snapshot scan tests were added first. Building the new
`fastdb_snapshot` target in `/tmp/p38-red` failed to link the two missing
snapshot scan entry points. The first implementation build found a duplicate
internal snapshot type. After correcting it, the GCC Debug suite in
`/tmp/p38-green` passed 24/24.

The final cases verify stable pages after an atomic update and compaction,
snapshot reads from a frozen tree after publication and retry, SSTable pin
cleanup, binary prefix/range pagination, tombstone masking, independent
page lifetime, combined memory admission, and scan I/O error propagation.
The out-of-range prefix case verifies that the unrelated table reader is
never called. The tests use the V064 Docker toolchain and a read-only source
mount.

```sh
CC=gcc cmake -S /repo -B /tmp/p38-red -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p38-red --target fastdb_snapshot fastdb_lsm --parallel
CC=gcc cmake -S /repo -B /tmp/p38-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/p38-gcc --parallel
ctest --test-dir /tmp/p38-gcc --output-on-failure
CC=clang cmake -S /repo -B /tmp/p38-clang -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/p38-clang --parallel
ctest --test-dir /tmp/p38-clang --output-on-failure
CC=gcc cmake -S /repo -B /tmp/p38-asan -DCMAKE_BUILD_TYPE=Debug \
  -DFASTDB_SANITIZER=address -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build /tmp/p38-asan --parallel
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/p38-asan --output-on-failure
```

All final full CTest profiles passed 24/24. `docker build -t
fastdb-native:local .` succeeded with 24/24 Release tests. For the V062 TSan
setup, configuring `/tmp/p38-tsan` with GCC Debug,
`-DFASTDB_SANITIZER=thread -DCMAKE_EXE_LINKER_FLAGS=-no-pie`, building in
parallel and running the command below passed 20 executions.

```sh
setarch x86_64 -R ctest --test-dir /tmp/p38-tsan -L concurrency --repeat until-fail:5 --output-on-failure
```
