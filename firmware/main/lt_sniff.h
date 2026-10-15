// SPDX-License-Identifier: GPL-3.0-or-later
//
// LocalTalk bring-up: log the frames TashTalk relays. A no-op unless
// CONFIG_AIRWRITER_LT_SNIFF is set; bench builds only.
#pragma once

void lt_sniff_start(void);
