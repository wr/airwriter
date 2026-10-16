// SPDX-License-Identifier: GPL-3.0-or-later
// Firmware updates (W-1048): from the newest GitHub release, on request or by
// themselves, and from an upload to /update. Both take the job lock and hold
// it until the restart, both give way to a print job that arrives first, and
// both write through esp_ota_*, whose esp_ota_end refuses an image not signed
// with the key that signed the running one (SPEC.md 3.12). The decisions are
// the pure code in fw_update_core.h.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_ota_ops.h"          // ESP_ERR_OTA_VALIDATE_FAILED

typedef struct {
    int     (*try_lock)(void);          // the job lock, without waiting: 1 if taken
    void    (*unlock)(void);
    int     (*job_waiting)(void);       // a job found the lock taken since the last clear
    void    (*job_waiting_clear)(void);
    int64_t (*last_job_s)(void);        // uptime seconds of the last job begin or end, -1 never
    int       setup_mode;               // on the setup AP: GitHub is out of reach
    int       auto_on;                  // the stored setting
} fw_update_cfg_t;

// Reads which key signed the running image and starts the task (not in setup
// mode). Call once, after settings_load and ota_health_start.
esp_err_t fw_update_start(const fw_update_cfg_t *cfg);
// Wi-Fi joined. The first join starts auto-update's clock.
void      fw_update_net_up(void);

// GET /api/update's body.
void      fw_update_json(char *buf, size_t cap);
// Look for a newer release now. Returns at once; the result shows in the JSON.
esp_err_t fw_update_check(const char **why);
// Install the newest release known (tag NULL), or a release by its tag.
// Refusals that can be known now come back at once with a sentence in `why`.
esp_err_t fw_update_install(const char *tag, const char **why);
esp_err_t fw_update_set_auto(int on);

// /update: a raw image in the request body. One at a time. Errors:
// ESP_ERR_INVALID_STATE for "not now" (a job, the last update on trial, an
// update already running), ESP_ERR_OTA_VALIDATE_FAILED for a refused image,
// anything else for a failure; `why` is the page's sentence. After any error
// the update is over and the job lock is free again.
esp_err_t fw_update_stream_begin(size_t len, const char **why);
esp_err_t fw_update_stream_write(const void *buf, size_t n, const char **why);
// Verified and set to boot: reply, then fw_update_restart().
esp_err_t fw_update_stream_end(const char **why);
// The upload timed out `timeouts` times in a row. ESP_OK to keep waiting;
// otherwise the update is over (a job is waiting, or the upload stalled).
esp_err_t fw_update_stream_idle(int timeouts, const char **why);
void      fw_update_stream_abort(void);
void      fw_update_restart(void);
