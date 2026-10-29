#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# A/B a tuning variant: tools/ab.sh NAME "-DFLAG=0 ..." [eval.py args]
# Builds build/v_NAME with the extra flags, runs tools/eval.py on it and prints the overall line;
# the full table is left in build/v_NAME/eval.txt. CC as for build.sh.
cd "$(dirname "$0")/.." || exit 1
name=$1; flags=$2; shift 2
OUT=build/v_$name CFLAGS="-O2 -std=c99 -D_DEFAULT_SOURCE $flags" ./build.sh >/dev/null 2>&1 || { echo "build failed"; exit 1; }
VAIL_BUILD=build/v_$name python3 tools/eval.py "$@" > build/v_$name/eval.txt
printf "%-14s %s\n" "$name" "$(tail -1 build/v_$name/eval.txt)"
