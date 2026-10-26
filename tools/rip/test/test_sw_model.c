// SPDX-License-Identifier: GPL-3.0-or-later
//
// sw_model (W-1103): the StyleWriter model table, the readers for the
// printer's one-byte answers, and the numbers the page stage and the session
// pace by. The expected values here are written out from the plan's Global
// Constraints, not read back from the table.
#include "test_util.h"
#include "sw_model.h"

static sw_model_t id(const char *s, size_t n, int *cs)
{
    return sw_model_from_id((const uint8_t *)s, n, cs);
}

static void test_id(void)
{
    int cs = 7;
    CHECK_EQ_INT(id("SW\r", 3, &cs), SW_II);        CHECK_EQ_INT(cs, 0);
    CHECK_EQ_INT(id("SW3\r", 4, &cs), SW_1200);     CHECK_EQ_INT(cs, 0);
    CHECK_EQ_INT(id("IJ10\r", 5, &cs), SW_ORIGINAL); CHECK_EQ_INT(cs, 0);
    // A colour model's reply names no model: `p` does. A reply that ends
    // without its CR counts (the session reads to a 250 ms gap).
    CHECK_EQ_INT(id("CS", 2, &cs), SW_NONE);        CHECK_EQ_INT(cs, 1);
    CHECK_EQ_INT(id("CS\r", 3, &cs), SW_NONE);      CHECK_EQ_INT(cs, 1);
    CHECK_EQ_INT(id("SW", 2, &cs), SW_II);          CHECK_EQ_INT(cs, 0);
    CHECK_EQ_INT(id("XYZ\r", 4, &cs), SW_NONE);     CHECK_EQ_INT(cs, 0);
    CHECK_EQ_INT(id("", 0, &cs), SW_NONE);          CHECK_EQ_INT(cs, 0);
    // Not a prefix match, and nothing past the CR is read.
    CHECK_EQ_INT(id("SW33\r", 5, &cs), SW_NONE);
    CHECK_EQ_INT(id("S\r", 2, &cs), SW_NONE);
    CHECK_EQ_INT(id("SW\rCS", 5, &cs), SW_II);
    CHECK_EQ_INT(id("\rSW", 3, &cs), SW_NONE);
    // is_cs may be left out.
    CHECK_EQ_INT(id("SW3\r", 4, NULL), SW_1200);
    CHECK_EQ_INT(id("CS\r", 3, NULL), SW_NONE);
}

static void test_p(void)
{
    CHECK_EQ_INT(sw_model_from_p(0x01), SW_C2400);
    CHECK_EQ_INT(sw_model_from_p(0x02), SW_C2200);
    CHECK_EQ_INT(sw_model_from_p(0x04), SW_C1500);
    CHECK_EQ_INT(sw_model_from_p(0x05), SW_C2500);
    CHECK_EQ_INT(sw_model_from_p(0x09), SW_C2200);   // anything else: a 2200
    CHECK_EQ_INT(sw_model_from_p(0x00), SW_C2200);
    CHECK_EQ_INT(sw_model_from_p(0xFF), SW_C2200);
}

static void test_cart(void)
{
    CHECK_EQ_INT(sw_cart_colour(SW_C2200, 0x81), 1);
    CHECK_EQ_INT(sw_cart_colour(SW_C2200, 0x01), 0);
    CHECK_EQ_INT(sw_cart_colour(SW_C2200, 0x40), -1);
    CHECK_EQ_INT(sw_cart_colour(SW_C2200, 0x80), -1);
    CHECK_EQ_INT(sw_cart_colour(SW_C2400, 0x81), 1);
    CHECK_EQ_INT(sw_cart_colour(SW_C2500, 0x01), 0);
    CHECK_EQ_INT(sw_cart_colour(SW_C2500, 0x00), -1);
    // The 1500 says it in bit 0x80 alone.
    CHECK_EQ_INT(sw_cart_colour(SW_C1500, 0x80), 1);
    CHECK_EQ_INT(sw_cart_colour(SW_C1500, 0x81), 1);
    CHECK_EQ_INT(sw_cart_colour(SW_C1500, 0x00), 0);
    CHECK_EQ_INT(sw_cart_colour(SW_C1500, 0x7F), 0);
    // A black-only model has no colour cartridge to read; no model, no answer.
    CHECK_EQ_INT(sw_cart_colour(SW_II, 0x81), 0);
    CHECK_EQ_INT(sw_cart_colour(SW_NONE, 0x81), -1);
}

