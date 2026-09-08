#!/usr/bin/env bash
set -u

SRC=/src
WORK=/work

rm -rf "$WORK"
mkdir -p "$WORK"
cp -a "$SRC"/. "$WORK"/

cd "$WORK" || exit 1
make clean >/dev/null 2>&1
./test/run.sh "$@"
rc=$?

mkdir -p "$SRC/test/out"
cp -a "$WORK"/test/out/. "$SRC/test/out/" 2>/dev/null

exit $rc
