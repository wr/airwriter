# SPDX-License-Identifier: GPL-3.0-or-later
# GIFs of every panel light effect, drawn from the firmware's own renderer:
#   make -C tools/lights OUT=<wiki checkout>/images/lights
# Needs Python 3 with numpy and Pillow; gifsicle shrinks the files if it's on PATH.
CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2
COMP    := ../../firmware/components
OUT     ?= build/gifs
WIDTH   ?= 240

.PHONY: gifs clean
gifs: build/frames.txt
	python3 render.py $< ../../hardware/panel/panel_layout.json $(OUT) --width $(WIDTH)

build:
	@mkdir -p build

build/scenes: scenes.c $(COMP)/led_grid/led_grid_core.c $(COMP)/led_grid/led_map.c | build
	$(CC) $(CFLAGS) -I$(COMP)/led_grid/include -I$(COMP)/printer_state/include -o $@ $^ -lm

build/frames.txt: build/scenes
	build/scenes > $@

clean:
	rm -rf build
