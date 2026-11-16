// SPDX-License-Identifier: GPL-3.0-or-later
// What webui.c and webui_auth.c (W-1156) share inside the component.
#pragma once
#include <stddef.h>
#include "esp_http_server.h"

#define BODY_MAX 512

int webui_json_str(const char *body, const char *key, char *out, size_t cap);
int webui_read_body(httpd_req_t *req, char *buf, size_t cap);

// 1 while the board serves its setup network's captive page.
int       webui_captive(void);
// 1 when this request may see the board: on the setup network, on a board
// with no password, or from a signed-in browser.
int       webui_signed_in(httpd_req_t *req, int captive);
// The 401 the page turns into its sign-in screen.
esp_err_t webui_auth_refuse(httpd_req_t *req);
// GET /api/auth/nonce, and POST /api/auth/signin, /signout and /password.
esp_err_t webui_auth_get(httpd_req_t *req);
esp_err_t webui_auth_post(httpd_req_t *req);
