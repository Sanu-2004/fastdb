# Simple C Key-Value Store

This project is a small key-value database written in C. It stores data in memory using a red-black tree and persists writes to a write-ahead log (WAL) so the latest state can be restored on startup.

## Features

- In-memory key-value storage
- Red-black tree insertion and lookup
- Interactive REPL interface
- Write-ahead logging to `wal_<timestamp>.log`
- Automatic restore from the most recent WAL file in the current directory

## Project Layout

```text
.
├── Makefile
├── include/
│   ├── db.h
│   ├── memTree.h
│   └── repl.h
├── src/
│   ├── db.c
│   ├── main.c
│   ├── memTree.c
│   └── repl.c
└── build/
```

## Build

Requirements:

- GCC
- Make

Build the application:

```sh
make
```

The executable is generated at `build/app`.

## Run

Run the program with:

```sh
make run
```

Or run the binary directly:

```sh
./build/app
```

## REPL Commands

The application starts an interactive REPL that accepts the following commands:

```text
SET key value
GET key
PRINT
```

Examples:

```text
SET name sanu
GET name
PRINT
```

Behavior:

- `SET key value` inserts a new key or updates an existing key
- `GET key` prints the stored value, or `NULL` if the key does not exist
- `PRINT` prints the tree contents in sorted key order

## Persistence

Each `SET` operation is appended to a WAL file.

- If no WAL file exists, the program creates one named like `wal_1710000000.log`
- On startup, the program looks for the most recent `wal_*.log` file in the current directory
- If a WAL file is found, the database restores its state by replaying logged `SET` operations

This means data persists across restarts as long as the WAL file remains available.

## Clean

Remove build artifacts with:

```sh
make clean
```

Note: `make clean` removes the `build/` directory, but does not remove generated WAL log files.

## Notes

- Keys and values are space-delimited in the current REPL parser
- WAL files are created in the directory where the program is executed
- The current implementation focuses on insertion, lookup, and recovery from logged writes