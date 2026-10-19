# SPDX-License-Identifier: GPL-3.0-or-later
CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -g
COMP    := ../../firmware/components
INCS    := -I$(COMP)/ripjob/include \
           -I$(COMP)/urf_decode/include \
           -I$(COMP)/resample/include \
           -I$(COMP)/halftone/include \
           -I$(COMP)/imagewriter/include \
           -I$(COMP)/serial_out/include \
           -I.
# serial_out is IDF-bound except for serial_writer.c, which is pure by design
# so the deadline loop can be tested here. Name it, don't wildcard the dir.
SRCS    := $(wildcard $(COMP)/ripjob/*.c) \
           $(wildcard $(COMP)/urf_decode/*.c) \
           $(wildcard $(COMP)/resample/*.c) \
           $(wildcard $(COMP)/halftone/*.c) \
           $(wildcard $(COMP)/imagewriter/*.c) \
           $(COMP)/serial_out/serial_writer.c
TESTSRC := $(wildcard test/test_*.c)
TESTBIN := $(patsubst test/%.c,build/%,$(TESTSRC))

.PHONY: all test clean
all: build/rip

build:
	@mkdir -p build

build/rip: main.c png_write.c $(SRCS) | build
	$(CC) $(CFLAGS) $(INCS) -Itest -o $@ $^ -lm

build/test_printer_state: test/test_printer_state.c ../../firmware/components/printer_state/printer_state_core.c | build
	$(CC) $(CFLAGS) $(INCS) -I../../firmware/components/printer_state/include -Itest -o $@ $^

build/test_led_grid: test/test_led_grid.c \
                      ../../firmware/components/led_grid/led_grid_core.c \
                      ../../firmware/components/led_grid/led_map.c \
                      ../../firmware/components/printer_state/printer_state_core.c | build
	$(CC) $(CFLAGS) $(INCS) -I../../firmware/components/led_grid/include \
	  -I../../firmware/components/printer_state/include -Itest -o $@ $^ -lm

build/test_dip_config: test/test_dip_config.c ../../firmware/components/dip_config/dip_config_core.c | build
	$(CC) $(CFLAGS) $(INCS) -I../../firmware/components/dip_config/include -Itest -o $@ $^

build/test_ota_health: test/test_ota_health.c ../../firmware/components/ota_health/ota_health_core.c | build
	$(CC) $(CFLAGS) -I../../firmware/components/ota_health/include -Itest -o $@ $^

build/test_known_good: test/test_known_good.c ../../firmware/components/ota_health/known_good_core.c | build
	$(CC) $(CFLAGS) -I../../firmware/components/ota_health/include -Itest -o $@ $^

build/test_fw_update: test/test_fw_update.c ../../firmware/components/fw_update/fw_update_core.c | build
	$(CC) $(CFLAGS) -I../../firmware/components/fw_update/include -Itest -o $@ $^

build/test_pic_prog:test/test_pic_prog.c test/pic_sim.c ../../firmware/components/pic_prog/pic_prog_core.c | build
	$(CC) $(CFLAGS) -I../../firmware/components/pic_prog/include -Itest -o $@ $^

LT := ../../firmware/components/localtalk
build/test_localtalk: test/test_localtalk.c $(LT)/lt_proto.c $(LT)/lt_node.c $(LT)/lt_tashtalk.c \
                      $(COMP)/serial_out/serial_writer.c | build
	$(CC) $(CFLAGS) -I$(LT)/include -I$(COMP)/serial_out/include -Itest -o $@ $^

build/test_ethertalk: test/test_ethertalk.c $(LT)/et_proto.c $(LT)/et_link.c $(LT)/lt_node.c $(LT)/lt_proto.c | build
	$(CC) $(CFLAGS) -I$(LT)/include -Itest -o $@ $^

CT := ../../firmware/components/cable_test
build/test_cable: test/test_cable.c $(CT)/cable_core.c $(LT)/lt_proto.c | build
	$(CC) $(CFLAGS) -I$(CT)/include -I$(LT)/include -Itest -o $@ $^

build/%: test/%.c $(SRCS) | build
	$(CC) $(CFLAGS) $(INCS) -Itest -o $@ $^ -lm

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
	@python3 test/make_colour_fixture.py /tmp/colour.urf >/dev/null
	@./build/rip --in /tmp/colour.urf --colour --out-stream /tmp/colour.bin 2>/dev/null
	@cmp /tmp/colour.bin $(FIX)/colour.expected.bin \
	  && echo "ok  regression: colour stream.bin unchanged" \
	  || (echo "FAIL regression: colour stream.bin changed"; exit 1)
	@./build/rip --in /tmp/colour.urf --out-stream /tmp/colour.gray.bin 2>/dev/null
	@cmp /tmp/colour.gray.bin $(FIX)/colour.gray.expected.bin \
	  && echo "ok  regression: colour-as-gray stream.bin unchanged" \
	  || (echo "FAIL regression: colour-as-gray stream.bin changed"; exit 1)
