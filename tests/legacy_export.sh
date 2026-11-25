#!/bin/sh
set -eu
work=$(mktemp -d "${TMPDIR:-/tmp}/fastdb-legacy-export.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
exporter=$1
cli=$2
mkdir "$work/source" "$work/database"
wal="$work/source/wal_1700000000.log"
printf 'SET example first value\nSET other  leading spaces\nSET example replacement\n' > "$wal"
cp "$wal" "$work/original"
"$exporter" "$work/source" "$work/export"
cmp "$wal" "$work/original"
expected=$(printf 'put 6578616d706c65 66697273742076616c7565\nput 6f74686572 206c656164696e6720737061636573\nput 6578616d706c65 7265706c6163656d656e74')
test "$(cat "$work/export")" = "$expected"
"$cli" --memory-persistent "$work/database" < "$work/export" > "$work/import-log"
test "$(printf 'get 6578616d706c65\n' | "$cli" --memory-persistent "$work/database")" = \
    'VALUE 3 7265706c6163656d656e74'

if "$exporter" "$work/source" "$work/export"; then exit 1; fi
test "$(cat "$work/export")" = "$expected"
if "$exporter" "$work/source" "$work/source/export"; then exit 1; fi
test ! -e "$work/source/export"

cp "$wal" "$work/source/wal_1700000001.log"
if "$exporter" "$work/source" "$work/multi"; then exit 1; fi
test ! -e "$work/multi"
rm "$work/source/wal_1700000001.log"
mkdir "$work/source/tables"
if "$exporter" "$work/source" "$work/tables"; then exit 1; fi
test ! -e "$work/tables"
rmdir "$work/source/tables"

printf 'SET complete yes\nSET partial no' > "$wal"
if "$exporter" "$work/source" "$work/torn"; then exit 1; fi
test ! -e "$work/torn"
printf 'SET key value\000extra\n' > "$wal"
if "$exporter" "$work/source" "$work/nontext"; then exit 1; fi
test ! -e "$work/nontext"
