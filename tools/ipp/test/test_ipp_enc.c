// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"
#include "ipp_enc.h"
#include "ipp_walk.h"

int main(void)
{
    uint8_t buf[512];
    ipp_enc_t e;

    /* The response header is fixed-width and byte-exact: version 1.1, the
     * status code, then the request id echoed back. */
    ipp_enc_init(&e, buf, sizeof buf);
    ipp_resp_begin(&e, IPP_STATUS_OK, 0x01020304);
    CHECK_EQ_INT(ipp_enc_len(&e), 8);
    static const uint8_t want_hdr[] = { 0x01, 0x01, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04 };
    CHECK_EQ_MEM(buf, want_hdr, sizeof want_hdr);

    /* One string attribute, byte for byte: tag, name length, name, value
     * length, value. */
    ipp_enc_init(&e, buf, sizeof buf);
    ipp_attr_str(&e, IPP_TAG_CHARSET, "ab", "utf-8");
    static const uint8_t want_attr[] = {
        0x47, 0x00, 0x02, 'a', 'b', 0x00, 0x05, 'u', 't', 'f', '-', '8'
    };
    CHECK_EQ_INT(ipp_enc_len(&e), sizeof want_attr);
    CHECK_EQ_MEM(buf, want_attr, sizeof want_attr);

    /* An integer is always four bytes, big endian, and a boolean is one. */
    ipp_enc_init(&e, buf, sizeof buf);
    ipp_attr_int(&e, IPP_TAG_INTEGER, "n", -2);
    static const uint8_t want_int[] = {
        0x21, 0x00, 0x01, 'n', 0x00, 0x04, 0xFF, 0xFF, 0xFF, 0xFE
    };
    CHECK_EQ_MEM(buf, want_int, sizeof want_int);

    ipp_enc_init(&e, buf, sizeof buf);
    ipp_attr_bool(&e, "b", 1);
    static const uint8_t want_bool[] = { 0x22, 0x00, 0x01, 'b', 0x00, 0x01, 0x01 };
    CHECK_EQ_MEM(buf, want_bool, sizeof want_bool);

    /* A resolution is x, y, then the units byte -- nine bytes, not eight. */
    ipp_enc_init(&e, buf, sizeof buf);
    ipp_attr_res(&e, "r", 144, 72);
    static const uint8_t want_res[] = {
        0x32, 0x00, 0x01, 'r', 0x00, 0x09,
        0x00, 0x00, 0x00, 0x90, 0x00, 0x00, 0x00, 0x48, 0x03
    };
    CHECK_EQ_MEM(buf, want_res, sizeof want_res);

    ipp_enc_init(&e, buf, sizeof buf);
    ipp_attr_range(&e, "r", 1, 99);
    static const uint8_t want_range[] = {
        0x33, 0x00, 0x01, 'r', 0x00, 0x08,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x63
    };
    CHECK_EQ_MEM(buf, want_range, sizeof want_range);

    /* Additional values repeat the tag with an empty name. That is the whole
     * of multi-value encoding and it is easy to get wrong by emitting the name
     * again, which clients read as a second attribute. */
    ipp_enc_init(&e, buf, sizeof buf);
    static const char *const three[] = { "x", "yy", "z" };
    ipp_attr_strs(&e, IPP_TAG_KEYWORD, "k", three, 3);
    static const uint8_t want_multi[] = {
        0x44, 0x00, 0x01, 'k',  0x00, 0x01, 'x',
        0x44, 0x00, 0x00,       0x00, 0x02, 'y', 'y',
        0x44, 0x00, 0x00,       0x00, 0x01, 'z'
    };
    CHECK_EQ_INT(ipp_enc_len(&e), sizeof want_multi);
    CHECK_EQ_MEM(buf, want_multi, sizeof want_multi);

    /* An empty value list emits nothing at all rather than a nameless stub. */
    ipp_enc_init(&e, buf, sizeof buf);
    ipp_attr_strs(&e, IPP_TAG_KEYWORD, "k", three, 0);
    CHECK_EQ_INT(ipp_enc_len(&e), 0);
    CHECK(ipp_enc_ok(&e));

    /* Overflow is sticky and lossless in the other direction: nothing past the
     * cap is written, the length stops growing, and every later call is a
     * no-op so a caller can check once at the end. */
    uint8_t small[10];
    ipp_enc_init(&e, small, sizeof small);
    ipp_resp_begin(&e, IPP_STATUS_OK, 1);
    CHECK(ipp_enc_ok(&e));
    ipp_attr_str(&e, IPP_TAG_CHARSET, "attributes-charset", "utf-8");
    CHECK(!ipp_enc_ok(&e));
    size_t stuck = ipp_enc_len(&e);
    ipp_attr_int(&e, IPP_TAG_INTEGER, "n", 1);
    ipp_end(&e);
    CHECK_EQ_INT(ipp_enc_len(&e), stuck);
    CHECK(stuck <= sizeof small);
    CHECK(!ipp_enc_ok(&e));

    /* The walker used by the other tests must agree with the encoder, so
     * prove it round-trips before anything relies on it. */
    ipp_enc_init(&e, buf, sizeof buf);
    ipp_resp_begin(&e, IPP_STATUS_OK, 7);
    ipp_group(&e, IPP_TAG_PRINTER);
    ipp_attr_str(&e, IPP_TAG_NAME, "printer-name", "AirWriter");
    ipp_attr_strs(&e, IPP_TAG_KEYWORD, "k", three, 3);
    ipp_attr_int(&e, IPP_TAG_INTEGER, "queued-job-count", 42);
    ipp_end(&e);
    CHECK(ipp_enc_ok(&e));

    ipp_found_t f;
    CHECK(ipp_find(buf, ipp_enc_len(&e), "printer-name", &f));
    CHECK_EQ_INT(f.tag, IPP_TAG_NAME);
    CHECK_EQ_INT(f.group, IPP_TAG_PRINTER);
    CHECK(ipp_val_is(&f, "AirWriter"));

    CHECK(ipp_find(buf, ipp_enc_len(&e), "k", &f));
    CHECK_EQ_INT(f.nvalues, 3);

    CHECK(ipp_find(buf, ipp_enc_len(&e), "queued-job-count", &f));
    CHECK_EQ_INT(ipp_val_int(&f), 42);

    CHECK(!ipp_find(buf, ipp_enc_len(&e), "not-here", &f));

    TEST_END;
}
