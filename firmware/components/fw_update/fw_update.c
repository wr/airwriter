// SPDX-License-Identifier: GPL-3.0-or-later
// Firmware updates on the device (fw_update.h). One task checks GitHub and
// installs from it; /update streams through the same lock, abort and panel
// code from the web server's task. Only one update runs at a time.
#include "fw_update.h"
#include "fw_update_core.h"

#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_secure_boot.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_grid.h"
#include "ota_health.h"
#include "settings.h"
#include "sdkconfig.h"

static const char *TAG = "fw_update";

// GitHub answers an asset download with a redirect to a signed URL of about
// 950 characters, which overflows esp_http_client's 512-byte defaults.
#define RX_BUF   4096
#define TX_BUF   2048
#define HTTP_TIMEOUT_MS 20000
#define MANIFEST_MAX    512
#define TICK_MS  30000          // how often auto-update looks at the clock
// A download that has not moved for this long is a dead connection (a Wi-Fi
// blip leaves TCP half-open, and esp_https_ota_perform keeps saying "in
// progress"). An upload gets the same, in timeouts of the web server's 20 s.
#define STALL_S  60
#define UPLOAD_STALL_TIMEOUTS 3
#define WHY_MAX  112

typedef enum { ST_IDLE, ST_CHECKING, ST_DOWNLOADING, ST_INSTALLING, ST_RESTARTING, ST_FAILED } st_t;
static const char *const ST_NAME[] = {
    "idle", "checking", "downloading", "installing", "restarting", "failed",
};
typedef enum { KEY_AIRWRITER, KEY_OTHER, KEY_UNSIGNED } key_t_;
static const char *const KEY_NAME[] = { "airwriter", "other", "unsigned" };

static fw_update_cfg_t s_cfg;
static SemaphoreHandle_t s_mx;          // guards everything below
static TaskHandle_t s_task;

static st_t    s_state;
static int     s_permille = -1;
static char    s_error[WHY_MAX];
static char    s_latest[32];            // the newest release's version, from its manifest
static char    s_file[65];              // and this chip's image in it
static int     s_newer;
static int     s_no_target;
static int64_t s_joined_s = -1, s_last_check_s = -1;
static int     s_check_failed, s_install_tried;
static int     s_auto;
static key_t_  s_key = KEY_UNSIGNED;
static int     s_updating;              // an install or an upload holds the job lock

// A request from the web page, for the task.
static int  s_req_check, s_req_install;
static char s_req_tag[32];              // "" for the newest

// The upload in progress.
static esp_ota_handle_t s_ota;
static const esp_partition_t *s_part;
static size_t s_stream_len, s_stream_done;

#define LOCK()   xSemaphoreTake(s_mx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mx)

static int64_t now_s(void) { return esp_timer_get_time() / 1000000; }

static void set_state(st_t st, const char *err)
{
    LOCK();
    s_state = st;
    if (err) snprintf(s_error, sizeof s_error, "%s", err);
    else if (st != ST_FAILED) s_error[0] = '\0';
    if (st != ST_DOWNLOADING) s_permille = -1;
    UNLOCK();
}

// The panel's overlay and the page's percentage, from one place. W-1049
// draws the overlays; until it lands they show nothing.
static void progress(size_t done, size_t total)
{
    uint16_t pm = total ? (uint16_t)((uint64_t)done * 1000 / total) : LG_PROGRESS_NONE;
    if (pm != LG_PROGRESS_NONE && pm > 1000) pm = 1000;
    led_grid_action(LG_ACT_FW_DOWNLOADING, pm);
    LOCK();
    int last = s_permille;
    s_permille = pm == LG_PROGRESS_NONE ? -1 : pm;
    UNLOCK();
    if (pm != LG_PROGRESS_NONE && pm / 100 != (last < 0 ? -1 : last / 100))
        ESP_LOGI(TAG, "%u%% of %u KB", pm / 10, (unsigned)(total >> 10));
}

// ------------------------------------------------------------- the gate
// Taken at the start of every update and kept to the restart: no job, test
// page or CTRL-X kick can begin meanwhile. A job that finds it taken raises
// job_waiting, and the update gives way to it.

