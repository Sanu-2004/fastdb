# Initial FastDB audit

Audit date 2026-11-25. The starting branch was `main` at
`ca5a528bb4fe577a789667c48ed8670962f5e82a`, tracking `origin/main`.
`AGENTS.md` and `PROGRESS.md` were already untracked. There were no modified
tracked files, checked-in tests, checked-in data, or license file at that
revision. The repository owner needs to decide licensing before redistribution.

## Environment and commands

The host was Windows x86-64, build 10.0.26300. Git 2.56.1.windows.1 was
available at `C:/Program Files/Git/cmd/git.exe`. The sandboxed WSL list
returned a logon-session error, and the sandboxed Docker client could not open
the engine pipe. Docker Desktop 4.99.0 with Docker Engine 29.9.3 was available
through approved host execution. The local build environment was Debian
12.15 x86-64 on Linux 6.18.58.1-microsoft-standard-WSL2, GCC 12.2.0, Clang
14.0.6, GNU Make 4.3, CMake 3.25.1 and Git 2.39.5.

The local verification image was built as follows. The Debian base resolved to
`sha256:e67eeead736ff7bbe258ef088bff683635c561b1fcfefad0806c1df4ab49ce0e`.

```powershell
@'
FROM debian:bookworm-slim@sha256:e67eeead736ff7bbe258ef088bff683635c561b1fcfefad0806c1df4ab49ce0e
RUN apt-get update && apt-get install -y --no-install-recommends build-essential clang cmake git ninja-build ca-certificates && rm -rf /var/lib/apt/lists/*
'@ | docker build --tag fastdb-local-toolchain:bookworm -
docker run --rm --network none --mount 'type=bind,source=D:\codex\fastdb,target=/repo,readonly' fastdb-local-toolchain:bookworm bash /repo/scripts/audit-baseline.sh
```

The runner verified the original `make` build using an archive of the original
commit. The resulting app exited successfully and printed
`Found value for 'SANU': IS AWESOME` after inserting its built-in sample data.
The fixture driver compiled with the original warnings and optimization flags.
It uses one disposable directory per probe and a fixed clock for filename tests.

## Verified baseline defects

| Probe | Observed behavior |
|---|---|
| Failed flush | A regular file at the synthetic `tables` path makes output creation fail. The engine removes the original WAL and clears the memtable. Both writes are missing after reopen. |
| Multiple WALs | Only the newest generation is replayed. A key found only in the older generation disappears. |
| Memory accounting | One 2-byte key/value entry accounts for 200 payload bytes after 100 overwrites, both live and after WAL replay. |
| GET ownership | A memtable read points into its live node. The SSTable read returns a separate allocation. |
| Merge | Two valid inputs lead to a zero-byte output at the destination level. Both inputs remain. |
| WAL file ID | Two WAL creations in the same simulated second open the same inode. |
| SSTable file ID | Two fresh processes in the same simulated second publish at the same filename. The second truncates the original. |
| Stale read | Given an old-first enumeration, which POSIX allows, a lookup returns the old value even when a newer table exists. The harness controls only enumeration order. |
| Index boundary | Exactly two records produce a sparse record offset pointing into an empty dense index. The probe checks offsets read from an ordinary writer output. |
| Delete and REPL EOF | The REPL treats DELETE as an unknown command. The value remains readable. The process reaches the one-second timeout after EOF. |

All ten probes observed the expected defects. A successful audit run records
the original behavior for comparison with replacement tests.

## Additional source findings

- The legacy loader trusts native `Footer` and `size_t` fields, allocation
  lengths and read results. A short or corrupt input may feed uninitialized
  metadata to a subsequent read. This finding was established by source
  inspection.
- Legacy WAL lines have no binary encoding, checksums, revisions or durability
  sync. The implementation checks neither append nor flush status. Directory
  cleanup matches every name beginning with `wal_`.
- The legacy source lacks a coherent locking protocol, exclusive directory
  ownership and explicit allocation/partial initialization handling.

Keep existing legacy files untouched. A future persistent engine should
identify documented legacy layout signatures before initializing new metadata.
Any verified exporter must use a separately selected destination and report
files whose contents cannot be recovered reliably. The old test binary and
the probes run only in disposable directories.
