// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"
#include "urf_decode.h"

/* Build a one-page URF around a caller-supplied raster body. */
static size_t build(uint8_t *out, uint32_t w, uint32_t h, uint8_t bpp,
                    uint8_t cspace, const uint8_t *body, size_t body_len)
{
    static const uint8_t magic[12] = { 'U','N','I','R','A','S','T',0, 0,0,0,1 };
    memcpy(out, magic, 12);
    uint8_t *p = out + 12;
    memset(p, 0, 32);
    p[0] = bpp; p[1] = cspace;
    p[12] = (uint8_t)(w >> 24); p[13] = (uint8_t)(w >> 16);
    p[14] = (uint8_t)(w >> 8);  p[15] = (uint8_t)w;
    p[16] = (uint8_t)(h >> 24); p[17] = (uint8_t)(h >> 16);
    p[18] = (uint8_t)(h >> 8);  p[19] = (uint8_t)h;
    p[23] = 144;
    memcpy(out + 44, body, body_len);
    return 44 + body_len;
}

int main(void)
{
    uint8_t file[512];
    const uint8_t *row;
    urf_t u;

    /* -- repeat run: L=0, control 0x07 means 7+1 = 8 copies of 0x80 -- */
    {
        const uint8_t body[] = { 0x00, 0x07, 0x80 };
        memsrc_t s = { file, build(file, 8, 1, 8, 0, body, sizeof body), 0 };
        CHECK_EQ_INT(urf_open(&u, memsrc_read, &s), URF_OK);
        CHECK_EQ_INT(urf_next_page(&u), URF_OK);
        CHECK_EQ_INT(urf_next_row(&u, &row), URF_OK);
        const uint8_t want[8] = { 0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80 };
        CHECK_EQ_MEM(row, want, 8);
        urf_free(&u);
    }

    /* -- literal run: control 0xFE means 257-254 = 3 verbatim values -- */
    {
        const uint8_t body[] = { 0x00, 0xFE, 0x11, 0x22, 0x33 };
        memsrc_t s = { file, build(file, 3, 1, 8, 0, body, sizeof body), 0 };
        CHECK_EQ_INT(urf_open(&u, memsrc_read, &s), URF_OK);
        CHECK_EQ_INT(urf_next_page(&u), URF_OK);
        CHECK_EQ_INT(urf_next_row(&u, &row), URF_OK);
        const uint8_t want[3] = { 0x11, 0x22, 0x33 };
        CHECK_EQ_MEM(row, want, 3);
        urf_free(&u);
    }

    /* -- control 0x80 clears to end of line; sGray is additive so 0xFF -- */
    {
        const uint8_t body[] = { 0x00, 0x00, 0x42, 0x80 };
        memsrc_t s = { file, build(file, 4, 1, 8, 0, body, sizeof body), 0 };
        CHECK_EQ_INT(urf_open(&u, memsrc_read, &s), URF_OK);
        CHECK_EQ_INT(urf_next_page(&u), URF_OK);
        CHECK_EQ_INT(urf_next_row(&u, &row), URF_OK);
        const uint8_t want[4] = { 0x42, 0xFF, 0xFF, 0xFF };
        CHECK_EQ_MEM(row, want, 4);
        urf_free(&u);
    }

    /* -- same, but CMYK (code 6) is subtractive, so the fill is 0x00 -- */
    {
        const uint8_t body[] = { 0x00, 0x80 };
        memsrc_t s = { file, build(file, 2, 1, 32, 6, body, sizeof body), 0 };
        CHECK_EQ_INT(urf_open(&u, memsrc_read, &s), URF_OK);
        CHECK_EQ_INT(urf_next_page(&u), URF_OK);
        CHECK_EQ_INT(u.page.bpp_bytes, 4);
        CHECK_EQ_INT(urf_next_row(&u, &row), URF_OK);
        const uint8_t want[8] = { 0,0,0,0, 0,0,0,0 };
        CHECK_EQ_MEM(row, want, 8);
        urf_free(&u);
    }

    /* -- runs count colour VALUES, not bytes: 24bpp repeat of 2 pixels -- */
    {
        const uint8_t body[] = { 0x00, 0x01, 0xAA, 0xBB, 0xCC };
        memsrc_t s = { file, build(file, 2, 1, 24, 1, body, sizeof body), 0 };
        CHECK_EQ_INT(urf_open(&u, memsrc_read, &s), URF_OK);
        CHECK_EQ_INT(urf_next_page(&u), URF_OK);
        CHECK_EQ_INT(u.page.bpp_bytes, 3);
        CHECK_EQ_INT(u.page.bytes_per_line, 6);
        CHECK_EQ_INT(urf_next_row(&u, &row), URF_OK);
        const uint8_t want[6] = { 0xAA,0xBB,0xCC, 0xAA,0xBB,0xCC };
        CHECK_EQ_MEM(row, want, 6);
        urf_free(&u);
    }

    /* -- line repeat byte: L=2 delivers the same row three times -- */
    {
        const uint8_t body[] = { 0x02, 0x01, 0x55 };
        memsrc_t s = { file, build(file, 2, 3, 8, 0, body, sizeof body), 0 };
        CHECK_EQ_INT(urf_open(&u, memsrc_read, &s), URF_OK);
        CHECK_EQ_INT(urf_next_page(&u), URF_OK);
        const uint8_t want[2] = { 0x55, 0x55 };
        for (int i = 0; i < 3; i++) {
            CHECK_EQ_INT(urf_next_row(&u, &row), URF_OK);
            CHECK_EQ_MEM(row, want, 2);
        }
        CHECK_EQ_INT(urf_next_row(&u, &row), URF_END);  /* height reached */
        urf_free(&u);
    }

    TEST_END;
}