static esp_err_t claim(const char **why)
{
    LOCK();
    int was = s_updating;
    if (!was) s_updating = 1;
    UNLOCK();
    if (was) { *why = "an update is already running"; return ESP_ERR_INVALID_STATE; }
    if (ota_health_on_trial()) {
        *why = "the last update is still finishing. Try again in a couple of minutes";
        goto busy;
    }
    if (!s_cfg.try_lock()) { *why = "a job is printing. Try again when it’s done"; goto busy; }
    s_cfg.job_waiting_clear();
    return ESP_OK;
busy:
    LOCK(); s_updating = 0; UNLOCK();
    return ESP_ERR_INVALID_STATE;
}

static void release(void)
{
    s_cfg.unlock();
    LOCK(); s_updating = 0; UNLOCK();
}

static const char *const WHY_JOB = "a job started printing. Try again when it’s done";
static const char *const WHY_REFUSED = "not a signed AirWriter update, or damaged";

// ------------------------------------------------------------- the upload

esp_err_t fw_update_stream_begin(size_t len, const char **why)
{
    esp_err_t err = claim(why);
    if (err != ESP_OK) return err;
    // Never the running slot or the known-good one (W-1059).
    const esp_partition_t *part = s_part = ota_health_update_target();
    err = part ? esp_ota_begin(part, len, &s_ota) : ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) {
        *why = err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE
            ? "the last update is still finishing. Try again in a couple of minutes"
            : "couldn’t start the update";
        release();
        return err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE ? ESP_ERR_INVALID_STATE : err;
    }
    ESP_LOGW(TAG, "upload: %u bytes into %s", (unsigned)len, part->label);
    s_stream_len = len;
    s_stream_done = 0;
    set_state(ST_DOWNLOADING, NULL);
    progress(0, len);
    return ESP_OK;
}

static void stream_fail(st_t st, const char *why)
{
    esp_ota_abort(s_ota);
    set_state(st, why);
    release();
}

esp_err_t fw_update_stream_write(const void *buf, size_t n, const char **why)
{
    if (s_cfg.job_waiting()) {
        ESP_LOGW(TAG, "upload stopped for a print job");
        led_grid_action(LG_ACT_FW_NONE, 0);
        *why = WHY_JOB;
        stream_fail(ST_IDLE, WHY_JOB);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_ota_write(s_ota, buf, n);
    if (err != ESP_OK) {
        // esp_ota_write checks the image's first bytes: a file that is not
        // a firmware image at all ends here.
        ESP_LOGE(TAG, "upload: write failed (%s)", esp_err_to_name(err));
        led_grid_action(LG_ACT_FW_REFUSED, 0);
        *why = err == ESP_ERR_OTA_VALIDATE_FAILED ? WHY_REFUSED : "couldn’t install the update";
        stream_fail(ST_FAILED, *why);
        return err == ESP_ERR_OTA_VALIDATE_FAILED ? err : ESP_FAIL;
    }
    s_stream_done += n;
    progress(s_stream_done, s_stream_len);
    return ESP_OK;
}

esp_err_t fw_update_stream_idle(int timeouts, const char **why)
{
    if (s_cfg.job_waiting()) {
        ESP_LOGW(TAG, "upload stopped for a print job");
        led_grid_action(LG_ACT_FW_NONE, 0);
        *why = WHY_JOB;
        stream_fail(ST_IDLE, WHY_JOB);
        return ESP_ERR_INVALID_STATE;
    }
    if (timeouts < UPLOAD_STALL_TIMEOUTS) return ESP_OK;
    ESP_LOGE(TAG, "upload stalled");
    led_grid_action(LG_ACT_FW_NONE, 0);
    *why = "the upload stalled";
    stream_fail(ST_FAILED, *why);
    return ESP_ERR_TIMEOUT;
}

void fw_update_stream_abort(void)
{
    led_grid_action(LG_ACT_FW_NONE, 0);
    stream_fail(ST_FAILED, "the upload was cut short");
}

esp_err_t fw_update_stream_end(const char **why)
{
    set_state(ST_INSTALLING, NULL);
    led_grid_action(LG_ACT_FW_VERIFYING, 0);
    // The image's own checks and its signature, before anything is made
    // bootable: a tampered, foreign or truncated upload cannot brick it.
    esp_err_t err = esp_ota_end(s_ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "upload refused (%s)", esp_err_to_name(err));
        led_grid_action(LG_ACT_FW_REFUSED, 0);
        *why = WHY_REFUSED;
        set_state(ST_FAILED, WHY_REFUSED);
        release();
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }
    const esp_partition_t *part = s_part;
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        led_grid_action(LG_ACT_FW_REFUSED, 0);
        *why = "couldn’t install the update";
        set_state(ST_FAILED, *why);
        release();
        return err;
    }
    ESP_LOGW(TAG, "upload installed into %s", part->label);
    led_grid_action(LG_ACT_FW_VERIFIED, 0);
    set_state(ST_RESTARTING, NULL);
    return ESP_OK;      // the lock stays taken: the restart is next
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));     // let the reply, and the panel, get out
    esp_restart();
}

