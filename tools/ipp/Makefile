# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host tests for the IPP codec. The encoder, the request parser and the
# attribute set are plain C with no ESP-IDF dependency precisely so they can be
# tested here -- only the esp_http_server transport needs a device.
CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -g
COMP    := ../../firmware/components
INCS    := -I$(COMP)/ipp_attrs/include \
           -I$(COMP)/ipp_server/include \
           -I../rip/test
SRCS    := $(COMP)/ipp_attrs/ipp_enc.c \
           $(COMP)/ipp_attrs/ipp_attrs.c \
           $(COMP)/ipp_server/ipp_req.c
TESTSRC := $(wildcard test/test_*.c)
TESTBIN := $(patsubst test/%.c,build/%,$(TESTSRC))

.PHONY: all test clean
all: test

build:
	@mkdir -p build

build/%: test/%.c $(SRCS) | build
	$(CC) $(CFLAGS) $(INCS) -Itest -o $@ $^

test: $(TESTBIN)
	@fail=0; for t in $(TESTBIN); do ./$$t || fail=1; done; exit $$fail

clean:
	rm -rf build
