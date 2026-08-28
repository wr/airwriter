# SPDX-License-Identifier: GPL-3.0-or-later
CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -g
COMP    := ../../firmware/components
INCS    := -I$(COMP)/ripjob/include \
           -I$(COMP)/urf_decode/include \
           -I$(COMP)/resample/include \
           -I$(COMP)/halftone/include \
           -I$(COMP)/imagewriter/include \
           -I.
SRCS    := $(wildcard $(COMP)/ripjob/*.c) \
           $(wildcard $(COMP)/urf_decode/*.c) \
           $(wildcard $(COMP)/resample/*.c) \
           $(wildcard $(COMP)/halftone/*.c) \
           $(wildcard $(COMP)/imagewriter/*.c)
TESTSRC := $(wildcard test/test_*.c)
TESTBIN := $(patsubst test/%.c,build/%,$(TESTSRC))

.PHONY: all test clean
all: build/rip

build:
	@mkdir -p build

build/rip: main.c png_write.c $(SRCS) | build
	$(CC) $(CFLAGS) $(INCS) -Itest -o $@ $^

build/%: test/%.c $(SRCS) | build
	$(CC) $(CFLAGS) $(INCS) -Itest -o $@ $^

test: $(TESTBIN)
	@fail=0; for t in $(TESTBIN); do ./$$t || fail=1; done; exit $$fail

clean:
	rm -rf build

FIX  := ../../fixtures
IOS  := ios-20260804-232652

.PHONY: regress
regress: build/rip
	@python3 test/make_fixture.py /tmp/checker.urf >/dev/null
	@./build/rip --in /tmp/checker.urf --out-stream /tmp/checker.bin 2>/dev/null
	@cmp /tmp/checker.bin $(FIX)/checker.expected.bin \
	  && echo "ok  regression: synthetic stream.bin unchanged" \
	  || (echo "FAIL regression: synthetic stream.bin changed"; exit 1)
	@./build/rip --in $(FIX)/$(IOS).urf --out-stream /tmp/ios.bin 2>/dev/null
	@cmp /tmp/ios.bin $(FIX)/$(IOS).expected.bin \
	  && echo "ok  regression: iOS stream.bin unchanged" \
	  || (echo "FAIL regression: iOS stream.bin changed"; exit 1)