void fw_update_restart(void)
{
    if (xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL) == pdPASS) return;
    // W-1094: with internal RAM short (23 bytes at its low on board 1) the
    // task could not be made, and the board sat "restarting" until someone
    // restarted it by hand. Then the caller's task does it: same wait, same end.
    ESP_LOGW(TAG, "no memory for the reboot task; restarting from here");
    reboot_task(NULL);
}

// ------------------------------------------------------------- GitHub

static char   s_body[MANIFEST_MAX];
static size_t s_body_len;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    // Only the final answer's body: a redirect's own body is not the manifest.
    if (e->event_id == HTTP_EVENT_ON_DATA &&
        esp_http_client_get_status_code(e->client) == 200) {
        size_t room = sizeof s_body - 1 - s_body_len;
        size_t n = (size_t)e->data_len < room ? (size_t)e->data_len : room;
        memcpy(s_body + s_body_len, e->data, n);
        s_body_len += n;
    }
    return ESP_OK;
}

static void http_cfg(esp_http_client_config_t *hc, const char *url)
{
    memset(hc, 0, sizeof *hc);
    hc->url = url;
    hc->crt_bundle_attach = esp_crt_bundle_attach;
    hc->buffer_size = RX_BUF;
    hc->buffer_size_tx = TX_BUF;
    hc->timeout_ms = HTTP_TIMEOUT_MS;
    hc->keep_alive_enable = true;
}

