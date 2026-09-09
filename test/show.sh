#!/usr/bin/env bash
for f in "$@"; do
  echo "=== $f ==="
  grep -aE '^  (ok|FAIL)|^  [0-9]+ cpus|^  (equal|work|priority|fork|counter|produced|threads)|: OK|: FAIL' "test/out/$f.log" | tr -d '\r'
done
