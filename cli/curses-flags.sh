#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Finds ncurses for the Makefile: prints its compile flags, a |, and its link flags. Prints nothing if there is none that
# this compiler can link, and vail-cli then builds without the full-screen view.
#
#   CC=cc EXTRA_LIBS="-static" sh curses-flags.sh
: "${CC:=cc}"
tmp=$(mktemp -d 2>/dev/null) || exit 0
trap 'rm -rf "$tmp"' EXIT
printf '#include <curses.h>\nint main(void) { return initscr() == 0; }\n' > "$tmp/probe.c"

works() {   # compile flags, link flags
    # shellcheck disable=SC2086
    $CC $1 "$tmp/probe.c" -o "$tmp/probe" $2 $EXTRA_LIBS > /dev/null 2>&1
}

if command -v pkg-config > /dev/null 2>&1; then
    for name in ncursesw ncurses; do
        if pkg-config --exists "$name" 2> /dev/null; then
            cflags=$(pkg-config --cflags "$name")
            libs=$(pkg-config --libs "$name")
            if works "$cflags" "$libs"; then
                echo "$cflags|$libs"
                exit 0
            fi
        fi
    done
fi
for libs in -lncursesw -lncurses -lcurses "-lncursesw -ltinfo" "-lncurses -ltinfo"; do
    if works "" "$libs"; then
        echo "|$libs"
        exit 0
    fi
done
# MSYS2 keeps the header one directory down.
if works "-I/mingw64/include/ncursesw -DNCURSES_STATIC" "-lncursesw"; then
    echo "-I/mingw64/include/ncursesw -DNCURSES_STATIC|-lncursesw"
fi