static void do_check(void)
{
    char url[160];
    if (!fwu_manifest_url(url, sizeof url, CONFIG_AIRWRITER_UPDATE_REPO)) {
        set_state(ST_FAILED, "the update repository setting is not owner/name");
        return;
    }
    set_state(ST_CHECKING, NULL);
    esp_http_client_config_t hc;
    http_cfg(&hc, url);
    hc.event_handler = on_http;
    s_body_len = 0;
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    esp_err_t err = c ? esp_http_client_perform(c) : ESP_ERR_NO_MEM;
    int status = c ? esp_http_client_get_status_code(c) : 0;
    if (c) esp_http_client_cleanup(c);
    s_body[s_body_len] = '\0';

    const char *app_ver = esp_app_get_description()->version;
    char ver[32] = "", file[65] = "";
    fwu_manifest_t m = FWU_M_BAD;
    const char *why = NULL;
    if (err != ESP_OK)       why = "can’t reach GitHub";
    else if (status == 404)  why = "no updates yet";
    else if (status != 200)  why = "couldn’t check for updates";
    else {
        m = fwu_parse_manifest(s_body, CONFIG_IDF_TARGET, ver, sizeof ver, file, sizeof file);
        if (m == FWU_M_BAD) why = "couldn’t check for updates";
    }
    if (why) ESP_LOGW(TAG, "check: %s (%s, HTTP %d)", why, esp_err_to_name(err), status);

    LOCK();
    s_last_check_s = now_s();
    s_check_failed = why != NULL;
    if (!why) {
        snprintf(s_latest, sizeof s_latest, "%s", ver);
        snprintf(s_file, sizeof s_file, "%s", m == FWU_M_OK ? file : "");
        s_no_target = m == FWU_M_NO_TARGET;
        s_newer = m == FWU_M_OK && fwu_newer(ver, app_ver);
        s_install_tried = 0;
    }
    int newer = s_newer;
    UNLOCK();
    // The TLS handshake is this task's deepest call; say how close it came.
    ESP_LOGI(TAG, "task stack: %u bytes never used", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (why) { set_state(ST_FAILED, why); return; }
    set_state(ST_IDLE, NULL);
    ESP_LOGI(TAG, "check: newest release %s%s; running %s", ver,
             m == FWU_M_NO_TARGET ? " (no image for this chip)" : newer ? " is newer" : "",
             app_ver);
}

// `tag` empty: the newest release, which must carry the version its manifest
// promised. Otherwise that tag's image, whatever version it is: an owner
// choosing an older or a pre-release build by name.
static void do_install(const char *tag, int automatic)
{
    char want[32], file[65], url[200];
    LOCK();
    s_install_tried = 1;
    if (tag[0]) {
        snprintf(want, sizeof want, "%s", tag);
        snprintf(file, sizeof file, "airwriter-%s.bin", CONFIG_IDF_TARGET);
    } else {
        snprintf(want, sizeof want, "%s", s_latest);
        snprintf(file, sizeof file, "%s", s_file);
    }
    UNLOCK();
    if (!fwu_asset_url(url, sizeof url, CONFIG_AIRWRITER_UPDATE_REPO, want, file)) {
        set_state(ST_FAILED, "no update to install");
        return;
    }
    const char *why = NULL;
    if (claim(&why) != ESP_OK) {
        // Not now is not a failed install. A long job reads as idle ten
        // minutes after it began, so auto-update asks every tick until it
        // ends; it says nothing, and tries again once the board is idle.
        LOCK(); s_install_tried = 0; UNLOCK();
        if (!automatic) set_state(ST_FAILED, why);
        return;
    }

    ESP_LOGW(TAG, "%sinstalling %s from %s", automatic ? "auto-update: " : "", want, url);
    set_state(ST_DOWNLOADING, NULL);
    progress(0, 0);
    esp_http_client_config_t hc;
    http_cfg(&hc, url);
    // Left NULL, esp_https_ota would pick IDF's next slot, which may be the
    // known-good one (W-1059).
    esp_https_ota_config_t oc = { .http_config = &hc,
                                  .partition.staging = ota_health_update_target() };
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (!oc.partition.staging) { why = "no room for the update"; goto fail; }
    err = esp_https_ota_begin(&oc, &h);
    if (err != ESP_OK) { why = "couldn’t download the update"; goto fail; }

    // The image's own description, before the rest of it comes down. It is
    // covered by the signature checked at the end, so what passes here is
    // what would install.
    esp_app_desc_t d;
    err = esp_https_ota_get_img_desc(h, &d);
    if (err != ESP_OK || strncmp(d.project_name, "airwriter", sizeof d.project_name)) {
        why = "the download isn’t AirWriter firmware";
        goto refuse;
    }
    if (!tag[0] && strncmp(d.version, want, sizeof d.version)) {
        why = "the download is the wrong version";
        goto refuse;
    }

    int last_len = -1;
    int64_t moved_s = now_s();
    for (;;) {
        err = esp_https_ota_perform(h);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int len = esp_https_ota_get_image_len_read(h);
        if (len != last_len) { last_len = len; moved_s = now_s(); }
        else if (now_s() - moved_s >= STALL_S) {
            err = ESP_ERR_TIMEOUT;
            why = "the download stalled";
            goto fail;
        }
        if (s_cfg.job_waiting()) {
            ESP_LOGW(TAG, "download stopped for a print job");
            esp_https_ota_abort(h);
            led_grid_action(LG_ACT_FW_NONE, 0);
            release();
            LOCK(); s_install_tried = 0; UNLOCK();     // try again once it is idle
            set_state(ST_IDLE, WHY_JOB);
            return;
        }
        int total = esp_https_ota_get_image_size(h);
        progress((size_t)len, total > 0 ? (size_t)total : 0);
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        why = "the download was cut short";
        goto fail;
    }

    set_state(ST_INSTALLING, NULL);
    led_grid_action(LG_ACT_FW_VERIFYING, 0);
    err = esp_https_ota_finish(h);      // esp_ota_end's checks, then set to boot
    h = NULL;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s refused (%s)", want, esp_err_to_name(err));
        why = err == ESP_ERR_OTA_VALIDATE_FAILED ? WHY_REFUSED : "couldn’t install the update";
        led_grid_action(LG_ACT_FW_REFUSED, 0);
        set_state(ST_FAILED, why);
        release();
        return;
    }
    ESP_LOGW(TAG, "%s installed; restarting", want);
    led_grid_action(LG_ACT_FW_VERIFIED, 0);
    set_state(ST_RESTARTING, NULL);
    vTaskDelay(pdMS_TO_TICKS(1500));    // the page sees it, the panel shows it
    esp_restart();

refuse:
    led_grid_action(LG_ACT_FW_REFUSED, 0);
    if (h) esp_https_ota_abort(h);
    ESP_LOGE(TAG, "%s: %s", want, why);
    set_state(ST_FAILED, why);
    release();
    return;
fail:
    led_grid_action(LG_ACT_FW_NONE, 0);
    if (h) esp_https_ota_abort(h);
    ESP_LOGE(TAG, "%s: %s (%s)", want, why, esp_err_to_name(err));
    set_state(ST_FAILED, why);
    release();
}

