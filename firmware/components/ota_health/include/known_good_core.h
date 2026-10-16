// SPDX-License-Identifier: GPL-3.0-or-later
// The known-good slot (W-1059), pure C. The flash holds three equal app slots
// and a record, written twice, naming the one that last worked: kept by the
// trial gate and through a real print. The power-up reset boots it, updates
// never write it, and nothing is ever copied between slots.
//
// No ESP-IDF here: the app (ota_health.c) and the bootloader hook
// (bootloader_components/airwriter_boot) both build this file.
#pragma once
#include <stddef.h>
#include <stdint.h>

// The `known_good` partition: two 4 KB sectors, each starting with one copy.
// A copy is written by erasing its sector, so the other is never touched.
#define KG_PART_LABEL   "known_good"
#define KG_SECTOR       0x1000u
#define KG_MAGIC        0x474b5741u     // "AWKG"

typedef struct {
    uint32_t magic;
    uint32_t seq;           // the higher of two valid copies is current
    uint32_t slot;          // 0 is ota_0
    uint32_t crc;           // kg_crc32 of the three words above
} kg_rec_t;

// CRC-32 (IEEE 802.3, as zlib's crc32).
uint32_t kg_crc32(const void *p, size_t n);

void kg_make(kg_rec_t *r, uint32_t seq, uint32_t slot);
// Magic and CRC match, and seq is not an erased word.
int  kg_valid(const kg_rec_t *r);
// The current copy, 0 or 1, or -1 when neither is valid (a blank record).
int  kg_current(const kg_rec_t two[2]);
// The known-good slot. A blank record, or one naming a slot this table does
// not have, means ota_0: where a USB flash puts the app.
int  kg_slot(const kg_rec_t two[2], int nslots);
// The write that names `slot`: fills `out` and returns the copy (0 or 1) it
// replaces, always the one that is not current, so a write cut short leaves
// the current copy in charge.
int  kg_next(const kg_rec_t two[2], int slot, kg_rec_t *out);

// Where an update goes: the first slot after the running one, counting
// around, that is neither the running slot nor the known-good one. -1 if
// there is none (fewer than three slots).
int  kg_update_target(int running, int good, int nslots);

// Whether the running image takes over as known-good: it is kept (not on
// trial), it has finished a real print, and the record names another slot.
int  kg_should_promote(int kept, int printed, int running, int good);

// The order the power-up reset tries slots in: the known-good one, then the
// one otadata boots now (-1 for none), then the rest from ota_0 up. Fills
// `order` with nslots entries and returns nslots.
int  kg_reset_order(int good, int booting, int nslots, int order[]);

// The otadata sequence number that makes the bootloader boot `slot`: the
// smallest past `active_seq` with (seq - 1) % nslots == slot (ESP-IDF's own
// mapping). `active_seq` is 0 when otadata has no valid entry.
uint32_t kg_ota_seq(uint32_t active_seq, int slot, int nslots);
