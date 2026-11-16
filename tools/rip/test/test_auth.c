// SPDX-License-Identifier: GPL-3.0-or-later
// The password's core (W-1156). Every expected value was made with Python's
// hashlib and hmac, so the board, the page's script and the tools agree.
#include "test_util.h"
#include "auth_core.h"

static void unhex(const char *h, uint8_t *out, size_t n)
{
    if (auth_hex_decode(h, out, n) != 0) { fprintf(stderr, "bad hex in test: %s\n", h); g_failed++; }
}

static void hashes(void)
{
    uint8_t out[32], want[32];

    aw_sha256((const uint8_t *)"abc", 3, out);
    unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
    CHECK_EQ_MEM(out, want, 32);

    // FIPS 180-4's two-block message: the padding runs into a second block.
    const char *m2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    aw_sha256((const uint8_t *)m2, strlen(m2), out);
    unhex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", want, 32);
    CHECK_EQ_MEM(out, want, 32);

    uint8_t k[20];
    memset(k, 0x0b, sizeof k);
    aw_hmac_sha256(k, sizeof k, (const uint8_t *)"Hi There", 8, out);
    unhex("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", want, 32);
    CHECK_EQ_MEM(out, want, 32);

    static const struct { uint32_t c; const char *dk; } v[] = {
        { 1,    "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b" },
        { 2,    "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43" },
        { 4096, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a" },
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        aw_pbkdf2_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, v[i].c, out);
        unhex(v[i].dk, want, 32);
        CHECK_EQ_MEM(out, want, 32);
    }
}

// The sign-in and the change, end to end, as the page does them.
static const char *T_NONCE = "00112233445566778899aabbccddeeff";
static const char *T_K     = "0fdc64748538ed622377390df1a9dee40bc40b26399f794dcae012613e371826";
static const char *T_PROOF = "08b2b19ea45f330916decd12523dc41f3e9876564aa6d08dc96ec4cc0e1763ac";

static void transcript(void)
{
    uint8_t salt[16], k[32], want[32], nonce[16], proof[32], msg[22];
    for (int i = 0; i < 16; i++) salt[i] = (uint8_t)i;
    aw_pbkdf2_sha256((const uint8_t *)"k7m2x9qp", 8, salt, 16, 1000, k);
    unhex(T_K, want, 32);
    CHECK_EQ_MEM(k, want, 32);

    unhex(T_NONCE, nonce, 16);
    memcpy(msg, "signin", 6);
    memcpy(msg + 6, nonce, 16);
    aw_hmac_sha256(k, 32, msg, sizeof msg, proof);
    unhex(T_PROOF, want, 32);
    CHECK_EQ_MEM(proof, want, 32);

    uint8_t enc[14];
    char out[64];
    unhex("ecac32bd55dfe0e2784e171a1a87", enc, sizeof enc);
    CHECK_EQ_INT(auth_change_open(k, nonce, enc, sizeof enc, out, sizeof out), 0);
    CHECK(!strcmp(out, "new-password-1"));
    CHECK_EQ_INT(auth_change_open(k, nonce, enc, 0, out, sizeof out), -1);
    uint8_t big[64] = {0};
    CHECK_EQ_INT(auth_change_open(k, nonce, big, 64, out, sizeof out), -1);
    // Seven characters come out of the first seven bytes: too short.
    CHECK_EQ_INT(auth_change_open(k, nonce, enc, 7, out, sizeof out), -1);
}

static void label(void)
{
    uint8_t blk[32];
    auth_label_t l;
    CHECK_EQ_INT(auth_label_pack("2652AWF2611-002", "k7m2x9qp", blk), 0);
    CHECK_EQ_INT(blk[0], 1);
    CHECK(!memcmp(blk + 1, "2652AWF2611-002", 15));
    CHECK_EQ_INT(blk[16], 0);
    CHECK(!memcmp(blk + 17, "k7m2x9qp", 8));
    static const uint8_t zero7[7] = {0};
    CHECK_EQ_MEM(blk + 25, zero7, 7);

    CHECK_EQ_INT(auth_label_parse(blk, &l), 1);
    CHECK(!strcmp(l.serial, "2652AWF2611-002"));
    CHECK(!strcmp(l.password, "k7m2x9qp"));

    uint8_t blank[32] = {0};
    CHECK_EQ_INT(auth_label_parse(blank, &l), 0);

    uint8_t bad[32];
    memcpy(bad, blk, 32); bad[0] = 2;
    CHECK_EQ_INT(auth_label_parse(bad, &l), -1);
    memcpy(bad, blk, 32); bad[24] = 0;                      // seven characters
    CHECK_EQ_INT(auth_label_parse(bad, &l), -1);
    memcpy(bad, blk, 32); bad[17] = '0';                    // not in the alphabet
    CHECK_EQ_INT(auth_label_parse(bad, &l), -1);
    memcpy(bad, blk, 32); bad[28] = 0x5a;                   // the custom MAC's bytes
    CHECK_EQ_INT(auth_label_parse(bad, &l), -1);
    memcpy(bad, blk, 32); bad[1] = 0;                       // no serial
    CHECK_EQ_INT(auth_label_parse(bad, &l), -1);

    CHECK_EQ_INT(auth_label_pack("2652AWF2611-002", "k7m2x9q0", blk), -1);
    CHECK_EQ_INT(auth_label_pack("", "k7m2x9qp", blk), -1);
    CHECK_EQ_INT(auth_label_pack("12345678901234567", "k7m2x9qp", blk), -1);
}

