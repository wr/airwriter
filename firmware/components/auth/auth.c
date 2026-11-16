// SPDX-License-Identifier: GPL-3.0-or-later
#include "auth.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_efuse.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static const char *TAG = "auth";
#define NS "auth"

static SemaphoreHandle_t s_mu;
static auth_state_t s_st;
static auth_kind_t  s_kind = AUTH_NONE;
static int          s_label;                 // burned in, or a test password
static char         s_pw[AUTH_PW_MAX + 1];
static uint8_t      s_salt[16];

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void lock(void)   { xSemaphoreTake(s_mu, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mu); }

// The sign-in key, kept in NVS beside a check of the password and salt it
// came from, so a start-up only derives it when either has changed. (Whoever
// can read NVS over USB can read the password too.)
static void derive(void)
{
    uint8_t chk[32], saved[32];
    size_t n = sizeof saved;
    uint8_t in[sizeof s_salt + AUTH_PW_MAX];
    size_t pn = strlen(s_pw);
    memcpy(in, s_salt, sizeof s_salt);
    memcpy(in + sizeof s_salt, s_pw, pn);
    aw_sha256(in, sizeof s_salt + pn, chk);
    memset(in, 0, sizeof in);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) h = 0;
    if (h && nvs_get_blob(h, "keychk", saved, &n) == ESP_OK && n == 32 && !memcmp(saved, chk, 32)) {
        n = sizeof s_st.key;
        if (nvs_get_blob(h, "key", s_st.key, &n) == ESP_OK && n == sizeof s_st.key) {
            nvs_close(h);
            return;
        }
    }
    int64_t t0 = esp_timer_get_time();
    aw_pbkdf2_sha256((const uint8_t *)s_pw, pn, s_salt, sizeof s_salt, AUTH_ITER, s_st.key);
    ESP_LOGI(TAG, "sign-in key derived in %d ms", (int)((esp_timer_get_time() - t0) / 1000));
    if (h) {
        nvs_set_blob(h, "key", s_st.key, sizeof s_st.key);
        nvs_set_blob(h, "keychk", chk, sizeof chk);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void save_tokens(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (s_st.ntok) nvs_set_blob(h, "tok", s_st.tok, (size_t)s_st.ntok * 16);
    else           nvs_erase_key(h, "tok");
    nvs_commit(h);
    nvs_close(h);
}

static void save_pw(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "pw", s_pw);
    nvs_commit(h);
    nvs_close(h);
}

static void new_token(char tok_hex[33])
{
    uint8_t t[16];
    esp_fill_random(t, sizeof t);
    auth_token_add(&s_st, t);
    save_tokens();
    auth_hex_encode(t, 16, tok_hex);
}

esp_err_t auth_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    if (!s_mu) return ESP_ERR_NO_MEM;

    // The chip first: a sold board's password is burned in with its serial.
    // The serial stays out of the log: the board never shows it.
    char label_pw[AUTH_PW_MAX + 1] = {0};
    uint8_t blk[32];
    auth_label_t l;
    int burned = esp_efuse_read_block(EFUSE_BLK_USER_DATA, blk, 0, 256) == ESP_OK
                 ? auth_label_parse(blk, &l) : -1;
    if (burned == 1) {
        strcpy(label_pw, l.password);
        s_label = 1;
        ESP_LOGI(TAG, "password from the chip");
    } else if (burned < 0) {
        ESP_LOGE(TAG, "the chip's user block isn't AirWriter's: no password from it");
    }
    if (!s_label && CONFIG_AIRWRITER_TEST_PASSWORD[0]) {
        if (auth_pw_ok(CONFIG_AIRWRITER_TEST_PASSWORD)) {
            strcpy(label_pw, CONFIG_AIRWRITER_TEST_PASSWORD);
            s_label = 1;
            ESP_LOGW(TAG, "test password in this build (CONFIG_AIRWRITER_TEST_PASSWORD)");
        } else {
            ESP_LOGE(TAG, "the build's test password isn't 8-63 printable ASCII: ignored");
        }
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    char saved[AUTH_PW_MAX + 1] = {0};
    size_t n = sizeof saved;
    if (nvs_get_str(h, "pw", saved, &n) == ESP_OK && auth_pw_ok(saved)) {
        strcpy(s_pw, saved);
        s_kind = s_label ? AUTH_CHANGED : AUTH_SET;
    } else if (s_label) {
        strcpy(s_pw, label_pw);
        s_kind = AUTH_LABEL;
    }

    n = sizeof s_salt;
    if (nvs_get_blob(h, "salt", s_salt, &n) != ESP_OK || n != sizeof s_salt) {
        esp_fill_random(s_salt, sizeof s_salt);
        nvs_set_blob(h, "salt", s_salt, sizeof s_salt);
        nvs_commit(h);
    }

    n = sizeof s_st.tok;
    if (nvs_get_blob(h, "tok", s_st.tok, &n) == ESP_OK && n % 16 == 0)
        s_st.ntok = (uint8_t)(n / 16);
    nvs_close(h);

    if (s_kind != AUTH_NONE) derive();
    ESP_LOGI(TAG, "password: %s, %u browser%s signed in", auth_kind_name(),
             (unsigned)s_st.ntok, s_st.ntok == 1 ? "" : "s");
    return ESP_OK;
}

