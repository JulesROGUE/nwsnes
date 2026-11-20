#!/bin/sh
# nwsnes self-test. Needs build/nwsnes-host (make host) and the test ROM.
# Everything here runs on the PC; nothing can check the calculator itself.
set -u
HOST=${HOST:-build/nwsnes-host}
ROM=${ROM:-tests/rom/nwsnes-test.sfc}
TMP=${TMPDIR:-/tmp}/nwsnes-test.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT
fails=0
check() { # check <description> <expected text> <actual output>
  if echo "$3" | grep -q -- "$2"; then echo "ok    $1"; else echo "FAIL  $1"; echo "$3" | sed 's/^/      /'; fails=$((fails+1)); fi
}

# 1. same timing on both sides: the fast line renderer draws exactly what the
#    reference renderer draws, and the game's memory is identical
out=$($HOST "$ROM" --frames 900 --input tests/input.txt --compare --exact 2>&1)
check "fast renderer = reference renderer (900 frames)" "900 frames, 0 with different pixels, 0 with different memory" "$out"

# 2. frames are the ones recorded when the test was written (catches any drift)
out=$($HOST "$ROM" --frames 900 --input tests/input.txt --hash 2>&1)
check "known frame hashes" "$(cat tests/expected-hash.txt)" "$out"

# 3. the speed-ups do not change a single pixel of this ROM
out=$($HOST "$ROM" --frames 900 --input tests/input.txt --compare 2>&1)
check "speed-ups change no pixel" "900 frames, 0 with different pixels" "$out"

# 4. profile, build a pack, then play it with 4 KB of RAM: same pictures, same memory
out=$($HOST "$ROM" --frames 900 --input tests/input.txt --profile "$TMP/test.pack" 2>&1)
check "profiling writes a pack" "dictionary" "$out"
out=$($HOST "$TMP/test.pack" --frames 900 --input tests/input.txt --compare --exact --pool-kb 4 2>&1)
check "pack + 4 KB of RAM = reference (900 frames)" "900 frames, 0 with different pixels, 0 with different memory" "$out"

# 5. a dictionary trained on one session helps another one
out=$($HOST "$TMP/test.pack" --frames 900 --seed 3 --compare --exact --pool-kb 4 2>&1)
check "same pack, other inputs, 4 KB of RAM" "900 frames, 0 with different pixels, 0 with different memory" "$out"

# 6. without the dictionary, 4 KB is not enough: the saving is real
out=$($HOST "$ROM" --frames 900 --input tests/input.txt --pool-kb 4 2>&1)
check "no dictionary + 4 KB of RAM runs out (expected)" "out of RAM" "$out"

# 7. a truncated or corrupt pack is refused instead of crashing
head -c 1000 "$TMP/test.pack" > "$TMP/bad.pack"
out=$($HOST "$TMP/bad.pack" --frames 10 2>&1)
check "truncated pack refused" "no SNES header found" "$out"

if [ "$fails" -eq 0 ]; then echo "all tests passed"; else echo "$fails test(s) failed"; exit 1; fi
