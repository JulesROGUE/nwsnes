#!/bin/sh
# Compares nwsnes with a reference emulator binary, frame by frame.
# usage: crosscheck.sh <reference-binary> <rom dir> [frames] [seed] [extra nwsnes-host options]
# The reference binary must print one "f<frame> <hash>" line per frame
# (see docs/TESTING.md for the 40-line LakeSnes driver used during development).
REF=$1; DIR=$2; FRAMES=${3:-300}; SEED=${4:-7}; shift 4 2>/dev/null
HOST=$(dirname "$0")/../build/nwsnes-host
same=0; total=0
for f in $(find "$DIR" -name "*.sfc" | sort); do
  "$REF" "$f" "$FRAMES" "$SEED" x 1 2>/dev/null | grep "^f" > /tmp/crosscheck_a.txt
  "$HOST" "$f" --frames "$FRAMES" --seed "$SEED" --hash-each "$@" 2>/dev/null | grep "^f" > /tmp/crosscheck_b.txt
  total=$((total+1))
  d=$(diff /tmp/crosscheck_a.txt /tmp/crosscheck_b.txt | grep -c '^<')
  if [ "$d" = "0" ]; then same=$((same+1)); else echo "DIFF ${f#$DIR/}: $d of $FRAMES frames differ"; fi
done
echo "identical on all $FRAMES frames: $same / $total"
