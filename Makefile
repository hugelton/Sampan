# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
#
# Host builds: tests and PC-side tools. Firmware builds live under firmware/.

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -Werror -Icore/include
SAN     ?= -fsanitize=address,undefined -fno-omit-frame-pointer
BUILD   := build/host

CORE_SRC := core/src/proto.c core/src/adpcm.c core/src/ring.c core/src/tx.c \
            core/src/button.c core/src/midi.c core/src/scope.c core/src/palette.c \
            core/src/dsp.c core/src/sample.c

.PHONY: all test tools e2e clean

all: test tools

$(BUILD):
	mkdir -p $@

$(BUILD)/test_core: tests/test_core.c $(CORE_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -D_DEFAULT_SOURCE -o $@ $^ -lm

$(BUILD)/test_midi: tests/test_midi.c $(CORE_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ $^ -lm

$(BUILD)/test_scope: tests/test_scope.c $(CORE_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -D_DEFAULT_SOURCE -o $@ $^ -lm

$(BUILD)/test_dsp: tests/test_dsp.c $(CORE_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -D_DEFAULT_SOURCE -o $@ $^ -lm

$(BUILD)/test_sample: tests/test_sample.c $(CORE_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -D_DEFAULT_SOURCE -o $@ $^ -lm

$(BUILD)/sampan-scope: host/sampan-scope.c host/serial.c host/png.c $(CORE_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -o $@ $^ -lm

$(BUILD)/sampan-recv: host/sampan-recv.c host/serial.c $(CORE_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -o $@ $^ -lm

$(BUILD)/sampan-send: host/sampan-send.c host/serial.c $(CORE_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -o $@ $^ -lm

tools: $(BUILD)/sampan-recv $(BUILD)/sampan-send $(BUILD)/sampan-scope

test: $(BUILD)/test_core $(BUILD)/test_midi $(BUILD)/test_scope $(BUILD)/test_dsp $(BUILD)/test_sample e2e
	$(BUILD)/test_core
	$(BUILD)/test_midi
	$(BUILD)/test_scope
	$(BUILD)/test_dsp
	$(BUILD)/test_sample

# Sender -> pipe -> receiver, both codecs.
e2e: tools
	sh tests/e2e.sh $(BUILD)

clean:
	rm -rf build
