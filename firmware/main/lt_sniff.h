// SPDX-License-Identifier: GPL-3.0-or-later
//
// LocalTalk bring-up: log the frames TashTalk relays. A no-op unless
// CONFIG_AIRWRITER_LT_SNIFF is set; bench builds only.
#pragma once

#include <stddef.h>

void lt_sniff_start(void);

// Counters and the latest frame lines, as JSON, for GET /api/lt.
void lt_sniff_json(char *buf, size_t cap);
