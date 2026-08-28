// SPDX-License-Identifier: GPL-3.0-or-later
//
// Decodes the real iPhone capture end to end. The other URF tests are built
// from hand-written headers, so they only prove the decoder agrees with our
// reading of cups/raster-stream.c. This one proves it agrees with what iOS
// actually sends, which is the claim assumption 5 in SPEC.md section 4 rests on.
#include "test_util.h"
#include "urf_decode.h"
#include <stdlib.h>

#define IOS_URF "../../fixtures/ios-20260804-232652.urf"

static int file_read(void *ctx, uint8_t *out, size_t n)
{
    return fread(out, 1, n, (FILE *)ctx) == n;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : IOS_URF;
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "FAIL cannot open %s (run from tools/rip)\n", path);
        return 1;
    }

    urf_t u;
    CHECK_EQ_INT(urf_open(&u, file_read, f), URF_OK);
    CHECK_EQ_INT(u.is_pwg, 0);
    CHECK_EQ_INT(u.pages_total, 1);

    CHECK_EQ_INT(urf_next_page(&u), URF_OK);

    /* The table in fixtures/README.md. iOS honoured RS72-144: 1224 x 1584 at
     * 144 dpi is 8.5 x 11 inches including the margins, which is why the page
     * arrives 72 dots wider than the printer's 1152. */
    CHECK_EQ_INT(u.page.width, 1224);
    CHECK_EQ_INT(u.page.height, 1584);
    CHECK_EQ_INT(u.page.res_x, 144);
    CHECK_EQ_INT(u.page.res_y, 144);
    CHECK_EQ_INT(u.page.bits_per_pixel, 8);
    CHECK_EQ_INT(u.page.colorspace, 0);      /* sGray */
    CHECK_EQ_INT(u.page.num_colors, 1);
    CHECK_EQ_INT(u.page.bpp_bytes, 1);
    CHECK_EQ_INT(u.page.bytes_per_line, 1224);

    /* Every row must decode, and there must be exactly height of them. A real
     * capture is the only thing that exercises the RLE against streams we did
     * not generate ourselves. */
    const uint8_t *row;
    uint32_t rows = 0;
    int rc;
    while ((rc = urf_next_row(&u, &row)) == URF_OK) rows++;
    CHECK_EQ_INT(rc, URF_END);
    CHECK_EQ_INT(rows, 1584);

    /* One page, then a clean end of stream. */
    CHECK_EQ_INT(urf_next_page(&u), URF_END);

    urf_free(&u);
    fclose(f);
    TEST_END;
}
