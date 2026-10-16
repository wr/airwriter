// SPDX-License-Identifier: GPL-3.0-or-later
// The known-good slot (W-1059), pure C. The flash holds three equal app slots
// and a record, written twice, naming the image that last worked: kept by the
// trial gate and through a real print. The power-up reset boots it, updates
// never write its slot, and nothing is ever copied between slots.
//
// The record names the image as well as the slot (the first bytes of its
// app_elf_sha256), so a slot written some other way (older firmware, a USB
// flash of the app alone) is no longer vouched for.
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
#define KG_SHA_LEN      16

typedef struct {
    uint32_t magic;
    uint32_t seq;           // the higher of two valid copies is current
    uint32_t slot;          // 0 is ota_0
    uint8_t  sha[KG_SHA_LEN];   // the image's app_elf_sha256, first bytes
    uint32_t crc;           // kg_crc32 of everything above
} kg_rec_t;

// CRC-32 (IEEE 802.3, as zlib's crc32).
uint32_t kg_crc32(const void *p, size_t n);

void kg_make(kg_rec_t *r, uint32_t seq, uint32_t slot, const uint8_t sha[KG_SHA_LEN]);
// Magic and CRC match, and seq is not an erased word.
int  kg_valid(const kg_rec_t *r);
// The current copy, 0 or 1, or -1 when neither is valid (a blank record).
int  kg_current(const kg_rec_t two[2]);
// The slot the record names. A blank record, or one naming a slot this table
// does not have, means ota_0: where a USB flash puts the app.
int  kg_slot(const kg_rec_t two[2], int nslots);
// 1 if the record names this image in this slot. A blank record names none.
int  kg_names(const kg_rec_t two[2], int nslots, int slot, const uint8_t sha[KG_SHA_LEN]);
// 1 if the power-up reset may trust `slot` as known-good: the record names
// this image there, or the record is blank and the slot is ota_0 (what a USB
// flash wrote). `sha` is NULL when the slot holds no readable image.
int  kg_vouches(const kg_rec_t two[2], int nslots, int slot, const uint8_t *sha);
// The write that names `slot` and its image: fills `out` and returns the copy
// (0 or 1) it replaces, always the one that is not current, so a write cut
// short leaves the current copy in charge.
int  kg_next(const kg_rec_t two[2], int slot, const uint8_t sha[KG_SHA_LEN], kg_rec_t *out);

// Where an update goes: the first slot after the running one, counting
// around, that is neither the running slot nor `avoid` (the slot the record
// names). -1 if there is none (fewer than three slots).
int  kg_update_target(int running, int avoid, int nslots);

// Whether the running image takes over the record: it is kept (not on trial
// or failed), it has finished a real print, and the record does not already
// name it.
int  kg_should_promote(int kept, int printed, int running, int named);

// otadata as the bootloader hook reads it: one entry per sector. `ok` is a
// CRC that matches and a seq that is not erased. States are IDF's
// esp_ota_img_states_t.
typedef struct {
    uint32_t seq, state;
    int ok;
} kg_ota_t;

#define KG_IMG_VALID        0x2u
#define KG_IMG_UNDEFINED    0xffffffffu

// Reads what otadata says about each slot. Returns a bit per slot whose
// newest entry is new, on trial or failed: an image no one has kept. Sets
// `*kept` to the slot of the newest entry that is valid or undefined (one
// kept, or written without a trial), -1 if none.
unsigned kg_ota_untrusted(const kg_ota_t e[2], int nslots, int *kept);

// The order the power-up reset tries slots in: the known-good one (`good`,
// -1 when the record does not vouch for it), then the newest kept one, then
// the rest from ota_0 up, and last any slot holding an image no one kept.
// Fills `order` with nslots entries and returns nslots.
int  kg_reset_order(int good, int kept, unsigned untrusted, int nslots, int order[]);

// The otadata sequence number that makes the bootloader boot `slot`: the
// smallest past `active_seq` with (seq - 1) % nslots == slot (ESP-IDF's own
// mapping). `active_seq` is 0 when otadata has no valid entry.
uint32_t kg_ota_seq(uint32_t active_seq, int slot, int nslots);
