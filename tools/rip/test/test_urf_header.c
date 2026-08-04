// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"
#include "urf_decode.h"

/* A 12-byte URF file header followed by one 32-byte page header.
 * Page: 8bpp, colorspace 0 (sGray), simplex, 1024 x 768, 144 dpi. */
static const uint8_t hdr[] = {
    'U','N','I','R','A','S','T', 0x00,
    0x00, 0x00, 0x00, 0x01,                 /* page count = 1, big endian */

    0x08,                                   /* [0]  bits per pixel */
    0x00,                                   /* [1]  colorspace 0 = sGray */
    0x00,                                   /* [2]  duplex */
    0x04,                                   /* [3]  quality = normal */
    0x00,                                   /* [4]  media type */
    0x00,                                   /* [5]  media position */
    0,0,0,0,0,0,                            /* [6..11] unknown */
    0x00, 0x00, 0x04, 0x00,                 /* [12] width  = 1024 */
    0x00, 0x00, 0x03, 0x00,                 /* [16] height = 768 */
    0x00, 0x00, 0x00, 0x90,                 /* [20] resolution = 144 */
    0,0,0,0,0,0,0,0                         /* [24..31] unknown */
};

int main(void)
{
    memsrc_t src = { hdr, sizeof hdr, 0 };
    urf_t u;

    CHECK_EQ_INT(urf_open(&u, memsrc_read, &src), URF_OK);
    CHECK_EQ_INT(u.pages_total, 1);

    CHECK_EQ_INT(urf_next_page(&u), URF_OK);
    CHECK_EQ_INT(u.page.width, 1024);
    CHECK_EQ_INT(u.page.height, 768);
    CHECK_EQ_INT(u.page.res_x, 144);
    CHECK_EQ_INT(u.page.res_y, 144);      /* URF has one resolution for both axes */
    CHECK_EQ_INT(u.page.bits_per_pixel, 8);
    CHECK_EQ_INT(u.page.num_colors, 1);
    CHECK_EQ_INT(u.page.bits_per_color, 8);
    CHECK_EQ_INT(u.page.bpp_bytes, 1);
    CHECK_EQ_INT(u.page.bytes_per_line, 1024);
    CHECK_EQ_INT(u.page.fill_byte, 0xFF); /* additive space: blank is white */

    urf_free(&u);

    /* A bad magic must be rejected, not guessed at. */
    uint8_t bad[sizeof hdr];
    memcpy(bad, hdr, sizeof hdr);
    bad[0] = 'X';
    memsrc_t bsrc = { bad, sizeof bad, 0 };
    urf_t bu;
    CHECK_EQ_INT(urf_open(&bu, memsrc_read, &bsrc), URF_ERR_MAGIC);

    /* An absurd width must be rejected to guard against overflow on 32-bit targets. */
    uint8_t overflow[sizeof hdr];
    memcpy(overflow, hdr, sizeof hdr);
    overflow[12] = 0x00; overflow[13] = 0x10; overflow[14] = 0x00; overflow[15] = 0x00; /* width = 0x00100000 */
    memsrc_t osrc = { overflow, sizeof overflow, 0 };
    urf_t ou;
    CHECK_EQ_INT(urf_open(&ou, memsrc_read, &osrc), URF_OK);
    CHECK_EQ_INT(urf_next_page(&ou), URF_ERR_UNSUPPORTED);

    TEST_END;
}
