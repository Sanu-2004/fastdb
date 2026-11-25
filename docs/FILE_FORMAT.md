# Native record encoding

The internal record codec defines the following byte layout for embedding
inside a future versioned WAL frame or table block. A record alone is a
payload and carries no container magic, version or checksum.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Unsigned key length, little-endian |
| 4 | 4 | Unsigned value length, little-endian |
| 8 | 8 | Positive sequence, little-endian |
| 16 | 8 | Absolute UTC expiration in milliseconds, zero for no expiration |
| 24 | 1 | Kind, 1 for PUT and 2 for DELETE |
| 25 | 3 | Reserved, zero |
| 28 | Variable | Key bytes followed by value bytes |

The codec rejects empty keys, sequence zero, unknown kinds, nonzero reserved
fields, and tombstones with a payload or expiration. Decode also checks both
lengths against caller limits and bytes available before returning borrowed
slices. It reports the number of bytes consumed so a container can enforce
record framing. The memory API does not yet expose TTL or persist records.

## WAL segment version 1

All integers are unsigned little-endian. File and frame header checksums
cover their preceding bytes. CRC32C uses the Castagnoli polynomial.

| File offset | Size | Field |
|---:|---:|---|
| 0 | 8 | Magic bytes `FASTWAL\0` |
| 8 | 4 | Format version, 1 |
| 12 | 4 | Reserved, zero |
| 16 | 8 | Positive file ID |
| 24 | 8 | Last sequence preceding this segment |
| 32 | 4 | Reserved, zero |
| 36 | 4 | CRC32C of bytes 0 through 35 |

Each frame carries one complete batch. Records are consecutive, begin at
the frame's first sequence, and consume the full payload.

| Frame offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Magic bytes `FDB1` |
| 4 | 4 | Payload length |
| 8 | 4 | Positive operation count |
| 12 | 4 | Payload CRC32C |
| 16 | 8 | First sequence |
| 24 | 4 | Reserved, zero |
| 28 | 4 | CRC32C of bytes 0 through 27 |
| 32 | Variable | Concatenated records |

The internal reader bounds file size, frame size, operation count and record
lengths before allocation or use. Its caller supplies the expected file ID
and preceding sequence from the authoritative catalog. Revisions are
contiguous within and across segments. A segment at its byte limit must
rotate before the next frame can be appended.

## Manifest version 1

`MANIFEST` is the authoritative catalog. It is a complete snapshot updated
through a same-directory atomic replacement. Its 68-byte header is followed
by entry bytes and a 4-byte CRC32C of the complete entry region.

| Header offset | Size | Field |
|---:|---:|---|
| 0 | 8 | Magic bytes `FASTMAN\0` |
| 8 | 4 | Format version, 1 |
| 12 | 4 | Mode, 1 for persistent memory and 2 for LSM |
| 16 | 16 | Database identity |
| 32 | 8 | Positive catalog generation |
| 40 | 8 | Next reservable file ID |
| 48 | 8 | Sequence high-water recorded by this catalog version |
| 56 | 4 | Entry count |
| 60 | 4 | Entry-region byte length |
| 64 | 4 | CRC32C of bytes 0 through 63 |

Entries are sorted by strictly increasing file ID. They contain an 8-byte
ID, a 4-byte kind (1 for WAL, 2 for SSTable, 3 for checkpoint), a 4-byte
level, an 8-byte file size, two 8-byte sequence bounds and two 4-byte
key lengths. Variable minimum and maximum keys follow the fixed fields.
WAL entries have empty key bounds. Their first sequence field is the
sequence preceding the segment. A zero ending sequence marks the active
segment, whose entry size is its 40-byte header size. A sealed entry
records the exact finished size and last sequence.

A checkpoint entry records its file's minimum live revision and the
checkpoint's covered revision. The covered revision can exceed the largest
live revision if later mutations deleted keys. An empty checkpoint has
zero key lengths and minimum revision. Its covered sequence remains the
database's revision when the checkpoint was taken.

WAL filenames are `wal-` followed by the decimal file ID padded to 20
digits. An unreferenced file is never replayed. The catalog advances its
next ID in a separate durable edit before creation. A later edit can
seal the previous WAL and reference a new active WAL together.

## SSTable version 1

Tables contain a 32-byte header, contiguous data blocks, optional Bloom
partitions, a sparse index, properties and a 56-byte footer. The header
starts with `FASTSST\0`, followed by a 4-byte version, 4-byte feature mask,
an 8-byte file ID, a 4-byte reserved field and a 4-byte CRC32C of bytes 0
through 27. Feature bit 0 indicates that the table has Bloom partitions.

Each data block starts with the 4-byte magic `BLK1`, a 4-byte record-area
length, a 4-byte record count and a 4-byte CRC32C covering the rest of the
block. Encoded records follow, then an increasing array of 4-byte offsets
relative to the start of the record area. Records are sorted by unsigned
key ascending and revision descending within each key. A key's versions
can span blocks. A record above the target block size can occupy a block
alone within the configured hard limit.

The sparse index begins with a 4-byte block count. Each entry contains an
8-byte file offset, a 4-byte block length, a 4-byte key length and that
block's last key. In a filtered table, a 4-byte partition length follows
each last key. The index ends with a CRC32C of all preceding index bytes.
The last keys are nondecreasing. Seek begins with a lower bound on those
keys so a repeated key at a block boundary remains searchable.

Bloom partitions are contiguous in block order between data and index.
Each contains a 4-byte probe count, a 4-byte bit-vector length in bytes,
the bit vector and a CRC32C of the preceding partition bytes. The builder
includes every record key, including tombstones and keys with expiration
timestamps. The hash uses a 64-bit FNV-1a accumulator followed by a
fixed finalizer and odd double-hash step. Probe count and bits are read
from the file. A malformed partition is reported in corruption counters
and bypassed. Lookup continues through the index and actual data blocks.

The properties region starts with two 4-byte key lengths, followed by
8-byte record and tombstone counts, 8-byte minimum and maximum revisions,
a 4-byte codec (zero for uncompressed) and a 4-byte reserved field. The
minimum and maximum key bytes follow, then a CRC32C of the preceding
property bytes.

The footer starts with `FDBSSTF1`. It stores four 8-byte fields for index
offset and length and property offset and length. An 8-byte file ID, a
4-byte copy of the feature mask and a 4-byte CRC32C of bytes 0 through 51
complete the footer. Offsets are checked against total file size and
contiguous region boundaries before allocation.

Persistent-memory checkpoints use the SSTable container under a `chk-`
filename, followed by the 20-digit file ID. They contain one live PUT per
key. An empty checkpoint has no data blocks or Bloom partitions, a zero
index count, zero key lengths, and zero record, tombstone, and revision
properties. The manifest supplies its covered revision. LSM inputs remain
nonempty.
