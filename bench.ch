#!/usr/bin/env bash
set -euo pipefail
CC="gcc -O2 -pipe -Wall -pthread \
    -I/opt/homebrew/include \
    -L/opt/homebrew/lib \
    -lcurl -lcjson"

OLD="OP_Old.c"
NEW="OP.c"
BIN_OLD=old
BIN_NEW=new
DUR=30

$CC "$OLD" -o $BIN_OLD
$CC "$NEW" -o $BIN_NEW

probe () {
  local exe=$1 tag=$2
  echo "### $tag" >> bench-report.txt
  /usr/bin/time -f "wall %e s  rss %M KB" ./$exe & pid=$!
  pidstat -rud -p $pid 1 $DUR >> bench-report.txt & mon=$!
  sleep $DUR; kill -INT $pid $mon 2>/dev/null || true
  wait $pid 2>/dev/null || true
  echo >> bench-report.txt
}
echo "Benchmark @ $(date)" > bench-report.txt
probe $BIN_OLD "legacy"
probe $BIN_NEW "optimized"
echo "OK  →  bench-report.txt"
