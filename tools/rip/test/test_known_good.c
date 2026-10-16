// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the known-good slot (known_good_core, W-1059): the record and
// its two copies, where an update goes, what the power-up reset tries, and
// the otadata number that boots a slot.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "known_good_core.h"

static void blank(kg_rec_t two[2]) { memset(two, 0xff, 2 * sizeof two[0]); }

// Does the write the firmware does: the copy kg_next names is replaced.
static void promote(kg_rec_t two[2], int slot)
{
    kg_rec_t r;
    int copy = kg_next(two, slot, &r);
    assert(copy == 0 || copy == 1);
    assert(copy != kg_current(two));
    two[copy] = r;
}

// The bootloader's view of an otadata sequence number (bootloader_utility.c).
static int ota_slot(uint32_t seq, int n) { return (int)((seq - 1) % (uint32_t)n); }

int main(void)
{
    kg_rec_t two[2];

    assert(kg_crc32("123456789", 9) == 0xcbf43926u);

    // A blank record (a USB flash) means ota_0; so does an all-zero one.
    blank(two);
    assert(kg_current(two) == -1 && kg_slot(two, 3) == 0);
    memset(two, 0, sizeof two);
    assert(kg_current(two) == -1 && kg_slot(two, 3) == 0);

    // First write goes to copy 0, then they alternate, and the newest wins.
    blank(two);
    promote(two, 1);
    assert(kg_current(two) == 0 && two[0].seq == 1 && kg_slot(two, 3) == 1);
    promote(two, 2);
    assert(kg_current(two) == 1 && two[1].seq == 2 && kg_slot(two, 3) == 2);
    promote(two, 0);
    assert(kg_current(two) == 0 && two[0].seq == 3 && kg_slot(two, 3) == 0);
    promote(two, 1);
    assert(kg_current(two) == 1 && kg_slot(two, 3) == 1);

    // A write cut short: the copy being written is erased, half-written or
    // garbage, and the one that was current still answers.
    kg_rec_t keep[2];
    memcpy(keep, two, sizeof two);              // current: copy 1, slot 1
    kg_rec_t r;
    int copy = kg_next(two, 2, &r);
    assert(copy == 0);
    memset(&two[copy], 0xff, sizeof two[copy]);                 // cut after the erase
    assert(kg_slot(two, 3) == 1);
    two[copy] = r; two[copy].crc = 0xffffffffu;                 // cut before the last word
    assert(kg_slot(two, 3) == 1);
    two[copy] = r; ((uint8_t *)&two[copy])[5] ^= 0x10;          // a flipped bit
    assert(kg_slot(two, 3) == 1);
    memcpy(two, keep, sizeof two);

    // One copy damaged on its own: the other is used, whichever it is.
    two[1].slot ^= 3;
    assert(kg_current(two) == 0 && kg_slot(two, 3) == 0);
    memcpy(two, keep, sizeof two);
    two[0].magic = 0;
    assert(kg_current(two) == 1 && kg_slot(two, 3) == 1);

    // A record naming a slot this table lacks means ota_0, not a guess.
    blank(two);
    promote(two, 2);
    assert(kg_slot(two, 3) == 2 && kg_slot(two, 2) == 0);

    // An erased seq never counts, whatever else matches.
    kg_make(&two[0], 0xffffffffu, 1);
    memset(&two[1], 0xff, sizeof two[1]);
    assert(kg_current(two) == -1);

    // Updates: never the running slot, never the known-good one, and the
    // first after the running one when both are the same.
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
    t = kg_update_target(run, good, 3); assert(t == 1); run = t;    // B
    assert(kg_should_promote(1, 1, run, good)); good = run;         // B prints
    t = kg_update_target(run, good, 3); assert(t == 2); run = t;    // C, no print
    assert(!kg_should_promote(1, 0, run, good));
    t = kg_update_target(run, good, 3); assert(t == 0); run = t;    // D
    t = kg_update_target(run, good, 3); assert(t == 2);             // E over C, not B

    // Promotion needs all three.
    assert(kg_should_promote(1, 1, 2, 1));
    assert(!kg_should_promote(0, 1, 2, 1));     // on trial
    assert(!kg_should_promote(1, 0, 2, 1));     // no print yet
    assert(!kg_should_promote(1, 1, 1, 1));     // already known-good
    assert(!kg_should_promote(1, 1, -1, 1));    // not an OTA slot

    // The reset's order: known-good, the slot otadata boots, then the rest.
    int o[3];
    assert(kg_reset_order(1, 2, 3, o) == 3 && o[0] == 1 && o[1] == 2 && o[2] == 0);
    assert(kg_reset_order(1, 1, 3, o) == 3 && o[0] == 1 && o[1] == 0 && o[2] == 2);
    assert(kg_reset_order(2, -1, 3, o) == 3 && o[0] == 2 && o[1] == 0 && o[2] == 1);
    assert(kg_reset_order(0, 0, 3, o) == 3 && o[0] == 0 && o[1] == 1 && o[2] == 2);

    // otadata: the number written boots the slot, and beats the active one.
    assert(kg_ota_seq(0, 0, 3) == 1 && kg_ota_seq(0, 2, 3) == 3);  // blank: slot + 1, as IDF
    for (uint32_t act = 0; act < 50; act++)
        for (int s = 0; s < 3; s++) {
            uint32_t q = kg_ota_seq(act, s, 3);
            assert(q > act && q <= act + 3 && ota_slot(q, 3) == s);
        }
    assert(ota_slot(kg_ota_seq(0xfffffff0u, 1, 3), 3) == 1);

    printf("ok  known_good: record, update target, reset order, otadata seq\n");
    return 0;
}
