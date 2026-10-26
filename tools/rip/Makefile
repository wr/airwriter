# SPDX-License-Identifier: GPL-3.0-or-later
CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -g
COMP    := ../../firmware/components
INCS    := -I$(COMP)/ripjob/include \
           -I$(COMP)/printer_caps/include \
           -I$(COMP)/ps_page/include \
           -I$(COMP)/job_pipe/include \
           -I$(COMP)/bigmem/include \
           -I$(COMP)/laser/include \
           -I$(COMP)/urf_decode/include \
           -I$(COMP)/resample/include \
           -I$(COMP)/halftone/include \
           -I$(COMP)/imagewriter/include \
           -I$(COMP)/serial_out/include \
           -I$(COMP)/testpage/include \
           -I$(COMP)/stylewriter/include \
           -I.
# serial_out is IDF-bound except for serial_writer.c, which is pure by design
# so the deadline loop can be tested here. Name it, don't wildcard the dir.
SRCS    := $(wildcard $(COMP)/ripjob/*.c) \
           $(COMP)/printer_caps/printer_caps.c \
           $(wildcard $(COMP)/ps_page/*.c) \
           $(COMP)/job_pipe/job_pipe_rip.c \
           $(COMP)/laser/lw_query.c $(COMP)/laser/lw_status.c $(COMP)/laser/lw_caps.c \
           $(wildcard $(COMP)/urf_decode/*.c) \
           $(wildcard $(COMP)/resample/*.c) \
           $(wildcard $(COMP)/halftone/*.c) \
           $(wildcard $(COMP)/imagewriter/*.c) \
           $(wildcard $(COMP)/testpage/*.c) \
           $(COMP)/stylewriter/sw_model.c $(COMP)/stylewriter/sw_code.c $(COMP)/stylewriter/sw_page.c \
           $(COMP)/stylewriter/sw_session.c \
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
build/test_localtalk: test/test_localtalk.c $(LT)/lt_proto.c $(LT)/lt_node.c $(LT)/pap_client.c $(LT)/lt_tashtalk.c \
                      $(COMP)/serial_out/serial_writer.c | build
	$(CC) $(CFLAGS) -I$(LT)/include -I$(COMP)/serial_out/include -Itest -o $@ $^

build/test_ethertalk: test/test_ethertalk.c $(LT)/et_proto.c $(LT)/et_link.c $(LT)/et_transport.c $(LT)/lt_node.c $(LT)/pap_client.c $(LT)/lt_proto.c | build
	$(CC) $(CFLAGS) -I$(LT)/include -Itest -o $@ $^

build/test_pap_client: test/test_pap_client.c $(LT)/lt_proto.c $(LT)/lt_node.c $(LT)/pap_client.c | build
	$(CC) $(CFLAGS) -I$(LT)/include -Itest -o $@ $^

build/test_pap_relay: test/test_pap_relay.c $(LT)/lt_proto.c $(LT)/lt_node.c $(LT)/pap_client.c $(LT)/pap_relay.c | build
	$(CC) $(CFLAGS) -I$(LT)/include -Itest -o $@ $^

# W-1088: the printer list, beside the LaserWriter pieces it names records with.
build/test_printers: test/test_printers.c $(COMP)/printers/printers_core.c $(COMP)/laser/lw_caps.c \
                     $(COMP)/laser/lw_query.c $(COMP)/printer_caps/printer_caps.c $(LT)/lt_proto.c | build
	$(CC) $(CFLAGS) $(INCS) -I$(COMP)/printers/include -I$(LT)/include -Itest -o $@ $^

build/test_lw_core: test/test_lw_core.c $(COMP)/laser/lw_core.c $(COMP)/laser/lw_status.c \
                    $(LT)/lt_proto.c | build
	$(CC) $(CFLAGS) -I$(LT)/include -I$(COMP)/laser/include -Itest -o $@ $^

# The board's LaserWriter side on the host, for test_interop.py (W-1082).
build/lwhost: lwhost.c $(LT)/et_proto.c $(LT)/et_link.c $(LT)/lt_node.c $(LT)/pap_client.c \
              $(LT)/lt_proto.c $(COMP)/laser/lw_query.c | build
	$(CC) $(CFLAGS) -I$(LT)/include -I$(COMP)/laser/include -o $@ $^

CT := ../../firmware/components/cable_test
build/test_cable: test/test_cable.c $(CT)/cable_core.c $(LT)/lt_proto.c | build
	$(CC) $(CFLAGS) -I$(CT)/include -I$(LT)/include -Itest -o $@ $^

build/%: test/%.c $(SRCS) | build
	$(CC) $(CFLAGS) $(INCS) -Itest -o $@ $^ -lm

# The page stage's memory, counted through bigmem.h (BIGMEM_COUNT, R15).
build/test_sw_page_mem: CFLAGS += -DBIGMEM_COUNT
build/test_sw_page_mem: test/test_sw_page_mem.c $(SRCS) | build
	$(CC) $(CFLAGS) $(INCS) -Itest -o $@ $^ -lm

# W-1088 review: the printer list's core takes none of the board's internal
# RAM of its own (docs/traps.md): no writable statics, its buffers are the
# caller's (the board's are in PSRAM).
build/printers_core.o: $(COMP)/printers/printers_core.c | build
	$(CC) $(CFLAGS) $(INCS) -I$(COMP)/printers/include -I$(LT)/include -c -o $@ $<

# The StyleWriter's model table, run coder, page stage and session are held to the same rule
# as the printer list's core (W-1103): no writable statics; the page stage's block and the
# session's frame buffer come from the board's PSRAM allocator (bigmem.h).
SW_OBJS := build/sw_model.o build/sw_code.o build/sw_page.o build/sw_session.o
build/sw_%.o: $(COMP)/stylewriter/sw_%.c | build
	$(CC) $(CFLAGS) $(INCS) -c -o $@ $<

test: $(TESTBIN) build/printers_core.o $(SW_OBJS)
	@fail=0; for t in $(TESTBIN); do ./$$t || fail=1; done; \
	for o in printers_core $(basename $(notdir $(SW_OBJS))); do \
	  if nm build/$$o.o | grep -E ' [bBdD] ' | grep -v ' ltmp'; then \
	    echo "FAIL $$o: writable statics (above)"; fail=1; \
	  else echo "ok  $$o: no writable statics"; fi; \
	done; exit $$fail

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

# The LaserWriter's PostScript held to Ghostscript dot for dot (W-1082). Needs
# Ghostscript (brew install ghostscript), a host test tool only.
.PHONY: pscheck
pscheck: build/rip build/test_lw
	@python3 test/pscheck.py

# The StyleWriter's page stage held to swfake.py's reading of its stream and to a
# second coder, lpstyl's (W-1103). Needs python3.14 with numpy (tools/stylewriter).
LPSTYL := test/lpstyl_encode.c test/lpstyl_stream.c

build/lpstyl_stream: $(LPSTYL) $(COMP)/stylewriter/sw_model.c $(COMP)/halftone/apple_tables.c | build
	$(CC) $(CFLAGS) $(INCS) -Itest -o $@ $^

.PHONY: swcheck
swcheck: build/rip build/lpstyl_stream
	@python3.14 test/swcheck.py

# The PAP client, built for the host, printing to etpeer.py's fake
# LaserWriter over a pipe (W-1082). Ghostscript renders what it gets when
# installed; the test does not need it.
.PHONY: interop
interop: build/lwhost build/rip
	@python3 ../ethertalk/test_interop.py

# The firmware's StyleWriter session, page stage and rip on the host (W-1103): swhost prints a
# page to a serial device, which is swfake.py's pty here and a real printer's adapter for an
# owner (swprint, tools/stylewriter/README.md). Reads PNG pages through zlib.
build/swhost: swhost.c png_read.c $(SRCS) | build
	$(CC) $(CFLAGS) $(INCS) -o $@ $^ -lm -lz

# The owner's name for it (tools/stylewriter/README.md); the program names itself by it.
build/swprint: build/swhost
	ln -sf swhost $@

.PHONY: swinterop
swinterop: build/swhost build/rip
	@python3.14 ../stylewriter/test_swinterop.py
