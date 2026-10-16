// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the known-good slot (known_good_core, W-1059): the record and
// its two copies, where an update goes, what the power-up reset tries and in
// what order, and the otadata number that boots a slot.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "known_good_core.h"

static const uint8_t SHA_A[KG_SHA_LEN] = {0xa1, 0xa2, 0xa3};
static const uint8_t SHA_B[KG_SHA_LEN] = {0xb1, 0xb2, 0xb3};
static const uint8_t SHA_C[KG_SHA_LEN] = {0xc1, 0xc2, 0xc3};

static void blank(kg_rec_t two[2]) { memset(two, 0xff, 2 * sizeof two[0]); }

// Does the write the firmware does: the copy kg_next names is replaced.
static void promote(kg_rec_t two[2], int slot, const uint8_t *sha)
{
    kg_rec_t r;
    int copy = kg_next(two, slot, sha, &r);
    assert(copy == 0 || copy == 1);
    assert(copy != kg_current(two));
    two[copy] = r;
}

// The bootloader's view of an otadata sequence number (bootloader_utility.c).
static int ota_slot(uint32_t seq, int n) { return (int)((seq - 1) % (uint32_t)n); }

// An otadata entry: ok, seq, state (IDF's numbers).
enum { NEW = 0, PENDING = 1, VALID = 2, INVALID = 3, ABORTED = 4 };
static kg_ota_t ent(uint32_t seq, uint32_t state) { return (kg_ota_t){ seq, state, 1 }; }
static const kg_ota_t NONE = { 0xffffffffu, 0xffffffffu, 0 };

static void order_is(int good, int kept, unsigned bad, int a, int b, int c)
{
    int o[3];
    assert(kg_reset_order(good, kept, bad, 3, o) == 3);
    assert(o[0] == a && o[1] == b && o[2] == c);
}