static void rules(void)
{
    char s64[65], s63[64];
    memset(s64, 'a', 64); s64[64] = 0;
    memset(s63, 'a', 63); s63[63] = 0;
    CHECK_EQ_INT(auth_pw_ok("abcdefgh"), 1);
    CHECK_EQ_INT(auth_pw_ok("abcdefg"), 0);
    CHECK_EQ_INT(auth_pw_ok(s63), 1);
    CHECK_EQ_INT(auth_pw_ok(s64), 0);
    CHECK_EQ_INT(auth_pw_ok("p\xc3\xa4ssword"), 0);
    CHECK_EQ_INT(auth_pw_ok("a\"b\\cdefg"), 1);
    CHECK_EQ_INT(auth_pw_ok("abc\tdefgh"), 0);
    CHECK_EQ_INT(auth_pw_ok("a b c d e"), 1);

    CHECK_EQ_INT(auth_label_pw_ok("k7m2x9qp"), 1);
    CHECK_EQ_INT(auth_label_pw_ok("k7m2x9q"), 0);
    CHECK_EQ_INT(auth_label_pw_ok("k7m2x9qpp"), 0);
    CHECK_EQ_INT(auth_label_pw_ok("k7m2x9ql"), 0);
    CHECK_EQ_INT(auth_label_pw_ok("K7M2X9QP"), 0);
}

// A state whose key is the transcript's, so T_PROOF answers T_NONCE.
static void state_with_key(auth_state_t *s)
{
    memset(s, 0, sizeof *s);
    unhex(T_K, s->key, 32);
}

static void nonces(void)
{
    auth_state_t s;
    uint8_t n[16], got[16], proof[32];
    state_with_key(&s);
    unhex(T_NONCE, n, 16);
    unhex(T_PROOF, proof, 32);

    auth_nonce_new(&s, 1000, n, got);
    CHECK_EQ_MEM(got, n, 16);
    CHECK_EQ_INT(auth_check_proof(&s, 2000, n, proof, "signin"), AUTH_OK);
    CHECK_EQ_INT(auth_check_proof(&s, 2001, n, proof, "signin"), AUTH_STALE);   // used up

    auth_nonce_new(&s, 10000, n, got);
    CHECK_EQ_INT(auth_check_proof(&s, 10000 + AUTH_NONCE_MS + 1, n, proof, "signin"), AUTH_STALE);

    // A proof made for "signin" is no proof of a change.
    auth_nonce_new(&s, 20000, n, got);
    CHECK_EQ_INT(auth_check_proof(&s, 20001, n, proof, "change"), AUTH_WRONG);

    // A fifth challenge pushes out the oldest.
    state_with_key(&s);
    uint8_t r[5][16];
    for (int i = 0; i < 5; i++) { memset(r[i], 0x10 + i, 16); auth_nonce_new(&s, 100 + i, r[i], got); }
    memcpy(r[1], n, 16);
    state_with_key(&s);
    for (int i = 0; i < 5; i++) auth_nonce_new(&s, 100 + i, r[i], got);    // r[1] is T_NONCE
    CHECK_EQ_INT(auth_check_proof(&s, 200, r[0], proof, "signin"), AUTH_STALE);
    CHECK_EQ_INT(auth_check_proof(&s, 200, r[1], proof, "signin"), AUTH_OK);

    // Stale challenges never count toward the lockout.
    state_with_key(&s);
    uint8_t unknown[16];
    memset(unknown, 0x77, 16);
    for (int i = 0; i < 20; i++)
        CHECK_EQ_INT(auth_check_proof(&s, 300 + i, unknown, proof, "signin"), AUTH_STALE);
    CHECK_EQ_INT(s.fails, 0);
    CHECK_EQ_INT(auth_wait_s(&s, 400), 0);

    // The clock wraps: a challenge made just before it does is still good.
    state_with_key(&s);
    auth_nonce_new(&s, 0xFFFFFF00u, n, got);
    CHECK_EQ_INT(auth_check_proof(&s, 0x00000100u, n, proof, "signin"), AUTH_OK);
}

