#!/bin/sh
set -eu
work=$(mktemp -d "${TMPDIR:-/tmp}/fastdb-cli.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
app=$1
mkdir "$work/db"
cd "$work/db"
actual=$(printf 'put 6100 00ff\nput 61 -\nget 6100\nscan 61\ndelete 6100\nget 6100' | "$app")
expected=$(printf 'OK\nOK\nVALUE 1 00ff\n61 2 -\n6100 1 00ff\nOK\nNOT_FOUND')
test "$actual" = "$expected"
test "$(printf 'get 61\n' | "$app")" = 'NOT_FOUND'
test -z "$(ls -A)"
printf 'get\nput 0g aa\nget 61\n' > "$work/bad"
if "$app" < "$work/bad" > "$work/out" 2> "$work/err"; then
    exit 1
fi
test "$(grep -Fxc 'error: invalid argument' "$work/err")" -eq 2
test "$(cat "$work/out")" = 'NOT_FOUND'
{
    printf 'put 61 00\000ff\n'
    printf '%065537d\n' 0
    printf 'put 61 -\nget 61\n'
} > "$work/bounds"
if "$app" < "$work/bounds" > "$work/out" 2> "$work/err"; then
    exit 1
fi
test "$(grep -Fxc 'error: invalid argument' "$work/err")" -eq 2
test "$(cat "$work/out")" = "$(printf 'OK\nVALUE 1 -')"
mkdir "$work/lsm" "$work/persistent"
test "$(printf 'put 6100 00ff\nflush\n' | "$app" --lsm "$work/lsm")" = "$(printf 'OK\nOK')"
test "$(printf 'put 6100 01\nflush\ncompact\n' | "$app" --lsm "$work/lsm")" = \
    "$(printf 'OK\nOK\nOK')"
test "$(printf 'get 6100\n' | "$app" --lsm "$work/lsm")" = 'VALUE 2 01'
test "$(printf 'put 61 -\ncheckpoint\n' | "$app" --memory-persistent "$work/persistent")" = \
    "$(printf 'OK\nOK')"
test "$(printf 'scan\n' | "$app" --memory-persistent "$work/persistent")" = '61 1 -'