static void test_free_and_empty(void)
{
    CHECK_EQ_INT(sw_free_bytes(SW_II, 0xF8), 61440);
    CHECK_EQ_INT(sw_free_bytes(SW_ORIGINAL, 0xA0), 8192);     // x 64
    CHECK_EQ_INT(sw_free_bytes(SW_ORIGINAL, 0xFF), 255 * 64 - 2048);
    CHECK_EQ_INT(sw_free_bytes(SW_II, 0x00), 0);               // never below 0
    CHECK_EQ_INT(sw_free_bytes(SW_II, 0x08), 0);
    CHECK_EQ_INT(sw_free_bytes(SW_ORIGINAL, 0x1F), 0);
    CHECK_EQ_INT(sw_free_bytes(SW_C2500, 0x80), 30720);
    CHECK_EQ_INT(sw_free_bytes(SW_C2200, 0xA3), 0xA3 * 256 - 2048);
    CHECK_EQ_INT(sw_free_bytes(SW_NONE, 0xF8), 0);

    CHECK_EQ_INT(sw_b_empty(SW_C2200, 0xA3, 1), 1);
    CHECK_EQ_INT(sw_b_empty(SW_C2200, 0xA2, 0), 1);            // B & A0 == A0
    CHECK_EQ_INT(sw_b_empty(SW_C2200, 0x83, 1), 0);
    CHECK_EQ_INT(sw_b_empty(SW_C2500, 0x80, 1), 1);
    CHECK_EQ_INT(sw_b_empty(SW_C2500, 0x7F, 1), 0);
    CHECK_EQ_INT(sw_b_empty(SW_C2500, 0x81, 1), 0);
    CHECK_EQ_INT(sw_b_empty(SW_C1500, 0x9F, 0), 1);
    CHECK_EQ_INT(sw_b_empty(SW_C1500, 0x9F, 1), 0);
    CHECK_EQ_INT(sw_b_empty(SW_C1500, 0x87, 1), 1);
    CHECK_EQ_INT(sw_b_empty(SW_C1500, 0x87, 0), 0);
    CHECK_EQ_INT(sw_b_empty(SW_C2400, 0xF8, 1), 1);
    CHECK_EQ_INT(sw_b_empty(SW_C2400, 0xF7, 1), 0);
    CHECK_EQ_INT(sw_b_empty(SW_II, 0xF8, 0), 1);
    CHECK_EQ_INT(sw_b_empty(SW_II, 0x00, 0), 0);
    CHECK_EQ_INT(sw_b_empty(SW_1200, 0xF8, 0), 1);
    CHECK_EQ_INT(sw_b_empty(SW_ORIGINAL, 0xA0, 0), 1);
    CHECK_EQ_INT(sw_b_empty(SW_ORIGINAL, 0xF8, 0), 0);
    CHECK_EQ_INT(sw_b_empty(SW_NONE, 0xF8, 0), 0);
}

// sw_block_cap: the model's cap, or what an empty buffer can take less the
// block's four bytes of framing (G, the length, the 00), whichever is less.
static void test_block_cap(void)
{
    CHECK_EQ_INT(sw_block_cap(SW_C2500, 4), 30720 - 4);        // below 0x79FA
    CHECK_EQ_INT(sw_block_cap(SW_C2500, 1), 30720 - 4);        // below 0x7E77
    CHECK_EQ_INT(sw_block_cap(SW_C2200, 1), 0x7E77);
    CHECK_EQ_INT(sw_block_cap(SW_C2200, 4), 0x79FA);
    CHECK_EQ_INT(sw_block_cap(SW_C2400, 1), 0x7E77);
    CHECK_EQ_INT(sw_block_cap(SW_C2400, 4), 0x79FA);
    CHECK_EQ_INT(sw_block_cap(SW_II, 1), 0x76ED - 360);
    CHECK_EQ_INT(sw_block_cap(SW_1200, 1), 0x76ED - 360);
    CHECK_EQ_INT(sw_block_cap(SW_C1500, 4), 0x76ED - 4 * 360); // 0x87 reads 32512 free
    CHECK_EQ_INT(sw_block_cap(SW_C1500, 1), 0x76ED - 360);
    CHECK_EQ_INT(sw_block_cap(SW_ORIGINAL, 1), 8192 - 4);      // 0x2000 is a whole buffer
    CHECK_EQ_INT(sw_block_cap(SW_NONE, 1), 0);
    // A block of 0x8000 stalls a CS printer: no cap reaches it.
    for (int m = SW_ORIGINAL; m < SW_MODELS; m++)
        for (int p = 1; p <= 4; p += 3)
            CHECK(sw_block_cap((sw_model_t)m, p) < 0x8000);
}

