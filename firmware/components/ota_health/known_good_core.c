// SPDX-License-Identifier: GPL-3.0-or-later
// The known-good slot, pure C (known_good_core.h).
#include "known_good_core.h"
#include <string.h>

_Static_assert(sizeof(kg_rec_t) == 32, "the record's layout is on every board's flash");

#define CRC_LEN offsetof(kg_rec_t, crc)

uint32_t kg_crc32(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *b++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

void kg_make(kg_rec_t *r, uint32_t seq, uint32_t slot, const uint8_t sha[KG_SHA_LEN])
{
    r->magic = KG_MAGIC;
    r->seq = seq;
    r->slot = slot;
    memcpy(r->sha, sha, KG_SHA_LEN);
    r->crc = kg_crc32(r, CRC_LEN);
}

int kg_valid(const kg_rec_t *r)
{
    return r->magic == KG_MAGIC && r->seq != 0xffffffffu && r->crc == kg_crc32(r, CRC_LEN);
}

int kg_current(const kg_rec_t two[2])
{
    int v0 = kg_valid(&two[0]), v1 = kg_valid(&two[1]);
    if (v0 && v1) return two[1].seq > two[0].seq ? 1 : 0;
    return v0 ? 0 : v1 ? 1 : -1;
}

int kg_slot(const kg_rec_t two[2], int nslots)
{
    int cur = kg_current(two);
    if (cur < 0 || two[cur].slot >= (uint32_t)nslots) return 0;
    return (int)two[cur].slot;
}

int kg_names(const kg_rec_t two[2], int nslots, int slot, const uint8_t sha[KG_SHA_LEN])
{
    int cur = kg_current(two);
    return cur >= 0 && slot >= 0 && slot < nslots && two[cur].slot == (uint32_t)slot &&
           sha && !memcmp(two[cur].sha, sha, KG_SHA_LEN);
}

int kg_vouches(const kg_rec_t two[2], int nslots, int slot, const uint8_t *sha)
{
    if (kg_current(two) < 0) return slot == 0;
    return kg_names(two, nslots, slot, sha);
}

int kg_next(const kg_rec_t two[2], int slot, const uint8_t sha[KG_SHA_LEN], kg_rec_t *out)
{
    int cur = kg_current(two);
    kg_make(out, cur < 0 ? 1 : two[cur].seq + 1, (uint32_t)slot, sha);
    return cur < 0 ? 0 : !cur;
}

int kg_update_target(int running, int avoid, int nslots)
{
    if (running < 0) running = -1;     // not in an OTA slot: start from ota_0
    for (int i = 1; i <= nslots; i++) {
        int s = (running + i) % nslots;
        if (s != running && s != avoid) return s;
    }
    return -1;
}

int kg_should_promote(int kept, int printed, int running, int named)
{
    return kept && printed && running >= 0 && !named;
}

unsigned kg_ota_untrusted(const kg_ota_t e[2], int nslots, int *kept)
{
    unsigned bad = 0;
    uint32_t newest[32] = {0}, kept_seq = 0;
    *kept = -1;
    for (int i = 0; i < 2; i++) {
        if (!e[i].ok || nslots <= 0 || nslots > 32) continue;
        int s = (int)((e[i].seq - 1) % (uint32_t)nslots);
        int good = e[i].state == KG_IMG_VALID || e[i].state == KG_IMG_UNDEFINED;
        // Both entries can name one slot; the newer one is what it holds now.
        if (e[i].seq >= newest[s]) {
            newest[s] = e[i].seq;
            bad = good ? bad & ~(1u << s) : bad | (1u << s);
        }
        if (good && e[i].seq > kept_seq) {
            kept_seq = e[i].seq;
            *kept = s;
        }
    }
    // A kept entry overtaken by a newer one for the same slot: that slot
    // now holds the newer, unkept image.
    if (*kept >= 0 && (bad & (1u << *kept))) *kept = -1;
    return bad;
}

int kg_reset_order(int good, int kept, unsigned untrusted, int nslots, int order[])
{
    int n = 0;
    unsigned listed = 0;
#define TAKE(s) do { order[n++] = (s); listed |= 1u << (s); } while (0)
    if (good >= 0 && good < nslots && !(untrusted & (1u << good))) TAKE(good);
    if (kept >= 0 && kept < nslots && !(listed & (1u << kept)) && !(untrusted & (1u << kept)))
        TAKE(kept);
    for (int s = 0; s < nslots; s++)
        if (!(listed & (1u << s)) && !(untrusted & (1u << s))) TAKE(s);
    for (int s = 0; s < nslots; s++)
        if (!(listed & (1u << s))) TAKE(s);
#undef TAKE
    return n;
}

uint32_t kg_ota_seq(uint32_t active_seq, int slot, int nslots)
{
    uint32_t seq = active_seq + 1;
    while ((seq - 1) % (uint32_t)nslots != (uint32_t)slot) seq++;
    return seq;
}
