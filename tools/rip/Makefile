# SPDX-License-Identifier: GPL-3.0-or-later
CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -g
COMP    := ../../firmware/components
INCS    := -I$(COMP)/urf_decode/include \
           -I$(COMP)/resample/include \
           -I$(COMP)/halftone/include \
           -I$(COMP)/imagewriter/include \
           -I.
SRCS    := $(wildcard $(COMP)/urf_decode/*.c) \
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
