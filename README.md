# FastDB

FastDB is a native C17 key-value storage engine with in-memory,
persistent-memory, and LSM modes. All three support binary keys and
values, revisions, conditional atomic batches, bounded range and prefix
scans, and immutable snapshots.
Native LSM callers can enable a background flush worker through the
[configuration API](docs/CONFIGURATION.md).

## Build and test

On Linux, install a C17 compiler (GCC or Clang), CMake 3.20 or newer, GNU Make,
and POSIX development headers.

```sh
make test
```

The build produces `build/libfastdb.a`, `build/libfastdb.so`, and
`build/fastdb-cli`. An equivalent standalone CMake build is available.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## CLI quickstart

```sh
printf 'put 6b 76616c7565\nget 6b\nput 6d -\nscan\ndelete 6b\nget 6b\n' | build/fastdb-cli
```

The CLI reads hex keys and values. Use `-` for an empty value. Commands are
limited to 65,535 bytes per line. The default database discards its records
on process exit.

```sh
mkdir lsm-data memory-data
printf 'put 6b 76616c7565\nflush\ncompact\n' | build/fastdb-cli --lsm lsm-data
printf 'get 6b\n' | build/fastdb-cli --lsm lsm-data
printf 'put 6b -\ncheckpoint\n' | build/fastdb-cli --memory-persistent memory-data
printf 'scan\n' | build/fastdb-cli --memory-persistent memory-data
```

Each persistent database holds an exclusive directory lock. Run
`build/fastdb-cli --help` for commands. See [migration](docs/MIGRATION.md)
for the offline exporter for supported legacy logs.

## Docker

```sh
docker build -t fastdb-native .
printf 'put 6b 76616c7565\nflush\ncompact\n' | docker run --rm -i -v fastdb-data:/data fastdb-native
printf 'get 6b\n' | docker run --rm -i -v fastdb-data:/data fastdb-native
```

The image runs the native suite during the build. It includes the CLI,
legacy exporter, C headers, and static and shared libraries. The default
command opens LSM storage under `/data` as UID and GID 10001. A named volume
persists data between runs. For another storage mode, override the arguments
with `--memory-persistent /data`, or use `--help` to list commands. Use a
separate volume per database. For a bind mount, give UID 10001 permission to
write its directory. Run the offline exporter with
`--entrypoint /usr/local/bin/fastdb-legacy-export` and separate source and
destination mounts. The Go server and Studio remain separate pending
milestones.

## C applications

```sh
cmake --install build --prefix "$PWD/build/install"
cmake -S examples/c -B build/example -DCMAKE_PREFIX_PATH="$PWD/build/install"
cmake --build build/example
build/example/fastdb-example-static
build/example/fastdb-example-shared
```

The installed targets are `FastDB::static` and `FastDB::shared`. See
[the native example](examples/c/hello.c),
[ownership and concurrency](docs/CONCURRENCY.md), and
[configuration limits](docs/CONFIGURATION.md).

## Project status

The [implementation ledger](PROGRESS.md) records verified milestones and
remaining work on persistent storage, the server and Studio.
[Durability](docs/DURABILITY.md) describes current storage guarantees.
`make test-baseline` uses disposable files to reproduce the
[original prototype defects](docs/INITIAL_AUDIT.md).
