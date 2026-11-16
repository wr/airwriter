// SPDX-License-Identifier: GPL-3.0-or-later
// AirWriter's password (W-1156), pure C: the hashes the sign-in stands on, the
// eFuse block a sold board's password and serial are burned into, the rules a
// password follows, and the sign-in's state (nonces, tokens, the lockout).
//
// No ESP-IDF and no statics here: the board (auth.c) and the host tests build
// this file, and every bit of state is in the caller's auth_state_t. The clock
// comes in as milliseconds and may wrap; only differences are used.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define AUTH_TOKENS    8          // browsers kept signed in; a ninth pushes out the oldest
#define AUTH_NONCES    4          // sign-in challenges outstanding at once
#define AUTH_NONCE_MS  60000u     // a challenge is good once, for this long
#define AUTH_TRIES     5          // wrong proofs in a row before the wait
#define AUTH_WAIT_MS   60000u     // the wait
#define AUTH_ITER      100000u    // PBKDF2 rounds: a guess at a recorded sign-in costs this much

#define AUTH_ALPHABET  "abcdefghjkmnpqrstuvwxyz23456789"
#define AUTH_PW_MAX    63         // WPA2's longest passphrase

enum {
    AUTH_OK    = 0,
    AUTH_STALE = 400,             // an unknown, used or expired challenge: ask for a new one
    AUTH_WRONG = 403,
    AUTH_TAKEN = 409,             // a first password set when one exists
    AUTH_WAIT  = 429,
};

void aw_sha256(const uint8_t *m, size_t n, uint8_t out[32]);
void aw_hmac_sha256(const uint8_t *k, size_t kn, const uint8_t *m, size_t mn, uint8_t out[32]);
// One 32-byte block of PBKDF2-HMAC-SHA256.
void aw_pbkdf2_sha256(const uint8_t *pw, size_t pwn, const uint8_t *salt, size_t sn,
                      uint32_t iter, uint8_t out[32]);

// The user eFuse block (EFUSE_BLK_USER_DATA, 32 bytes): byte 0 the layout
// version (1), bytes 1-16 the serial, ASCII, zero-padded, bytes 17-24 the
// password, bytes 25-31 zero (on the S3 the custom MAC's field; never used).
typedef struct { char serial[17]; char password[9]; } auth_label_t;
// 1 burned (and *out filled), 0 blank (all zero), -1 anything else.
int auth_label_parse(const uint8_t blk[32], auth_label_t *out);
// 0, or -1 for a serial of 0 or more than 16 characters or a password that fails auth_label_pw_ok.
int auth_label_pack(const char *serial, const char *pw, uint8_t blk[32]);

// A password the person sets: 8 to 63 printable ASCII characters (WPA2's rules).
int auth_pw_ok(const char *pw);
// A burned password: 8 characters from AUTH_ALPHABET.
int auth_label_pw_ok(const char *pw);

typedef struct {
    uint8_t  key[32];                         // K, PBKDF2 of the current password
    uint8_t  nonce[AUTH_NONCES][16];
    uint32_t nonce_at[AUTH_NONCES];
    uint8_t  nonce_live[AUTH_NONCES];
    uint8_t  tok[AUTH_TOKENS][16];            // oldest first
    uint8_t  ntok;
    uint8_t  fails;                           // wrong proofs in a row
    uint8_t  locked;
    uint32_t locked_at;
} auth_state_t;

// Keeps rnd as a new challenge and copies it to out; the oldest goes when all are live.
void auth_nonce_new(auth_state_t *s, uint32_t now_ms, const uint8_t rnd[16], uint8_t out[16]);
// Checks HMAC-SHA256(key, what || nonce) against proof. what is "signin" or
// "change". The challenge is used up whatever the answer. AUTH_STALE is never
// counted toward the lockout; AUTH_WRONG is; while locked, AUTH_WAIT.
int auth_check_proof(auth_state_t *s, uint32_t now_ms, const uint8_t nonce[16],
                     const uint8_t proof[32], const char *what);
// Seconds left in the wait, rounded up; 0 when there is none.
uint32_t auth_wait_s(const auth_state_t *s, uint32_t now_ms);

void auth_token_add(auth_state_t *s, const uint8_t t[16]);
int  auth_token_has(const auth_state_t *s, const uint8_t t[16]);
void auth_token_drop(auth_state_t *s, const uint8_t t[16]);
void auth_token_keep_only(auth_state_t *s, const uint8_t t[16]);

// A new password sent encrypted with HMAC-SHA256(key, "change" || nonce || i)
// for i = 1, 2 (one byte each). 0 with out filled, or -1 when n is 0 or over
// 63, or what comes out fails auth_pw_ok.
int auth_change_open(const uint8_t key[32], const uint8_t nonce[16], const uint8_t *enc, size_t n,
                     char *out, size_t cap);

// Exactly 2n hex digits (either case) into n bytes: 0, or -1.
int  auth_hex_decode(const char *hex, uint8_t *out, size_t n);
// n bytes to 2n lowercase hex digits and a terminating zero.
void auth_hex_encode(const uint8_t *in, size_t n, char *out);
