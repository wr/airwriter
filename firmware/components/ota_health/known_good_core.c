// SPDX-License-Identifier: GPL-3.0-or-later
// The known-good slot, pure C (known_good_core.h).
#include "known_good_core.h"

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

void kg_make(kg_rec_t *r, uint32_t seq, uint32_t slot)
{
    r->magic = KG_MAGIC;
    r->seq = seq;
    r->slot = slot;
    r->crc = kg_crc32(r, 12);
}

int kg_valid(const kg_rec_t *r)
{
    return r->magic == KG_MAGIC && r->seq != 0xffffffffu && r->crc == kg_crc32(r, 12);
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

int kg_next(const kg_rec_t two[2], int slot, kg_rec_t *out)
{
    int cur = kg_current(two);
    kg_make(out, cur < 0 ? 1 : two[cur].seq + 1, (uint32_t)slot);
    return cur < 0 ? 0 : !cur;
}

int kg_update_target(int running, int good, int nslots)
{
    if (running < 0) running = -1;     // not in an OTA slot: start from ota_0
    for (int i = 1; i <= nslots; i++) {
        int s = (running + i) % nslots;
        if (s != running && s != good) return s;
    }
    return -1;
}

int kg_should_promote(int kept, int printed, int running, int good)
{
    return kept && printed && running >= 0 && running != good;
}

int kg_reset_order(int good, int booting, int nslots, int order[])
{
    int n = 0;
    if (good >= 0 && good < nslots) order[n++] = good;
    if (booting >= 0 && booting < nslots && booting != good) order[n++] = booting;
    for (int s = 0; s < nslots; s++)
        if (s != good && s != booting) order[n++] = s;
    return n;
}

uint32_t kg_ota_seq(uint32_t active_seq, int slot, int nslots)
{
    uint32_t seq = active_seq + 1;
    while ((seq - 1) % (uint32_t)nslots != (uint32_t)slot) seq++;
    return seq;
}