static fwu_inputs_t inputs(void)
{
    const char *app_ver = esp_app_get_description()->version;
    fwu_inputs_t in = {
        .auto_on = s_auto,
        .setup_mode = s_cfg.setup_mode,
        .key_ok = s_key == KEY_AIRWRITER,
        .on_trial = ota_health_on_trial(),
        .running_release = fwu_is_release(app_ver),
        .known_newer = s_newer,
        .newer_rolled_back = s_latest[0] && !strcmp(s_latest, ota_health_rolled_back()),
        .install_tried = s_install_tried,
        .check_failed = s_check_failed,
        .now_s = now_s(),
        .joined_s = s_joined_s,
        .last_check_s = s_last_check_s,
        .last_job_s = s_cfg.last_job_s(),
    };
    return in;
}

static void task(void *arg)
{
    (void)arg;
    const fwu_timing_t tm = {
        .first_s    = CONFIG_AIRWRITER_UPDATE_FIRST_MIN * 60LL,
        .interval_s = CONFIG_AIRWRITER_UPDATE_INTERVAL_H * 3600LL,
        .retry_s    = 3600,
        .idle_s     = CONFIG_AIRWRITER_UPDATE_IDLE_MIN * 60LL,
    };
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));
        LOCK();
        int check = s_req_check, install = s_req_install;
        char tag[sizeof s_req_tag];
        memcpy(tag, s_req_tag, sizeof tag);
        s_req_check = s_req_install = 0;
        fwu_inputs_t in = inputs();
        UNLOCK();

        if (install)    do_install(tag, 0);
        else if (check) do_check();
        else switch (fwu_policy(&in, &tm)) {
            case FWU_CHECK:   do_check(); break;
            case FWU_INSTALL: do_install("", 1); break;
            default: break;
        }
    }
}

// ------------------------------------------------------------- the page

static const char *ready_for_github(void)
{
    if (s_cfg.setup_mode) return "join a network to look for updates";
    if (s_key == KEY_OTHER)
        return "this version is signed with another key, so AirWriter updates can’t install";
    // IDF trusts the key that signed the running image; with none, it
    // refuses every image (only an image flashed unsigned by hand gets here).
    if (s_key == KEY_UNSIGNED)
        return "this version isn’t signed. Install a release over USB to get updates";
    return NULL;
}

