// SPDX-License-Identifier: GPL-3.0-or-later
#include "auth_core.h"
#include <string.h>

/* ── SHA-256 (FIPS 180-4) ─────────────────────────────────────────── */

typedef struct { uint32_t h[8]; uint8_t buf[64]; size_t nbuf; uint64_t len; } sha_t;

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

// One block from words: w[0..15] in, w[16..63] scratch.
static void sha_words(uint32_t h[8], uint32_t w[64])
{
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    sha_words(h, w);
}

static void sha_init(sha_t *s)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof iv);
    s->nbuf = 0;
    s->len = 0;
}

static void sha_update(sha_t *s, const uint8_t *m, size_t n)
{
    s->len += n;
    while (n) {
        size_t take = 64 - s->nbuf;
        if (take > n) take = n;
        memcpy(s->buf + s->nbuf, m, take);
        s->nbuf += take; m += take; n -= take;
        if (s->nbuf == 64) { sha_block(s->h, s->buf); s->nbuf = 0; }
    }
}

static void sha_final(sha_t *s, uint8_t out[32])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha_update(s, &pad, 1);
    pad = 0;
    while (s->nbuf != 56) sha_update(s, &pad, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, lenb, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i]     = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

void aw_sha256(const uint8_t *m, size_t n, uint8_t out[32])
{
    sha_t s;
    sha_init(&s);
    sha_update(&s, m, n);
    sha_final(&s, out);
}

/* ── HMAC (RFC 2104) and PBKDF2 (RFC 8018) ────────────────────────── */

// The inner and outer states after their pads, so PBKDF2 pays for them once.
typedef struct { sha_t in, out; } hmac_t;

