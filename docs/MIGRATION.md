# Legacy WAL export

The native persistent engine refuses to initialize an unrecognized nonempty
directory. It preserves legacy filenames and their contents. Open a
separate directory for new storage.

`fastdb-legacy-export` converts a supported offline log into a hex-encoded
CLI command file. Use a quiesced directory or an offline copy. The legacy
writer has no compatible lock. The exporter reads source files with
read-only descriptors and checks that the selected file and layout stayed
stable while it read them.

Supported input has exactly one top-level `wal_<digits>.log`, no
`tables` entry, and no recognized native or loose legacy table files. Each
line is a complete LF-terminated `SET key value` record with a nonempty
single-token key and nonempty value. Values preserve spaces. Embedded NUL,
CR, invalid commands, and incomplete final lines fail the export. The
maximum input line is 32,760 bytes. Interpret these records as a canonical
log from the legacy text interface.

```sh
build/fastdb-legacy-export ./offline-legacy ./recovered.commands
mkdir ./new-fastdb
build/fastdb-cli --memory-persistent ./new-fastdb < ./recovered.commands
```

The output must be a new filename in a directory distinct from the source.
The exporter publishes a complete command file exclusively. Apply it only
to an intentionally chosen new database. CLI import executes independent
PUTs in source order. Verify the exit status and representative keys after
reopen. If import stops partway, keep the export and use another fresh
database for the retry.

The exporter covers the records it can read from the retained WAL. Old
acknowledged writes may be absent because the prototype could delete older
logs on a failed flush. Multiple WALs and legacy SSTables lack reliable
sequence or portable metadata. Those layouts require separate analysis and
have no automatic migration path. Preserve the originals.