esp_err_t fw_update_check(const char **why)
{
    if ((*why = ready_for_github())) return ESP_ERR_INVALID_STATE;
    LOCK();
    int running = s_updating || s_state == ST_CHECKING;
    if (!running) s_req_check = 1;
    UNLOCK();
    if (running) { *why = "an update is already running"; return ESP_ERR_INVALID_STATE; }
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t fw_update_install(const char *tag, const char **why)
{
    if ((*why = ready_for_github())) return ESP_ERR_INVALID_STATE;
    if (tag && tag[0] && !fwu_valid_tag(tag)) {
        *why = "that is not a release tag";
        return ESP_ERR_INVALID_ARG;
    }
    if (ota_health_on_trial()) {
        *why = "the last update is still finishing. Try again in a couple of minutes";
        return ESP_ERR_INVALID_STATE;
    }
    // A look at the job lock, for an answer now; the install takes it for real.
    if (!s_cfg.try_lock()) {
        *why = "a job is printing. Try again when it’s done";
        return ESP_ERR_INVALID_STATE;
    }
    s_cfg.unlock();
    LOCK();
    const char *no = s_updating || s_req_install ? "an update is already running"
                   : (!tag || !tag[0]) && !s_newer ? "no newer version. Check for updates first"
                   : NULL;
    if (!no) {
        s_req_install = 1;
        snprintf(s_req_tag, sizeof s_req_tag, "%s", tag ? tag : "");
    }
    UNLOCK();
    if (no) { *why = no; return ESP_ERR_INVALID_STATE; }
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t fw_update_set_auto(int on)
{
    esp_err_t err = settings_save_auto_update(on);
    if (err != ESP_OK) return err;
    LOCK(); s_auto = on ? 1 : 0; UNLOCK();
    if (s_task) xTaskNotifyGive(s_task);
    return ESP_OK;
}

void fw_update_json(char *buf, size_t cap)
{
    const char *ver = esp_app_get_description()->version;
    LOCK();
    int64_t ago = s_last_check_s < 0 ? -1 : now_s() - s_last_check_s;
    snprintf(buf, cap,
        "{\"version\":\"%s\",\"release\":%s,\"latest\":\"%s\",\"newer\":%s,\"no_target\":%s,"
        "\"state\":\"%s\",\"progress\":%d,\"error\":\"%s\",\"checked\":%lld,\"auto\":%s,"
        "\"key\":\"%s\",\"repo\":\"%s\",\"setup\":%s}",
        ver, fwu_is_release(ver) ? "true" : "false", s_latest, s_newer ? "true" : "false",
        s_no_target ? "true" : "false", ST_NAME[s_state],
        s_permille < 0 ? -1 : s_permille / 10, s_error, (long long)ago,
        s_auto ? "true" : "false", KEY_NAME[s_key], CONFIG_AIRWRITER_UPDATE_REPO,
        s_cfg.setup_mode ? "true" : "false");
    UNLOCK();
}

// ------------------------------------------------------------- start

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Which key signed this image: the release key, another, or none. Only
// spares a download that would be refused; esp_ota_end makes the real check.
static key_t_ running_key(void)
{
    esp_image_sig_public_key_digests_t d = {0};
    if (esp_secure_boot_get_signature_blocks_for_running_app(true, &d) != ESP_OK ||
        d.num_digests == 0)
        return KEY_UNSIGNED;
    const char *hex = CONFIG_AIRWRITER_UPDATE_KEY_DIGEST;
    if (strlen(hex) != 64) return KEY_OTHER;
    for (int i = 0; i < 32; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0 || d.key_digests[0][i] != (uint8_t)(hi << 4 | lo)) return KEY_OTHER;
    }
    return KEY_AIRWRITER;
}

esp_err_t fw_update_start(const fw_update_cfg_t *cfg)
{
    s_cfg = *cfg;
    s_auto = cfg->auto_on ? 1 : 0;
    s_mx = xSemaphoreCreateMutex();
    if (!s_mx) return ESP_ERR_NO_MEM;
    s_key = running_key();
    ESP_LOGI(TAG, "running %s, signed with %s key; auto-update %s",
             esp_app_get_description()->version,
             s_key == KEY_AIRWRITER ? "the AirWriter release" : s_key == KEY_OTHER ? "another"
                                                                  : "no",
             s_auto ? "on" : "off");
    if (cfg->setup_mode) return ESP_OK;     // /update still works; GitHub is out of reach
    // 8 KB, as IDF's own OTA examples give theirs: a TLS handshake with the
    // certificate bundle, then esp_ota_end's RSA-3072 check.
    if (xTaskCreate(task, "fw_update", 8192, NULL, 2, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void fw_update_net_up(void)
{
    if (!s_mx) return;
    LOCK();
    if (s_joined_s < 0) s_joined_s = now_s();
    UNLOCK();
}
