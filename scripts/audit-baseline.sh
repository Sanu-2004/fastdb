#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")/.." && pwd)
revision=ca5a528bb4fe577a789667c48ed8670962f5e82a
work=$(mktemp -d "${TMPDIR:-/tmp}/fastdb-baseline.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
mkdir "$work/source" "$work/smoke"
git -c safe.directory="$root" -C "$root" archive "$revision" | tar -x -C "$work/source"
make -C "$work/source"
(
    cd "$work/smoke"
    timeout 10 "$work/source/build/app"
    test -f wal_*.log
    test -d tables/level1
)
legacy_sources=("$work/source/src/db.c" "$work/source/src/memTree.c" "$work/source/src/sstable.c")
gcc -Wall -Wextra -I"$work/source/include" -g -O0 -Wl,--wrap=time,--wrap=readdir \
    "$root/tests/legacy/probe.c" "${legacy_sources[@]}" -o "$work/probe"
for probe in failed-flush multi-wal accounting ownership merge wal-id table-id stale index; do
    mkdir "$work/$probe"
    (cd "$work/$probe" && timeout 10 "$work/probe" "$probe")
done
gcc -Wall -Wextra -I"$work/source/include" -g -O0 \
    "$root/tests/legacy/repl.c" "$work/source/src/repl.c" "${legacy_sources[@]}" \
    -o "$work/repl"
mkdir "$work/delete"
(
    cd "$work/delete"
    set +e
    printf 'SET still here\nDELETE still\nGET still\n\n' |
        timeout 1 stdbuf -oL "$work/repl" > result
    status=$?
    set -e
    test "$status" -eq 124
    grep -Fx 'Help:' result >/dev/null
    grep -Fx 'here' result >/dev/null
    printf '%s\n' 'delete: unrecognized; stored value remains; REPL spins on EOF'
)
