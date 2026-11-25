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
