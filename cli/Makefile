# SPDX-License-Identifier: GPL-3.0-or-later
# Builds vail-cli, one self-contained binary, with the engine in ../src compiled in. Linux, macOS and Windows
# (MSYS2/MinGW, or `zig cc`) need only a C compiler. The engine itself does not depend on anything here.
#
#   make                  build build/vail-cli
#   make test             build, then run the end-to-end tests (needs python3)
#   make install          copy the binary and man page under PREFIX (default /usr/local)
#   make CC="zig cc -target x86_64-windows-gnu" TARGET=Windows   cross-compile

ENGINE := ../src

CC      ?= cc
HOSTCC  ?= cc
CFLAGS  ?= -O2 -g
PREFIX  ?= /usr/local
TARGET  ?= $(if $(filter Windows_NT,$(OS)),Windows,$(shell uname -s))
BUILD   ?= build

# The same miniaudio options must reach every file that includes its header.
MINIAUDIO := -DMA_NO_ENGINE -DMA_NO_NODE_GRAPH -DMA_NO_RESOURCE_MANAGER -DMA_NO_GENERATION -DMA_NO_NULL
CPPFLAGS  += -D_DEFAULT_SOURCE -I$(ENGINE) -Ithird_party/miniaudio $(MINIAUDIO)

ifeq ($(TARGET),Windows)
  EXE    := .exe
  LDLIBS := -lm -static
else ifeq ($(TARGET),Darwin)
  LDLIBS := -lm -lpthread
else
  LDLIBS := -lm -lpthread -ldl
endif

BIN     := $(BUILD)/vail-cli$(EXE)
OURS    := $(wildcard src/*.c)
HEADERS := $(wildcard src/*.h) $(wildcard $(ENGINE)/*.h)

OBJS := $(OURS:src/%.c=$(BUILD)/%.o) \
        $(patsubst $(ENGINE)/%.c,$(BUILD)/engine_%.o,$(wildcard $(ENGINE)/*.c)) \
        $(BUILD)/miniaudio.o

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c99 -Wall -Wextra -c -o $@ $<

$(BUILD)/engine_%.o: $(ENGINE)/%.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c99 -c -o $@ $<

$(BUILD)/miniaudio.o: third_party/miniaudio/miniaudio.c third_party/miniaudio/miniaudio.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

# Test audio comes from the engine's own generator.
$(BUILD)/cwgen: ../tools/cwgen.c | $(BUILD)
	$(HOSTCC) -O2 -std=c99 -D_DEFAULT_SOURCE -o $@ $< -lm

test: $(BIN) $(BUILD)/cwgen
	python3 tests/e2e.py

install: $(BIN)
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/man/man1
	cp $(BIN) $(DESTDIR)$(PREFIX)/bin/
	cp vail-cli.1 $(DESTDIR)$(PREFIX)/share/man/man1/

clean:
	rm -rf $(BUILD)

.PHONY: all test install clean
