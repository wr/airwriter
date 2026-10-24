// SPDX-License-Identifier: GPL-3.0-or-later
// W-1094: what holds internal RAM, the part Wi-Fi, lwIP and task stacks need.
//
// A 20 ms sampler keeps the lowest free internal RAM it has seen and, at each
// new low, a snapshot of the internal heap: used and free blocks by size, each
// known task's stack (size, where, high-water mark), the biggest used blocks no
// task's stack accounts for, and how many allocations have failed. /api/mem
// shows that snapshot beside one taken at the request. Marks (a relay connect,
// a job) log free RAM and the low since the mark before, so a console log says
// what each burst cost.
//
// Diagnostics only: its task, its snapshots and its JSON live in PSRAM, so it
// costs internal RAM a few hundred bytes (the failure ring, which the
// allocator's IRAM path writes). Nothing here changes how memory is used.
#pragma once
#include <stddef.h>

// Starts the sampler and the failed-allocation hook. Once, early in app_main.
void memdiag_start(void);

// Logs free internal RAM, its largest block, the low since the last mark (and
// which mark that was), and the lowest since boot; then starts a new window.
// Any task; cheap.
void memdiag_mark(const char *why);

// The JSON for /api/mem: {"now":{...},"low":{...},"failed":{...},"window":{...}}.
// Takes the "now" snapshot itself. Returns the length written.
size_t memdiag_json(char *buf, size_t cap);

// Forgets the low snapshot, so the next burst (a job, say) takes its own.
void memdiag_reset_low(void);
