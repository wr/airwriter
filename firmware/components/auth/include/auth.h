// SPDX-License-Identifier: GPL-3.0-or-later
// AirWriter's password on the board (W-1156; SPEC §3.20). A sold board's is
// burned into the chip with its serial and printed on its label; a test build
// can stand one in from CONFIG_AIRWRITER_TEST_PASSWORD; a person can change it,
// or set one on a board with none burned, and that one lives in NVS
// (namespace "auth") until the settings are erased.
//
// One lock guards the state: httpd's handlers and the status call in.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "auth_core.h"

typedef enum { AUTH_NONE, AUTH_LABEL, AUTH_CHANGED, AUTH_SET } auth_kind_t;

// A set or changed password that WPA2 won't take. Both answer 400, each with its own sentence.
enum { AUTH_SHORT = 1400, AUTH_CHARS = 1401 };

// Once, after nvs_flash_init: reads the chip, the build and NVS, and derives
// the sign-in key (AUTH_ITER rounds of PBKDF2; the time is logged).
esp_err_t   auth_init(void);

auth_kind_t auth_kind(void);
const char *auth_kind_name(void);         // "none", "label", "changed", "set"
const char *auth_password(void);          // NULL with AUTH_NONE
int         auth_label(void);             // a password burned in, or a test one: the kind on the label
int         auth_floor_applies(void);     // the same: such a board won't go back before sign-in

int  auth_token_ok_hex(const char *tok_hex);
// {"nonce":"<32 hex>","salt":"<32 hex>","iter":N}
void auth_nonce_json(char *buf, size_t cap);

// Each returns AUTH_OK or an auth_core.h code; *wait_s is set with AUTH_WAIT.
// On success tok_hex is the new browser's token (33 bytes with its zero).
int  auth_signin(const char *nonce_hex, const char *proof_hex, char tok_hex[33], uint32_t *wait_s);
void auth_signout(const char *tok_hex);
// The current password proves itself ("change"); the new one comes encrypted
// (auth_change_open). Only the caller's browser stays signed in.
int  auth_change(const char *tok_hex, const char *nonce_hex, const char *proof_hex,
                 const char *enc_hex, uint32_t *wait_s);
// A board with no password takes its first one in the clear (there is no key
// yet to hide it with). AUTH_TAKEN if it has one; AUTH_SHORT or AUTH_CHARS.
int  auth_set_first(const char *pw, char tok_hex[33]);
