// SPDX-License-Identifier: GPL-3.0-or-later
//
// The configuration page on port 80, and the firmware update that goes with
// it. Used in two situations that share almost all their code: as the settings
// page of a printer already on the network, and as the captive portal shown
// while the device is its own access point and has no credentials yet.
//
// esp_http_server is fine here, unlike for IPP: these requests carry a
// Content-Length, so its inability to read a chunked request body never comes
// up. See the note at the top of ipp_http.c.
#pragma once
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    // Fills `buf` with the status JSON body. The app owns what goes in it.
    void  (*status_json)(void *ctx, char *buf, size_t cap);
    void   *ctx;

    // Flashes TashTalk into the LocalTalk PIC and writes the result JSON into
    // `buf`. NULL leaves POST /api/localtalk/flash answering 404.
    void  (*localtalk_flash)(void *ctx, char *buf, size_t cap);
    // LocalTalk bring-up log for GET /api/lt (bench builds). NULL answers 404.
    void  (*lt_json)(void *ctx, char *buf, size_t cap);

    // Serves the setup page for every unknown host, so joining the device's
    // access point opens the page by itself.
    int     captive;
} webui_cfg_t;

esp_err_t webui_start(const webui_cfg_t *cfg);
void      webui_stop(void);