static void test_printable(void)
{
    CHECK_EQ_INT(sw_printable_w(SW_II, 3060), 2880);           // Letter
    CHECK_EQ_INT(sw_printable_w(SW_C2400, 3060), 2916);
    CHECK_EQ_INT(sw_printable_w(SW_C2400, 2976), 2832);        // A4
    CHECK_EQ_INT(sw_printable_w(SW_II, 2976), 2832);
    CHECK_EQ_INT(sw_printable_w(SW_C2500, 2880 + 144), 2880);  // the paper is the limit
    CHECK_EQ_INT(sw_printable_w(SW_C2500, 100), 0);            // narrower than the margins
    CHECK_EQ_INT(sw_printable_w(SW_NONE, 3060), 0);
}

typedef struct {
    sw_model_t m;
    const char *name, *make_model;
    uint16_t across, ml, mr, mt, mb, band, dx;
    uint8_t colour, delta, cleans;
} want_t;

static void test_specs(void)
{
    // The plan's Global Constraints, a row a model.
    static const want_t want[] = {
        { SW_ORIGINAL, "StyleWriter",            "Apple StyleWriter",
          2880, 72, 72, 72, 114, 800,  0, 0, 0, 0 },
        { SW_II,       "StyleWriter II",         "Apple StyleWriter II",
          2880, 72, 72, 72, 114, 800, 16, 0, 1, 1 },
        { SW_1200,     "StyleWriter 1200",       "Apple StyleWriter 1200",
          2880, 72, 72, 72, 114, 800, 16, 0, 1, 1 },
        { SW_C1500,    "Color StyleWriter 1500", "Apple Color StyleWriter 1500",
          2880, 72, 72, 72, 114, 800, 16, 1, 1, 1 },
        { SW_C2200,    "Color StyleWriter 2200", "Apple Color StyleWriter 2200",
          2919, 72, 72, 45,  99, 400,  0, 1, 1, 1 },
        { SW_C2400,    "Color StyleWriter 2400", "Apple Color StyleWriter 2400",
          2919, 72, 72, 45,  99, 400,  0, 1, 1, 1 },
        { SW_C2500,    "Color StyleWriter 2500", "Apple Color StyleWriter 2500",
          2919, 72, 72, 45,  99, 400,  0, 1, 1, 1 },
    };
    CHECK_EQ_INT(sizeof want / sizeof want[0], SW_MODELS - 1);
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        const want_t *w = &want[i];
        const sw_spec_t *s = sw_spec(w->m);
        CHECK(s != NULL);
        if (!s) continue;
        CHECK(strcmp(s->name, w->name) == 0);
        CHECK(strcmp(s->make_model, w->make_model) == 0);
        CHECK_EQ_INT(s->dots_across, w->across);
        CHECK_EQ_INT(s->margin_l, w->ml);
        CHECK_EQ_INT(s->margin_r, w->mr);
        CHECK_EQ_INT(s->margin_t, w->mt);
        CHECK_EQ_INT(s->margin_b, w->mb);
        CHECK_EQ_INT(s->band_rows, w->band);
        CHECK_EQ_INT(s->rect_dx, w->dx);
        CHECK_EQ_INT(s->colour, w->colour);
        CHECK_EQ_INT(s->delta, w->delta);
        CHECK_EQ_INT(s->cleans, w->cleans);
    }
    CHECK(sw_spec(SW_NONE) == NULL);
    CHECK(sw_spec(SW_MODELS) == NULL);
    CHECK(sw_spec((sw_model_t)99) == NULL);
}

int main(void)
{
    test_id();
    test_p();
    test_cart();
    test_free_and_empty();
    test_block_cap();
    test_printable();
    test_specs();
    TEST_END;
}