int main(void)
{
    kg_rec_t two[2];

    assert(sizeof(kg_rec_t) == 32);
    assert(kg_crc32("123456789", 9) == 0xcbf43926u);

    // A blank record (a USB flash) means ota_0, vouches for whatever ota_0
    // holds, and names no image; so does an all-zero one.
    blank(two);
    assert(kg_current(two) == -1 && kg_slot(two, 3) == 0);
    assert(kg_vouches(two, 3, 0, SHA_A) && kg_vouches(two, 3, 0, NULL));
    assert(!kg_vouches(two, 3, 1, SHA_A));
    assert(!kg_names(two, 3, 0, SHA_A));
    memset(two, 0, sizeof two);
    assert(kg_current(two) == -1 && kg_slot(two, 3) == 0);

    // First write goes to copy 0, then they alternate, and the newest wins.
    blank(two);
    promote(two, 1, SHA_B);
    assert(kg_current(two) == 0 && two[0].seq == 1 && kg_slot(two, 3) == 1);
    assert(kg_names(two, 3, 1, SHA_B) && kg_vouches(two, 3, 1, SHA_B));
    promote(two, 2, SHA_C);
    assert(kg_current(two) == 1 && two[1].seq == 2 && kg_slot(two, 3) == 2);
    promote(two, 0, SHA_A);
    assert(kg_current(two) == 0 && two[0].seq == 3 && kg_slot(two, 3) == 0);
    promote(two, 1, SHA_B);
    assert(kg_current(two) == 1 && kg_slot(two, 3) == 1);

    // The record vouches for an image, not a slot: the same slot holding
    // something else (written by older firmware, or a USB flash of the app
    // alone), or no readable image at all, is not known-good.
    assert(kg_vouches(two, 3, 1, SHA_B));
    assert(!kg_vouches(two, 3, 1, SHA_C));
    assert(!kg_vouches(two, 3, 1, NULL));
    assert(!kg_vouches(two, 3, 0, SHA_B));      // a non-blank record never means ota_0
    assert(!kg_names(two, 3, 1, SHA_C));

    // A write cut short: the copy being written is erased, half-written or
    // garbage, and the one that was current still answers.
    kg_rec_t keep[2];
    memcpy(keep, two, sizeof two);              // current: copy 1, ota_1, B
    kg_rec_t r;
    int copy = kg_next(two, 2, SHA_C, &r);
    assert(copy == 0);
    memset(&two[copy], 0xff, sizeof two[copy]);                 // cut after the erase
    assert(kg_slot(two, 3) == 1 && kg_vouches(two, 3, 1, SHA_B));
    two[copy] = r; two[copy].crc = 0xffffffffu;                 // cut before the last word
    assert(kg_slot(two, 3) == 1 && kg_vouches(two, 3, 1, SHA_B));
    two[copy] = r; two[copy].sha[3] ^= 0x10;                    // a flipped bit
    assert(kg_slot(two, 3) == 1 && kg_vouches(two, 3, 1, SHA_B));
    memcpy(two, keep, sizeof two);

    // One copy damaged on its own: the other is used, whichever it is.
    two[1].slot ^= 3;
    assert(kg_current(two) == 0 && kg_slot(two, 3) == 0);
    memcpy(two, keep, sizeof two);
    two[0].magic = 0;
    assert(kg_current(two) == 1 && kg_slot(two, 3) == 1);

    // A record naming a slot this table lacks means ota_0 and vouches for
    // nothing.
    blank(two);
    promote(two, 2, SHA_C);
    assert(kg_slot(two, 3) == 2 && kg_slot(two, 2) == 0);
    assert(!kg_vouches(two, 2, 0, SHA_C) && !kg_names(two, 2, 2, SHA_C));

    // An erased seq never counts, whatever else matches.
    kg_make(&two[0], 0xffffffffu, 1, SHA_B);
    memset(&two[1], 0xff, sizeof two[1]);
    assert(kg_current(two) == -1);

    // Updates: never the running slot, never the named one, and the first
    // after the running one when both are the same.
    for (int run = 0; run < 3; run++)
        for (int good = 0; good < 3; good++) {
            int t = kg_update_target(run, good, 3);
            assert(t >= 0 && t < 3 && t != run && t != good);
            if (run == good) assert(t == (run + 1) % 3);
        }
    // Not running from an OTA slot (the old table's factory slot): any but good.
    assert(kg_update_target(-1, 0, 3) == 1);
    assert(kg_update_target(-1, 1, 3) == 0);
    // Two slots cannot keep a known-good copy apart from the running one.
    assert(kg_update_target(0, 1, 2) == -1);
    assert(kg_update_target(0, 0, 2) == 1);

    // The update chain the bench walks: B into ota_1, C into ota_2, D into
    // ota_0 while B is known-good, then E over C.
    int run = 0, good = 0, t;
    blank(two);
    t = kg_update_target(run, kg_slot(two, 3), 3); assert(t == 1); run = t;     // B
    assert(kg_should_promote(1, 1, run, kg_names(two, 3, run, SHA_B)));         // B prints
    promote(two, run, SHA_B); good = kg_slot(two, 3); assert(good == 1);
    assert(!kg_should_promote(1, 1, run, kg_names(two, 3, run, SHA_B)));        // once
    t = kg_update_target(run, good, 3); assert(t == 2); run = t;                // C, no print
    t = kg_update_target(run, good, 3); assert(t == 0); run = t;                // D
    t = kg_update_target(run, good, 3); assert(t == 2);                         // E over C, not B

    // A USB flash leaves a blank record: ota_0's image takes it over at its
    // first print, even though the blank record already means ota_0.
    blank(two);
    assert(kg_should_promote(1, 1, 0, kg_names(two, 3, 0, SHA_A)));

    // Promotion needs all of it.
    assert(kg_should_promote(1, 1, 2, 0));
    assert(!kg_should_promote(0, 1, 2, 0));     // on trial, or never kept
    assert(!kg_should_promote(1, 0, 2, 0));     // no print yet
    assert(!kg_should_promote(1, 1, 2, 1));     // the record names it already
    assert(!kg_should_promote(1, 1, -1, 0));    // not an OTA slot

    // otadata, read for the reset. Kept: the newest valid or undefined entry.
    // Untrusted: a slot whose newest entry is new, on trial or failed.
    kg_ota_t e[2];
    int kept;
    e[0] = ent(5, VALID); e[1] = NONE;                          // one kept image in ota_1
    assert(kg_ota_untrusted(e, 3, &kept) == 0 && kept == 1);
    e[0] = ent(5, VALID); e[1] = ent(6, PENDING);               // ota_2 on trial over ota_1
    assert(kg_ota_untrusted(e, 3, &kept) == 1u << 2 && kept == 1);
    e[0] = ent(5, VALID); e[1] = ent(6, NEW);                   // installed, never booted
    assert(kg_ota_untrusted(e, 3, &kept) == 1u << 2 && kept == 1);
    e[0] = ent(7, ABORTED); e[1] = ent(6, VALID);               // ota_0 failed, back on ota_2
    assert(kg_ota_untrusted(e, 3, &kept) == 1u << 0 && kept == 2);
    e[0] = ent(5, INVALID); e[1] = NONE;
    assert(kg_ota_untrusted(e, 3, &kept) == 1u << 1 && kept == -1);
    e[0] = ent(3, KG_IMG_UNDEFINED); e[1] = NONE;               // written without a trial
    assert(kg_ota_untrusted(e, 3, &kept) == 0 && kept == 2);
    e[0] = ent(4, VALID); e[1] = ent(7, NEW);                   // both name ota_0: the new one is there now
    assert(kg_ota_untrusted(e, 3, &kept) == 1u << 0 && kept == -1);
    e[0] = ent(7, VALID); e[1] = ent(4, ABORTED);               // and the other way round
    assert(kg_ota_untrusted(e, 3, &kept) == 0 && kept == 0);
    e[0] = NONE; e[1] = NONE;                                   // blank otadata (a USB flash)
    assert(kg_ota_untrusted(e, 3, &kept) == 0 && kept == -1);

    // The reset's order: known-good, the newest kept image, the rest, and
    // last any image no one kept.
    order_is(1, 2, 0, 1, 2, 0);
    order_is(1, 1, 0, 1, 0, 2);
    order_is(2, -1, 0, 2, 0, 1);
    order_is(0, 0, 0, 0, 1, 2);
    // Known-good damaged (-1): the kept image first, not the one on trial,
    // even though otadata boots the trial image now.
    order_is(-1, 1, 1u << 2, 1, 0, 2);
    order_is(-1, -1, 1u << 0, 1, 2, 0);
    // Known-good holding an image no one kept goes to the back.
    order_is(2, 0, 1u << 2, 0, 1, 2);
    // Nothing kept and nothing vouched for: everything, untrusted last.
    order_is(-1, -1, (1u << 0) | (1u << 1), 2, 0, 1);

    // The review's case: an update on trial in ota_2 over kept ota_1, and
    // the known-good ota_0 damaged. The kept image wins.
    e[0] = ent(5, VALID); e[1] = ent(6, PENDING);
    unsigned bad = kg_ota_untrusted(e, 3, &kept);
    int o[3];
    kg_reset_order(-1, kept, bad, 3, o);
    assert(o[0] == 1 && o[2] == 2);

    // otadata: the number written boots the slot, and beats the active one.
    assert(kg_ota_seq(0, 0, 3) == 1 && kg_ota_seq(0, 2, 3) == 3);  // blank: slot + 1, as IDF
    for (uint32_t act = 0; act < 50; act++)
        for (int s = 0; s < 3; s++) {
            uint32_t q = kg_ota_seq(act, s, 3);
            assert(q > act && q <= act + 3 && ota_slot(q, 3) == s);
        }
    assert(ota_slot(kg_ota_seq(0xfffffff0u, 1, 3), 3) == 1);

    printf("ok  known_good: record, image check, update target, reset order, otadata\n");
    return 0;
}