auth_kind_t auth_kind(void) { return s_kind; }

const char *auth_kind_name(void)
{
    switch (s_kind) {
    case AUTH_LABEL:   return "label";
    case AUTH_CHANGED: return "changed";
    case AUTH_SET:     return "set";
    default:           return "none";
    }
}

const char *auth_password(void) { return s_kind == AUTH_NONE ? NULL : s_pw; }
int auth_label(void)            { return s_label; }
int auth_floor_applies(void)    { return s_label; }

int auth_token_ok_hex(const char *tok_hex)
{
    uint8_t t[16];
    if (!tok_hex || auth_hex_decode(tok_hex, t, 16)) return 0;
    lock();
    int ok = auth_token_has(&s_st, t);
    unlock();
    return ok;
}

void auth_nonce_json(char *buf, size_t cap)
{
    uint8_t rnd[16], n[16];
    char nh[33], sh[33];
    esp_fill_random(rnd, sizeof rnd);
    lock();
    auth_nonce_new(&s_st, now_ms(), rnd, n);
    unlock();
    auth_hex_encode(n, 16, nh);
    auth_hex_encode(s_salt, 16, sh);
    snprintf(buf, cap, "{\"nonce\":\"%s\",\"salt\":\"%s\",\"iter\":%u}", nh, sh, (unsigned)AUTH_ITER);
}

int auth_signin(const char *nonce_hex, const char *proof_hex, char tok_hex[33], uint32_t *wait_s)
{
    uint8_t n[16], p[32];
    if (s_kind == AUTH_NONE) return AUTH_STALE;
    if (auth_hex_decode(nonce_hex, n, 16) || auth_hex_decode(proof_hex, p, 32)) return AUTH_STALE;
    lock();
    uint32_t now = now_ms();
    int r = auth_check_proof(&s_st, now, n, p, "signin");
    if (r == AUTH_WAIT) *wait_s = auth_wait_s(&s_st, now);
    if (r == AUTH_OK) new_token(tok_hex);
    unlock();
    if (r == AUTH_WRONG) ESP_LOGW(TAG, "a wrong password");
    if (r == AUTH_WAIT)  ESP_LOGW(TAG, "too many wrong passwords: waiting");
    return r;
}

void auth_signout(const char *tok_hex)
{
    uint8_t t[16];
    if (!tok_hex || auth_hex_decode(tok_hex, t, 16)) return;
    lock();
    auth_token_drop(&s_st, t);
    save_tokens();
    unlock();
}

int auth_change(const char *tok_hex, const char *nonce_hex, const char *proof_hex,
                const char *enc_hex, uint32_t *wait_s)
{
    uint8_t n[16], p[32], t[16], enc[AUTH_PW_MAX];
    size_t en = strlen(enc_hex) / 2;
    if (s_kind == AUTH_NONE) return AUTH_STALE;
    if (auth_hex_decode(nonce_hex, n, 16) || auth_hex_decode(proof_hex, p, 32)) return AUTH_STALE;
    int have_tok = tok_hex && !auth_hex_decode(tok_hex, t, 16);
    if (en < 8) return AUTH_SHORT;
    if (en > AUTH_PW_MAX || auth_hex_decode(enc_hex, enc, en)) return AUTH_CHARS;

    lock();
    uint32_t now = now_ms();
    int r = auth_check_proof(&s_st, now, n, p, "change");
    if (r == AUTH_WAIT) *wait_s = auth_wait_s(&s_st, now);
    if (r == AUTH_OK) {
        char pw[AUTH_PW_MAX + 1];
        if (auth_change_open(s_st.key, n, enc, en, pw, sizeof pw)) {
            r = AUTH_CHARS;
        } else {
            strcpy(s_pw, pw);
            memset(pw, 0, sizeof pw);
            save_pw();
            s_kind = s_label ? AUTH_CHANGED : AUTH_SET;
            derive();
            if (have_tok) auth_token_keep_only(&s_st, t);
            else          auth_token_keep_only(&s_st, (const uint8_t[16]){0});
            save_tokens();
            ESP_LOGI(TAG, "password changed");
        }
    }
    unlock();
    return r;
}

int auth_set_first(const char *pw, char tok_hex[33])
{
    if (strlen(pw) < 8) return AUTH_SHORT;
    if (!auth_pw_ok(pw)) return AUTH_CHARS;
    lock();
    int r = AUTH_TAKEN;
    if (s_kind == AUTH_NONE) {
        strcpy(s_pw, pw);
        save_pw();
        s_kind = AUTH_SET;
        derive();
        new_token(tok_hex);
        ESP_LOGI(TAG, "password set");
        r = AUTH_OK;
    }
    unlock();
    return r;
}
