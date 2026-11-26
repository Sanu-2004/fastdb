# ADR 0006 — Bounded pinned SSTable reads

Status: Accepted

An LSM point read first checks both memory trees while holding the engine
mutex. A match is copied immediately. A miss can capture the installed
catalog entries that could contain the key and the read revision. The
engine releases the mutex during disk lookup and publishes the result from
this captured revision. Batch conditions still hold the mutex for their
entire read and commit.

The database admits one unlocked read at a time. Admission charges an
extra table reader workspace and the copied catalog bounds to available
engine memory. Other reads use the existing locked workspace. A captured
view owns its metadata and protects file names, including names the reader
has yet to open.

Durable publication may advance once while a reader holds the pin. The
installation defers all orphan reclamation, and a later publication waits
or returns `FASTDB_BUSY` until the pin releases. This bounds the deferred
file set and keeps reclamation away from outputs under construction.
Reclamation runs on the next maintenance operation, close, or reopen. An
I/O failure during that cleanup is reported on the maintenance path. The
point reader releases its pin without doing file cleanup.