static void lockout(void)
{
    auth_state_t s;
    uint8_t n[16], got[16], proof[32], wrong[32];
    state_with_key(&s);
    unhex(T_NONCE, n, 16);
    unhex(T_PROOF, proof, 32);
    memset(wrong, 0, 32);

    for (int i = 0; i < AUTH_TRIES; i++) {
        auth_nonce_new(&s, 1000, n, got);
        CHECK_EQ_INT(auth_check_proof(&s, 1000, n, wrong, "signin"), AUTH_WRONG);
    }
    auth_nonce_new(&s, 1000, n, got);
    CHECK_EQ_INT(auth_check_proof(&s, 1000, n, proof, "signin"), AUTH_WAIT);
    CHECK_EQ_INT(auth_wait_s(&s, 1000), 60);
    CHECK_EQ_INT(auth_wait_s(&s, 1000 + 59500), 1);
    auth_nonce_new(&s, 1000 + AUTH_WAIT_MS, n, got);
    CHECK_EQ_INT(auth_check_proof(&s, 1000 + AUTH_WAIT_MS, n, proof, "signin"), AUTH_OK);
    CHECK_EQ_INT(s.fails, 0);
    CHECK_EQ_INT(auth_wait_s(&s, 1000 + AUTH_WAIT_MS), 0);

    // A success between wrong tries starts the count again.
    state_with_key(&s);
    for (int i = 0; i < AUTH_TRIES - 1; i++) {
        auth_nonce_new(&s, 5000, n, got);
        CHECK_EQ_INT(auth_check_proof(&s, 5000, n, wrong, "signin"), AUTH_WRONG);
    }
    auth_nonce_new(&s, 5000, n, got);
    CHECK_EQ_INT(auth_check_proof(&s, 5000, n, proof, "signin"), AUTH_OK);
    for (int i = 0; i < AUTH_TRIES - 1; i++) {
        auth_nonce_new(&s, 5000, n, got);
        CHECK_EQ_INT(auth_check_proof(&s, 5000, n, wrong, "signin"), AUTH_WRONG);
    }
    auth_nonce_new(&s, 5000, n, got);
    CHECK_EQ_INT(auth_check_proof(&s, 5000, n, proof, "signin"), AUTH_OK);
}

static void tokens(void)
{
    auth_state_t s;
    uint8_t t[9][16];
    memset(&s, 0, sizeof s);
    for (int i = 0; i < 9; i++) { memset(t[i], 0xA0 + i, 16); auth_token_add(&s, t[i]); }
    CHECK_EQ_INT(s.ntok, AUTH_TOKENS);
    CHECK_EQ_INT(auth_token_has(&s, t[0]), 0);
    for (int i = 1; i < 9; i++) CHECK_EQ_INT(auth_token_has(&s, t[i]), 1);

    auth_token_drop(&s, t[4]);
    CHECK_EQ_INT(auth_token_has(&s, t[4]), 0);
    CHECK_EQ_INT(s.ntok, AUTH_TOKENS - 1);
    // The order holds after a drop: the next add pushes out nothing, the one after t[1].
    auth_token_add(&s, t[0]);
    CHECK_EQ_INT(auth_token_has(&s, t[1]), 1);
    auth_token_add(&s, t[4]);
    CHECK_EQ_INT(auth_token_has(&s, t[1]), 0);
    CHECK_EQ_INT(auth_token_has(&s, t[2]), 1);

    auth_token_keep_only(&s, t[6]);
    CHECK_EQ_INT(s.ntok, 1);
    CHECK_EQ_INT(auth_token_has(&s, t[6]), 1);
    CHECK_EQ_INT(auth_token_has(&s, t[7]), 0);

    // Adding one already there doesn't keep it twice.
    auth_token_add(&s, t[6]);
    CHECK_EQ_INT(s.ntok, 1);

    uint8_t zero[16] = {0};
    memset(&s, 0, sizeof s);
    CHECK_EQ_INT(auth_token_has(&s, zero), 0);              // an empty store knows no token
}

static void hex(void)
{
    uint8_t b[2];
    char h[5];
    CHECK_EQ_INT(auth_hex_decode("0aFf", b, 2), 0);
    CHECK_EQ_INT(b[0], 0x0a);
    CHECK_EQ_INT(b[1], 0xff);
    auth_hex_encode(b, 2, h);
    CHECK(!strcmp(h, "0aff"));
    CHECK_EQ_INT(auth_hex_decode("0aF", b, 2), -1);
    CHECK_EQ_INT(auth_hex_decode("0aFg", b, 2), -1);
    CHECK_EQ_INT(auth_hex_decode("0aFf00", b, 2), -1);
    CHECK_EQ_INT(auth_hex_decode("", b, 2), -1);
}

int main(void)
{
    hashes();
    transcript();
    label();
    rules();
    nonces();
    lockout();
    tokens();
    hex();
    TEST_END;
}
