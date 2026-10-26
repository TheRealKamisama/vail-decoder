#!/bin/sh
# Builds the vail core library and tools into build/.
# CC defaults to cc; on hosts without one, a Zig toolchain works: CC="zig cc" ./build.sh
set -e
cd "$(dirname "$0")"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--O2 -g -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -Wno-unused-parameter}"
OUT="${OUT:-build}"
mkdir -p "$OUT"
SRC="src/fe.c src/filt.c src/sigext.c src/seg.c src/sta.c src/spd.c src/cvd.c src/chr.c src/wrd.c src/msg.c src/sig.c src/bcm.c src/api.c"
$CC $CFLAGS -o "$OUT/vail_decode" tools/vail_decode.c $SRC -lm
$CC $CFLAGS -o "$OUT/cwgen" tools/cwgen.c -lm
$CC $CFLAGS -Isrc -o "$OUT/test_conformance" tests/test_conformance.c $SRC -lm
echo "built: $OUT/vail_decode $OUT/cwgen $OUT/test_conformance"