static void hmac_init(hmac_t *h, const uint8_t *k, size_t kn)
{
    uint8_t key[64] = {0}, pad[64];
    if (kn > 64) aw_sha256(k, kn, key);
    else memcpy(key, k, kn);
    for (int i = 0; i < 64; i++) pad[i] = key[i] ^ 0x36;
    sha_init(&h->in);
    sha_update(&h->in, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = key[i] ^ 0x5c;
    sha_init(&h->out);
    sha_update(&h->out, pad, 64);
}

static void hmac_run(const hmac_t *h, const uint8_t *m, size_t mn, uint8_t out[32])
{
    sha_t s = h->in;
    uint8_t inner[32];
    sha_update(&s, m, mn);
    sha_final(&s, inner);
    s = h->out;
    sha_update(&s, inner, 32);
    sha_final(&s, out);
}

void aw_hmac_sha256(const uint8_t *k, size_t kn, const uint8_t *m, size_t mn, uint8_t out[32])
{
    hmac_t h;
    hmac_init(&h, k, kn);
    hmac_run(&h, m, mn, out);
}

void aw_pbkdf2_sha256(const uint8_t *pw, size_t pwn, const uint8_t *salt, size_t sn,
                      uint32_t iter, uint8_t out[32])
{
    hmac_t h;
    hmac_init(&h, pw, pwn);

    // U1 = HMAC(pw, salt || INT(1)); the salt is at most the 16 bytes the board keeps.
    sha_t s = h.in;
    static const uint8_t one[4] = { 0, 0, 0, 1 };
    uint8_t u[32], inner[32];
    sha_update(&s, salt, sn);
    sha_update(&s, one, 4);
    sha_final(&s, inner);
    s = h.out;
    sha_update(&s, inner, 32);
    sha_final(&s, u);
    memcpy(out, u, 32);

    // Each later round is two compressions of one padded block (32 bytes of
    // digest, 0x80, the length 768 bits) from the pads' saved states, in words:
    // a board runs a whole round in what one hmac_run spent on its padding.
    uint32_t U[8], T[8], S[8], w[64];
    for (int j = 0; j < 8; j++)
        T[j] = U[j] = (uint32_t)u[4 * j] << 24 | (uint32_t)u[4 * j + 1] << 16 |
                      (uint32_t)u[4 * j + 2] << 8 | u[4 * j + 3];
    for (uint32_t i = 1; i < iter; i++) {
        memcpy(S, h.in.h, sizeof S);
        memcpy(w, U, 32);
        w[8] = 0x80000000u;
        memset(w + 9, 0, 6 * 4);
        w[15] = 768;
        sha_words(S, w);
        memcpy(U, h.out.h, sizeof U);
        memcpy(w, S, 32);
        w[8] = 0x80000000u;
        memset(w + 9, 0, 6 * 4);
        w[15] = 768;
        sha_words(U, w);
        for (int j = 0; j < 8; j++) T[j] ^= U[j];
    }
    for (int j = 0; j < 8; j++) {
        out[4 * j]     = (uint8_t)(T[j] >> 24);
        out[4 * j + 1] = (uint8_t)(T[j] >> 16);
        out[4 * j + 2] = (uint8_t)(T[j] >> 8);
        out[4 * j + 3] = (uint8_t)T[j];
    }
}

/* ── The password's rules and the eFuse block ─────────────────────── */

int auth_pw_ok(const char *pw)
{
    size_t n = 0;
    for (; pw[n]; n++) {
        unsigned char c = (unsigned char)pw[n];
        if (c < 0x20 || c > 0x7e) return 0;
        if (n >= AUTH_PW_MAX) return 0;
    }
    return n >= 8;
}

int auth_label_pw_ok(const char *pw)
{
    for (int i = 0; i < 8; i++)
        if (!pw[i] || !strchr(AUTH_ALPHABET, pw[i])) return 0;
    return pw[8] == 0;
}

int auth_label_pack(const char *serial, const char *pw, uint8_t blk[32])
{
    size_t sn = strlen(serial);
    if (sn == 0 || sn > 16 || !auth_label_pw_ok(pw)) return -1;
    memset(blk, 0, 32);
    blk[0] = 1;
    memcpy(blk + 1, serial, sn);
    memcpy(blk + 17, pw, 8);
    return 0;
}

int auth_label_parse(const uint8_t blk[32], auth_label_t *out)
{
    int any = 0;
    for (int i = 0; i < 32; i++) any |= blk[i];
    if (!any) return 0;
    if (blk[0] != 1) return -1;
    for (int i = 25; i < 32; i++) if (blk[i]) return -1;

    // The serial: printable ASCII, then only zeros to byte 16.
    size_t sn = 0;
    while (sn < 16 && blk[1 + sn]) {
        if (blk[1 + sn] < 0x21 || blk[1 + sn] > 0x7e) return -1;
        sn++;
    }
    if (sn == 0) return -1;
    for (size_t i = sn; i < 16; i++) if (blk[1 + i]) return -1;

    char pw[9];
    memcpy(pw, blk + 17, 8);
    pw[8] = 0;
    if (!auth_label_pw_ok(pw)) return -1;

    memcpy(out->serial, blk + 1, sn);
    out->serial[sn] = 0;
    memcpy(out->password, pw, 9);
    return 1;
}

/* ── Challenges, the lockout and tokens ───────────────────────────── */

void auth_nonce_new(auth_state_t *s, uint32_t now_ms, const uint8_t rnd[16], uint8_t out[16])
{
    int slot = -1;
    for (int i = 0; i < AUTH_NONCES; i++)
        if (!s->nonce_live[i]) { slot = i; break; }
    if (slot < 0) {
        // All live: the one made longest ago goes.
        slot = 0;
        for (int i = 1; i < AUTH_NONCES; i++)
            if (now_ms - s->nonce_at[i] > now_ms - s->nonce_at[slot]) slot = i;
    }
    memcpy(s->nonce[slot], rnd, 16);
    s->nonce_at[slot] = now_ms;
    s->nonce_live[slot] = 1;
    memcpy(out, rnd, 16);
}

static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

uint32_t auth_wait_s(const auth_state_t *s, uint32_t now_ms)
{
    if (!s->locked) return 0;
    uint32_t gone = now_ms - s->locked_at;
    if (gone >= AUTH_WAIT_MS) return 0;
    return (AUTH_WAIT_MS - gone + 999) / 1000;
}

int auth_check_proof(auth_state_t *s, uint32_t now_ms, const uint8_t nonce[16],
                     const uint8_t proof[32], const char *what)
{
    int slot = -1;
    for (int i = 0; i < AUTH_NONCES; i++)
        if (s->nonce_live[i] && !memcmp(s->nonce[i], nonce, 16)) { slot = i; break; }
    if (slot >= 0) s->nonce_live[slot] = 0;

    if (s->locked) {
        if (now_ms - s->locked_at < AUTH_WAIT_MS) return AUTH_WAIT;
        s->locked = 0;
        s->fails = 0;
    }
    if (slot < 0 || now_ms - s->nonce_at[slot] > AUTH_NONCE_MS) return AUTH_STALE;

    uint8_t msg[6 + 16], want[32];
    size_t wn = strlen(what);
    if (wn > 6) return AUTH_STALE;
    memcpy(msg, what, wn);
    memcpy(msg + wn, nonce, 16);
    aw_hmac_sha256(s->key, 32, msg, wn + 16, want);
    if (ct_equal(want, proof, 32)) {
        s->fails = 0;
        return AUTH_OK;
    }
    if (++s->fails >= AUTH_TRIES) {
        s->locked = 1;
        s->locked_at = now_ms;
    }
    return AUTH_WRONG;
}

int auth_token_has(const auth_state_t *s, const uint8_t t[16])
{
    for (int i = 0; i < s->ntok; i++)
        if (ct_equal(s->tok[i], t, 16)) return 1;
    return 0;
}

void auth_token_drop(auth_state_t *s, const uint8_t t[16])
{
    for (int i = 0; i < s->ntok; i++) {
        if (!memcmp(s->tok[i], t, 16)) {
            memmove(s->tok[i], s->tok[i + 1], (size_t)(s->ntok - i - 1) * 16);
            s->ntok--;
            memset(s->tok[s->ntok], 0, 16);
            return;
        }
    }
}

void auth_token_add(auth_state_t *s, const uint8_t t[16])
{
    auth_token_drop(s, t);                      // already there: it moves to newest
    if (s->ntok == AUTH_TOKENS) {
        memmove(s->tok[0], s->tok[1], (AUTH_TOKENS - 1) * 16);
        s->ntok--;
    }
    memcpy(s->tok[s->ntok++], t, 16);
}

void auth_token_keep_only(auth_state_t *s, const uint8_t t[16])
{
    int had = auth_token_has(s, t);
    memset(s->tok, 0, sizeof s->tok);
    s->ntok = 0;
    if (had) memcpy(s->tok[s->ntok++], t, 16);
}

/* ── A changed password, and hex ──────────────────────────────────── */

int auth_change_open(const uint8_t key[32], const uint8_t nonce[16], const uint8_t *enc, size_t n,
                     char *out, size_t cap)
{
    if (n == 0 || n > AUTH_PW_MAX || cap < n + 1) return -1;
    uint8_t stream[64], msg[6 + 16 + 1];
    memcpy(msg, "change", 6);
    memcpy(msg + 6, nonce, 16);
    for (int i = 0; i < 2; i++) {
        msg[22] = (uint8_t)(i + 1);
        aw_hmac_sha256(key, 32, msg, sizeof msg, stream + 32 * i);
    }
    for (size_t i = 0; i < n; i++) out[i] = (char)(enc[i] ^ stream[i]);
    out[n] = 0;
    if (!auth_pw_ok(out)) { memset(out, 0, n); return -1; }
    return 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int auth_hex_decode(const char *hex, uint8_t *out, size_t n)
{
    if (strlen(hex) != 2 * n) return -1;
    for (size_t i = 0; i < n; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

void auth_hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[in[i] >> 4];
        out[2 * i + 1] = d[in[i] & 15];
    }
    out[2 * n] = 0;
}

int auth_cookie_token(const char *h, char tok_hex[33])
{
    if (!h) return 0;
    while (*h) {
        while (*h == ' ' || *h == ';') h++;
        const char *end = h;
        while (*end && *end != ';') end++;
        if (end - h == 3 + 32 && !strncmp(h, "aw=", 3)) {
            uint8_t t[16];
            char v[33];
            memcpy(v, h + 3, 32);
            v[32] = 0;
            if (!auth_hex_decode(v, t, 16)) {
                auth_hex_encode(t, 16, tok_hex);
                return 1;
            }
        }
        h = end;
    }
    return 0;
}
