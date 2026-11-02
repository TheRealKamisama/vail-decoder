#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Cross-compiles vail_decode.exe (WAV files) into build/win/. CC must target Windows, e.g.
# CC="zig cc -target x86_64-windows-gnu". For live decoding from a sound card, see cli/.
set -e
cd "$(dirname "$0")"
CC="${CC:-zig cc -target x86_64-windows-gnu}"
CFLAGS="${CFLAGS:--O2 -std=c99 -Wall -Wextra -Wno-unused-parameter}"
mkdir -p build/win
SRC="src/fe.c src/filt.c src/sigext.c src/seg.c src/sta.c src/spd.c src/cvd.c src/chr.c src/wrd.c src/msg.c src/sig.c src/bcm.c src/api.c"
$CC $CFLAGS -D_DEFAULT_SOURCE -o build/win/vail_decode.exe tools/vail_decode.c $SRC
rm -f build/win/*.pdb
echo "built: build/win/vail_decode.exe"
