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
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    // Fills `buf` with the status JSON body. The app owns what goes in it.
    void  (*status_json)(void *ctx, char *buf, size_t cap);
    void   *ctx;

    // Flashes TashTalk into the LocalTalk PIC and writes the result JSON into
    // `buf`. NULL leaves POST /api/localtalk/flash answering 404.
    void  (*localtalk_flash)(void *ctx, char *buf, size_t cap);
    // The LocalTalk node's state and log for GET /api/lt. NULL answers 404.
    void  (*lt_json)(void *ctx, char *buf, size_t cap);
    // The last PAP job's bytes for GET /api/lt/job; NULL (or no job) is 404.
    const uint8_t *(*lt_job)(void *ctx, size_t *len);
    // POST /api/lt/echo with a node number as the body: an AEP echo request.
    void  (*lt_echo)(void *ctx, int node);
    // POST /api/cable: runs the cable test (W-1055) and writes its JSON into
    // `buf`; a body of "both" tries J2 -> J4 whatever J4 -> J2 found. NULL
    // answers 404.
    void  (*cable_test)(void *ctx, int both, char *buf, size_t cap);
    // GET /api/et: the EtherTalk links' state (W-1076). NULL answers 404.
    void  (*et_json)(void *ctx, char *buf, size_t cap);
    // GET and POST /api/et/settings: the EtherTalk switches (radio: Macs on
    // this network, W-1078; tunnel: Basilisk II; infinite: Infinite Mac's
    // relay) and Infinite Mac's zone name.
    // set takes -1 and NULL to keep each, and returns 0, or nonzero for a zone
    // name it will not take. NULL answers 404.
    void  (*et_settings_get)(void *ctx, int *tunnel, int *radio, int *infinite, char *zone, size_t cap);
    int   (*et_settings_set)(void *ctx, int tunnel, int radio, int infinite, const char *zone);

    // W-1082: GET /api/laser, the LaserWriters heard and the one shared (the
    // card is open while the page asks), and POST /api/laser: {"share":"<NBP
    // name>"} or {"share":null}, {"dither":..,"gain":..} for its screen. set
    // takes unshare 1, or share, NULL dither and gain < 0 to keep each, and
    // returns 0 or an HTTP status (400, 409). NULL answers 404.
    void  (*laser_json)(void *ctx, char *buf, size_t cap);
    int   (*laser_set)(void *ctx, int unshare, const char *share, const char *dither, int gain);

    // W-1088: GET /api/printers, the printer list, and POST /api/printers,
    // one of its actions (printers_core.h's pr_post). post returns 0, or an
    // HTTP status with a sentence for the page in err. NULL answers 404.
    void  (*printers_json)(void *ctx, char *buf, size_t cap);
    int   (*printers_post)(void *ctx, const char *body, char *err, size_t cap);

    // The general settings (model, name, hostname, dither, gain, the AirPrint
    // and AppleTalk-port switches) were just saved: the app re-reads them, so
    // the next job prints with the screen the page shows and the switches take
    // effect. NULL and they wait for a restart.
    void  (*general_saved)(void *ctx);

    // W-1079: POST /api/restart, /api/erase and /api/test. restart and erase
    // answer before they act (restart waits for a printing page to finish);
    // test_page returns 0, or nonzero when the printer is busy. NULL answers 404.
    void  (*restart)(void *ctx);
    void  (*erase)(void *ctx);
    // W-1088: id is the printer's in the list, or -1 for the one the test
    // button prints on. Returns 0, 1 while the printer is busy, 2 for none or
    // one with no test page, or 3 (W-1116) when the record is not the printer
    // on the port -- another is -- in which case it writes the sentence for the
    // page into msg ("Connect <name> to the printer port first.").
    int   (*test_page)(void *ctx, int id, char *msg, size_t cap);

    // Serves the setup page for every unknown host, so joining the device's
    // access point opens the page by itself.
    int     captive;
} webui_cfg_t;

esp_err_t webui_start(const webui_cfg_t *cfg);
void      webui_stop(void);
