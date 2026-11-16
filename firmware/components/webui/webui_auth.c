// SPDX-License-Identifier: GPL-3.0-or-later
// Signing in to the settings page (W-1156; SPEC §3.20). The password never
// crosses the network: the board hands out a challenge, the page answers with
// an HMAC keyed by PBKDF2 of the password, and a right answer gets a token in
// a cookie the browser keeps for 6 months.
#include "webui_internal.h"
#include "auth.h"

#include <string.h>
#include <stdio.h>

#define COOKIE_MAX 600
#define SET_COOKIE "aw=%s; Max-Age=15552000; Path=/; HttpOnly; SameSite=Strict"

static const char *sentence(int rc)
{
    switch (rc) {
    case AUTH_STALE: return "Try again.";
    case AUTH_WRONG: return "Wrong password.";
    case AUTH_WAIT:  return "Too many tries. Try again in a minute.";
    case AUTH_SHORT: return "Use at least 8 characters.";
    case AUTH_CHARS: return "Use letters, numbers and punctuation only.";
    case AUTH_TAKEN: return "AirWriter already has a password.";
    default:         return "Try again.";
    }
}

static esp_err_t refuse(httpd_req_t *req, int rc, uint32_t wait_s)
{
    char ra[12];
    httpd_resp_set_status(req, rc == AUTH_WRONG ? "403 Forbidden"
                             : rc == AUTH_WAIT ? "429 Too Many Requests"
                             : rc == AUTH_TAKEN ? "409 Conflict" : "400 Bad Request");
    if (rc == AUTH_WAIT) {
        snprintf(ra, sizeof ra, "%u", (unsigned)(wait_s ? wait_s : 1));
        httpd_resp_set_hdr(req, "Retry-After", ra);
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, sentence(rc));
}

// The caller's token, if its Cookie header carries a well-formed one.
static int cookie_token(httpd_req_t *req, char tok[33])
{
    char h[COOKIE_MAX];
    if (httpd_req_get_hdr_value_str(req, "Cookie", h, sizeof h) != ESP_OK) return 0;
    return auth_cookie_token(h, tok);
}

int webui_signed_in(httpd_req_t *req, int captive)
{
    if (captive || auth_kind() == AUTH_NONE) return 1;
    char tok[33];
    return cookie_token(req, tok) && auth_token_ok_hex(tok);
}

esp_err_t webui_auth_refuse(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, auth_label() ? "{\"signin\":true,\"label\":true}"
                                                : "{\"signin\":true,\"label\":false}");
}

static esp_err_t signed_in_with(httpd_req_t *req, const char *tok)
{
    char sc[sizeof SET_COOKIE + 32];
    snprintf(sc, sizeof sc, SET_COOKIE, tok);
    httpd_resp_set_hdr(req, "Set-Cookie", sc);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t webui_auth_get(httpd_req_t *req)
{
    if (strncmp(req->uri, "/api/auth/nonce", 15) || (req->uri[15] && req->uri[15] != '?'))
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such page");
    char json[160];
    auth_nonce_json(json, sizeof json);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// A JSON string with an escape JSON.stringify never makes for printable
// ASCII (\n, \t, \u...): webui_json_str would drop its backslash and save
// something the person never typed.
static int odd_escape(const char *body, const char *key)
{
    char pat[16];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p || !(p = strchr(p + strlen(pat), '"'))) return 0;
    for (p++; *p && *p != '"'; p++) {
        if (*p != '\\') continue;
        if (p[1] != '"' && p[1] != '\\') return 1;
        p++;
    }
    return 0;
}

esp_err_t webui_auth_post(httpd_req_t *req)
{
    const char *what = req->uri + sizeof "/api/auth/" - 1;
    char body[BODY_MAX], nonce[40], proof[72], tok[33];
    uint32_t wait = 0;
    int rc;

    if (webui_read_body(req, body, sizeof body) < 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short body");

    if (!strcmp(what, "signin")) {
        if (!webui_json_str(body, "nonce", nonce, sizeof nonce) ||
            !webui_json_str(body, "proof", proof, sizeof proof)) return refuse(req, AUTH_STALE, 0);
        rc = auth_signin(nonce, proof, tok, &wait);
        return rc == AUTH_OK ? signed_in_with(req, tok) : refuse(req, rc, wait);
    }

    // Signing out and the password ask for a signed-in browser, as every other route does.
    if (!webui_signed_in(req, webui_captive())) return webui_auth_refuse(req);

    if (!strcmp(what, "signout")) {
        if (cookie_token(req, tok)) auth_signout(tok);
        httpd_resp_set_hdr(req, "Set-Cookie", "aw=; Max-Age=0; Path=/");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }

    if (!strcmp(what, "password")) {
        char pw[AUTH_PW_MAX + 2], enc[2 * AUTH_PW_MAX + 4];
        if (webui_json_str(body, "new", pw, sizeof pw)) {
            if (odd_escape(body, "new")) return refuse(req, AUTH_CHARS, 0);
            if (strlen(pw) > AUTH_PW_MAX) return refuse(req, AUTH_CHARS, 0);
            rc = auth_set_first(pw, tok);
            memset(pw, 0, sizeof pw);
            return rc == AUTH_OK ? signed_in_with(req, tok) : refuse(req, rc, 0);
        }
        if (!webui_json_str(body, "nonce", nonce, sizeof nonce) ||
            !webui_json_str(body, "proof", proof, sizeof proof)) return refuse(req, AUTH_STALE, 0);
        if (!webui_json_str(body, "enc", enc, sizeof enc) || strlen(enc) > 2 * AUTH_PW_MAX)
            return refuse(req, AUTH_CHARS, 0);
        rc = auth_change(cookie_token(req, tok) ? tok : NULL, nonce, proof, enc, &wait);
        if (rc != AUTH_OK) return refuse(req, rc, wait);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such page");
}
