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

# 8. a game too big for one half of the flash: the ROM in two parts, the
#    second one (the extension) for the other half. Budgets scaled down to
#    the 32 KB test ROM.
out=$($HOST "$ROM" --frames 900 --input tests/input.txt --profile "$TMP/two.pack" --budget-kb 24 --ext-kb 16 2>&1)
check "two-part pack written" "in its extension" "$out"
out=$($HOST "$TMP/two.pack" --frames 900 --input tests/input.txt --compare --exact --pool-kb 8 2>&1)
check "two-part pack + 8 KB of RAM = reference (900 frames)" "900 frames, 0 with different pixels, 0 with different memory" "$out"
# the extension depends on the ROM alone, not on what was played
$HOST "$ROM" --frames 300 --seed 5 --profile "$TMP/two-b.pack" --budget-kb 24 --ext-kb 16 >/dev/null 2>&1
main=$(od -An -tu4 -j16 -N4 "$TMP/two.pack" | tr -d ' ')
mainb=$(od -An -tu4 -j16 -N4 "$TMP/two-b.pack" | tr -d ' ')
tail -c +$((main+1)) "$TMP/two.pack" > "$TMP/ext-a.bin"
tail -c +$((mainb+1)) "$TMP/two-b.pack" > "$TMP/ext-b.bin"
if [ -s "$TMP/ext-a.bin" ] && cmp -s "$TMP/ext-a.bin" "$TMP/ext-b.bin"; then out="same"; else out="different"; fi
check "same extension whatever was played" "same" "$out"
# without its second part, or with a damaged one, the pack is refused
head -c "$main" "$TMP/two.pack" > "$TMP/main-only.pack"
out=$($HOST "$TMP/main-only.pack" --frames 10 2>&1)
check "two-part pack without its extension refused" "extension) is missing or damaged" "$out"
{ head -c $((main+200)) "$TMP/two.pack"                                              # one byte of the extension
  tail -c +$((main+201)) "$TMP/two.pack" | head -c 1 | tr '\000-\377' '\001-\377\000' # changed (whatever it was)
  tail -c +$((main+202)) "$TMP/two.pack"; } > "$TMP/bad-ext.pack"
out=$($HOST "$TMP/bad-ext.pack" --frames 10 2>&1)
check "two-part pack with a damaged extension refused" "extension) is missing or damaged" "$out"

if [ "$fails" -eq 0 ]; then echo "all tests passed"; else echo "$fails test(s) failed"; exit 1; fi
