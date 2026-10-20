// SPDX-License-Identifier: GPL-3.0-or-later
//
// ESP glue for EtherTalk; see ethertalk.h. One task owns both links under
// s_lock. The relay's WebSocket task never takes s_lock: the client holds its
// own lock while it runs our handler, and our task sends through that client,
// so the handler only queues what it receives and sets flags, and what the
// zone link sends is queued under s_lock and written by our task outside it.
#include "ethertalk.h"
#include <stdio.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "et_link.h"
#include "et_transport.h"

static const char *TAG = "et";

#define EV_LINES        24
#define LINE_LEN        100
#define ZQ_DEPTH        16
#define WS_MSG_MAX      4096           // one relay message: a 621-byte frame is ~2.5 KB of JSON
#define BACKOFF_MIN_MS  1000
#define BACKOFF_MAX_MS  60000
#define STEADY_MS       60000          // a connection this long resets the backoff

typedef struct { uint16_t n; uint8_t f[ELAP_MAX_FRAME]; } zframe_t;

typedef struct {
    et_link_t   link;
    at_owner_t  owner;
    const char *name;              // "tunnel", "zone": the log's prefix
    const char *nvs_net, *nvs_node;
    uint16_t    status;
    int         active;            // ticking: the tunnel with a socket and an address,
                                   // the zone while the relay is connected
    uint32_t    seen_replies;      // what the panel was last told (W-1049)
    uint8_t     seen_heard[32];
} et_side_t;

static SemaphoreHandle_t s_lock;           // the links, the settings, the pending flags
static SemaphoreHandle_t s_ring_lock;      // the log ring alone; never held around a send
static int        s_running;
static et_side_t  s_tun, s_zone;
static char       s_obj[NBP_NAME_MAX + 1], s_type[NBP_NAME_MAX + 1];
static void     (*s_on_event)(lt_event_t ev);
static char       s_ev[EV_LINES][LINE_LEN];
static unsigned   s_ev_head, s_ev_n;

// Settings, and what the task has yet to act on.
static int        s_tunnel_on = 1;
static char       s_zone_name[ET_ZONE_MAX + 1];
static int        s_want_tunnel = -1;      // -1: nothing to do
static int        s_want_zone;
static uint32_t   s_ip, s_new_ip;
static int        s_ip_changed, s_net_down;

// The tunnel.
static int        s_sock = -1;
static uint32_t   s_tun_drops;

// The relay.
static esp_websocket_client_handle_t s_ws;
static QueueHandle_t s_zin, s_zout;
static volatile int s_ws_up, s_ws_went_down, s_ws_came_up;
static volatile int64_t s_up_at;
static uint8_t    s_zone_mac[ETH_ALEN];
static volatile uint32_t s_zone_in_drops, s_zone_out_drops, s_zone_bad, s_zone_pongs;
static uint32_t   s_backoff_ms = BACKOFF_MIN_MS;
static char       s_zone_err[64];

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static void note(const char *who, const char *line)
{
    char l[LINE_LEN];
    snprintf(l, sizeof l, "%s: %s", who, line);
    ESP_LOGI(TAG, "%s", l);
    if (!s_ring_lock) return;
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    strncpy(s_ev[s_ev_head], l, LINE_LEN - 1);
    s_ev[s_ev_head][LINE_LEN - 1] = '\0';
    s_ev_head = (s_ev_head + 1) % EV_LINES;
    if (s_ev_n < EV_LINES) s_ev_n++;
    xSemaphoreGive(s_ring_lock);
}

static void tun_note(const char *line)  { note("tunnel", line); }
static void zone_note(const char *line) { note("zone", line); }

// ------------------------------------------------------------- link ops
// On our task with s_lock held, except abort (the print task).

static void op_log(void *ctx, const char *line) { note(((et_side_t *)ctx)->name, line); }
static int op_begin(void *ctx) { return at_job_begin(&((et_side_t *)ctx)->owner); }
static void op_data(void *ctx, const uint8_t *d, size_t n) { at_job_data(&((et_side_t *)ctx)->owner, d, n); }
static size_t op_room(void *ctx) { return at_job_room(&((et_side_t *)ctx)->owner); }
static void op_end(void *ctx, int normal) { at_job_end(&((et_side_t *)ctx)->owner, normal); }

static void op_claimed(void *ctx, uint16_t net, uint8_t node)
{
    et_side_t *s = ctx;
    nvs_handle_t h;
    if (nvs_open("et", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, s->nvs_net, net);
    nvs_set_u8(h, s->nvs_node, node);
    nvs_commit(h);
    nvs_close(h);
}

static void side_abort(void *ctx)
{
    et_side_t *s = ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    lt_node_pap_abort(&s->link.node, now_ms());
    xSemaphoreGive(s_lock);
}

static void tun_send(void *ctx, const uint8_t *f, size_t n)
{
    (void)ctx;
    uint32_t ip;
    if (s_sock < 0 || !et_tunnel_dest(f, &ip)) {
        s_tun_drops++;
        return;
    }
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(ET_TUNNEL_PORT) };
    to.sin_addr.s_addr = htonl(ip);
    if (sendto(s_sock, f, n, 0, (struct sockaddr *)&to, sizeof to) < 0) s_tun_drops++;
}

static void zone_send(void *ctx, const uint8_t *f, size_t n)
{
    (void)ctx;
    static zframe_t z;                             // s_lock held
    if (!s_ws_up || n > sizeof z.f) {
        s_zone_out_drops++;
        return;
    }
    z.n = (uint16_t)n;
    memcpy(z.f, f, n);
    if (xQueueSend(s_zout, &z, 0) != pdTRUE) s_zone_out_drops++;
}

static void side_init(et_side_t *s, const uint8_t mac[ETH_ALEN])
{
    uint16_t hnet = 0;
    uint8_t hnode = 0;
    nvs_handle_t h;
    if (nvs_open("et", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, s->nvs_net, &hnet);
        nvs_get_u8(h, s->nvs_node, &hnode);
        nvs_close(h);
    }
    et_cfg_t c = { s_obj, s_type, s->status, {0}, hnet, hnode, esp_random() | 1, LT_IDLE_HOLD_MS };
    memcpy(c.mac, mac, ETH_ALEN);
    et_ops_t o = { s, s == &s_tun ? tun_send : zone_send, op_log, op_begin, op_data, op_room,
                   op_end, op_claimed };
    et_link_init(&s->link, &c, &o, now_ms());
}

// ------------------------------------------------------------- the tunnel

static void tunnel_open(void)
{
    if (s_sock >= 0) return;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        note("tunnel", "no socket");
        return;
    }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(ET_TUNNEL_PORT) };
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        note("tunnel", "port 6066 is taken");
        return;
    }
    s_sock = fd;
}

static void tunnel_close(void)
{
    if (s_sock < 0) return;
    close(s_sock);
    s_sock = -1;
}

// ------------------------------------------------------------- the relay
// The handler runs on the client's task, holding the client's lock.

static void on_relay_message(esp_websocket_client_handle_t c, const char *msg, size_t len)
{
    static zframe_t z;                             // the client's task only
    static uint8_t f[ELAP_MAX_FRAME + 64];
    size_t n = 0;
    int r = et_zone_parse(msg, len, f, sizeof f, &n);
    if (r < 0) {
        s_zone_bad++;
        return;
    }
    if (r == 0) return;
    uint8_t pong[ET_PONG_LEN];
    if (et_zone_pong(f, n, s_zone_mac, pong)) {    // Infinite Mac's latency check
        char j[256];
        size_t jl = et_zone_send_json(j, sizeof j, pong, sizeof pong);
        if (jl) esp_websocket_client_send_text(c, j, (int)jl, pdMS_TO_TICKS(200));
        s_zone_pongs++;
        return;
    }
    if (n > sizeof z.f) {
        s_zone_bad++;
        return;
    }
    z.n = (uint16_t)n;
    memcpy(z.f, f, n);
    if (xQueueSend(s_zin, &z, 0) != pdTRUE) s_zone_in_drops++;
}

static void ws_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    static char msg[WS_MSG_MAX];
    esp_websocket_event_data_t *d = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        char init[80];
        size_t n = et_zone_init_json(init, sizeof init, s_zone_mac);
        if (n) esp_websocket_client_send_text(d->client, init, (int)n, pdMS_TO_TICKS(1000));
        s_up_at = esp_timer_get_time() / 1000;
        s_zone_err[0] = '\0';
        s_ws_up = 1;
        s_ws_came_up = 1;
        break;
    }
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code != 0x1 && d->op_code != 0x0) break;   // text, and its continuations
        if (d->payload_len > (int)sizeof msg || d->payload_offset < 0 ||
            d->payload_offset + d->data_len > (int)sizeof msg) {
            s_zone_bad++;
            break;
        }
        memcpy(msg + d->payload_offset, d->data_ptr, (size_t)d->data_len);
        if (d->payload_offset + d->data_len < d->payload_len) break;
        on_relay_message(d->client, msg, (size_t)d->payload_len);
        break;
    case WEBSOCKET_EVENT_ERROR: {
        // The handshake status outlives a failed attempt (it is the last
        // upgrade's), so the TLS and socket errors come first, and a 101 is
        // never the error.
        const esp_websocket_error_codes_t *e = &d->error_handle;
        if (e->esp_tls_last_esp_err)
            snprintf(s_zone_err, sizeof s_zone_err, "TLS: %s (%d)", esp_err_to_name(e->esp_tls_last_esp_err),
                     e->esp_tls_stack_err);
        else if (e->esp_ws_handshake_status_code && e->esp_ws_handshake_status_code != 101)
            snprintf(s_zone_err, sizeof s_zone_err, "the relay answered %d", e->esp_ws_handshake_status_code);
        else
            snprintf(s_zone_err, sizeof s_zone_err, "connection error (type %d)", (int)e->error_type);
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED: {
        // 1 s, doubling to 60 s; back to 1 s after a connection that held a minute.
        int64_t now = esp_timer_get_time() / 1000;
        if (s_ws_up && now - s_up_at >= STEADY_MS) s_backoff_ms = BACKOFF_MIN_MS;
        else s_backoff_ms = s_backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoff_ms * 2;
        esp_websocket_client_set_reconnect_timeout(d->client, (int)s_backoff_ms);
        s_ws_up = 0;
        s_ws_went_down = 1;
        break;
    }
    default:
        break;
    }
}

// Stops and frees the client; on our task, never the client's own.
static void zone_stop(void)
{
    if (!s_ws) return;
    if (s_ws_up) {
        static const char bye[] = "{\"type\":\"close\"}";
        esp_websocket_client_send_text(s_ws, bye, sizeof bye - 1, pdMS_TO_TICKS(200));
    }
    esp_websocket_client_stop(s_ws);
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
    s_ws_up = 0;
}

static void zone_start(const char *zone)
{
    if (s_ws || !zone[0]) return;
    char uri[96];
    snprintf(uri, sizeof uri, "wss://infinitemac.org/zone/%s/websocket", zone);
    esp_websocket_client_config_t c = {
        .uri = uri,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .task_stack = 6144,
        // Core 0, with Wi-Fi: a TLS handshake on core 1 stutters the panel (W-1060).
        .task_core_id_set = true,
        .task_core_id = 0,
        .reconnect_timeout_ms = BACKOFF_MIN_MS,
        .network_timeout_ms = 10000,
        // A relay restart closes cleanly; without this the client's task ends
        // and the zone stays down until the setting is saved again.
        .enable_close_reconnect = true,
        // Wi-Fi gone leaves the TLS socket looking open: pings find it in 45 s.
        .ping_interval_sec = 15,
        .pingpong_timeout_sec = 45,
    };
    s_backoff_ms = BACKOFF_MIN_MS;
    s_ws = esp_websocket_client_init(&c);
    if (!s_ws) {
        note("zone", "no memory for the relay");
        return;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_handler, NULL);
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        note("zone", "could not start the relay client");
        return;
    }
    note("zone", "connecting to Infinite Mac");
}

// ------------------------------------------------------------- the task

static void side_events(et_side_t *s, int *lookup, int *fresh)
{
    const lt_node_t *n = &s->link.node;
    if (n->st.replies != s->seen_replies) *lookup = 1;
    for (size_t k = 0; k < sizeof s->seen_heard; k++) {
        if (n->heard[k] & ~s->seen_heard[k]) *fresh = 1;
        s->seen_heard[k] = n->heard[k];
    }
    s->seen_replies = n->st.replies;
}

static void apply_pending(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int want_t = s_want_tunnel, want_z = s_want_zone, ipch = s_ip_changed, down = s_net_down;
    uint32_t ip = s_new_ip;
    char zone[ET_ZONE_MAX + 1];
    memcpy(zone, s_zone_name, sizeof zone);
    s_want_tunnel = -1;
    s_want_zone = 0;
    s_ip_changed = 0;
    s_net_down = 0;
    xSemaphoreGive(s_lock);
    uint64_t now = now_ms();

    if (down) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_tun.link.node.pap.open || s_zone.link.node.pap.open) note("both", "Wi-Fi lost; the job ends");
        et_link_down(&s_tun.link, now);
        et_link_down(&s_zone.link, now);
        xSemaphoreGive(s_lock);
    }

    if (want_t == 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        et_link_down(&s_tun.link, now);
        s_tun.active = 0;
        xSemaphoreGive(s_lock);
        tunnel_close();
        note("tunnel", "off");
    }
    if ((ipch && ip != s_ip) || want_t == 1) {
        if (ipch) s_ip = ip;
        if (s_tunnel_on) tunnel_open();
        uint8_t mac[ETH_ALEN];
        et_tunnel_mac(s_ip, mac);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_tunnel_on && s_sock >= 0 && s_ip) {
            et_link_set_mac(&s_tun.link, mac, now);    // a job on the old address ends
            s_tun.active = 1;
        } else {
            et_link_down(&s_tun.link, now);
            s_tun.active = 0;
        }
        xSemaphoreGive(s_lock);
        if (s_tun.active) note("tunnel", "listening for Basilisk II");
    }
    if (want_z) {
        zone_stop();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        et_link_down(&s_zone.link, now);
        s_zone.active = 0;
        xSemaphoreGive(s_lock);
        s_ws_came_up = s_ws_went_down = 0;
        if (zone[0]) zone_start(zone);
        else note("zone", "off");
    }
}

static void et_task(void *arg)
{
    (void)arg;
    static uint8_t buf[ELAP_MAX_FRAME + 64];
    static zframe_t z;
    static char json[WS_MSG_MAX];
    for (;;) {
        apply_pending();

        // The tunnel: up to 20 ms for a datagram.
        if (s_sock >= 0) {
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(s_sock, &rf);
            struct timeval tv = { 0, 20000 };
            if (select(s_sock + 1, &rf, NULL, NULL, &tv) > 0) {
                int n = recv(s_sock, buf, sizeof buf, 0);
                if (n > 0) {
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    if (s_tun.active) et_link_rx(&s_tun.link, buf, (size_t)n, now_ms());
                    xSemaphoreGive(s_lock);
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        // The relay's comings and goings, then what it brought.
        if (s_ws_came_up) {
            s_ws_came_up = 0;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            et_link_set_mac(&s_zone.link, s_zone_mac, now_ms());   // the address again
            s_zone.active = 1;
            xSemaphoreGive(s_lock);
            note("zone", "connected to Infinite Mac");
        }
        if (s_ws_went_down) {
            s_ws_went_down = 0;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_zone.active) et_link_down(&s_zone.link, now_ms());
            s_zone.active = 0;
            xSemaphoreGive(s_lock);
            char l[LINE_LEN];
            snprintf(l, sizeof l, "disconnected (%s); again in %lu s",
                     s_zone_err[0] ? s_zone_err : "closed", (unsigned long)(s_backoff_ms / 1000));
            note("zone", l);
        }
        while (xQueueReceive(s_zin, &z, 0) == pdTRUE) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_zone.active) et_link_rx(&s_zone.link, z.f, z.n, now_ms());
            xSemaphoreGive(s_lock);
        }

        // Timers, and what the panel should hear.
        int lookup = 0, fresh = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint64_t now = now_ms();
        if (s_tun.active) et_link_tick(&s_tun.link, now);
        if (s_zone.active) et_link_tick(&s_zone.link, now);
        side_events(&s_tun, &lookup, &fresh);
        side_events(&s_zone, &lookup, &fresh);
        xSemaphoreGive(s_lock);
        if (s_on_event) {
            if (lookup) s_on_event(LT_EV_LOOKUP);
            if (fresh)  s_on_event(LT_EV_NEW_NODE);
        }

        // The zone link's frames out, outside s_lock.
        while (xQueueReceive(s_zout, &z, 0) == pdTRUE) {
            size_t jl = et_zone_send_json(json, sizeof json, z.f, z.n);
            if (!jl || !s_ws || !s_ws_up ||
                esp_websocket_client_send_text(s_ws, json, (int)jl, pdMS_TO_TICKS(200)) < 0)
                s_zone_out_drops++;
        }
    }
}

// ------------------------------------------------------------- settings

static void load_settings(int *tunnel, char *zone, size_t cap)
{
    *tunnel = 1;
    zone[0] = '\0';
    nvs_handle_t h;
    if (nvs_open("et", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t t;
    if (nvs_get_u8(h, "tunnel", &t) == ESP_OK) *tunnel = t != 0;
    size_t l = cap;
    if (nvs_get_str(h, "zone", zone, &l) != ESP_OK || !et_zone_name_ok(zone)) zone[0] = '\0';
    nvs_close(h);
}

esp_err_t ethertalk_apply(int tunnel, const char *zone)
{
    if (zone && zone[0] && !et_zone_name_ok(zone)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open("et", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (tunnel >= 0) nvs_set_u8(h, "tunnel", tunnel ? 1 : 0);
    if (zone) nvs_set_str(h, "zone", zone);
    err = nvs_commit(h);
    nvs_close(h);
    if (!s_running) return err;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (tunnel >= 0 && (tunnel != 0) != s_tunnel_on) {
        s_tunnel_on = tunnel != 0;
        s_want_tunnel = s_tunnel_on;
    }
    if (zone && strcmp(zone, s_zone_name)) {
        strncpy(s_zone_name, zone, ET_ZONE_MAX);
        s_zone_name[ET_ZONE_MAX] = '\0';
        s_want_zone = 1;
    }
    xSemaphoreGive(s_lock);
    return err;
}

void ethertalk_settings(int *tunnel, char *zone, size_t cap)
{
    if (!s_running) {
        load_settings(tunnel, zone, cap);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *tunnel = s_tunnel_on;
    snprintf(zone, cap, "%s", s_zone_name);
    xSemaphoreGive(s_lock);
}

// ------------------------------------------------------------- start

esp_err_t ethertalk_start(const ethertalk_cfg_t *cfg)
{
    if (s_running) return ESP_OK;
    esp_err_t err = at_printer_init(&cfg->printer);
    if (err != ESP_OK) return err;
    s_lock = xSemaphoreCreateMutex();
    s_ring_lock = xSemaphoreCreateMutex();
    s_zin = xQueueCreateWithCaps(ZQ_DEPTH, sizeof(zframe_t), MALLOC_CAP_SPIRAM);
    s_zout = xQueueCreateWithCaps(ZQ_DEPTH, sizeof(zframe_t), MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_ring_lock || !s_zin || !s_zout) return ESP_ERR_NO_MEM;

    strncpy(s_obj, cfg->object, sizeof s_obj - 1);
    strncpy(s_type, cfg->type, sizeof s_type - 1);
    s_on_event = cfg->on_event;
    load_settings(&s_tunnel_on, s_zone_name, sizeof s_zone_name);

    // Field by field: a compound literal would put two links (~7 KB) on this stack.
    s_tun.owner = (at_owner_t){ AT_LINK_TUNNEL, side_abort, tun_note, &s_tun };
    s_tun.name = "tunnel";
    s_tun.nvs_net = "tnet";
    s_tun.nvs_node = "tnode";
    s_tun.status = cfg->tunnel_status;
    s_zone.owner = (at_owner_t){ AT_LINK_ZONE, side_abort, zone_note, &s_zone };
    s_zone.name = "zone";
    s_zone.nvs_net = "znet";
    s_zone.nvs_node = "znode";
    s_zone.status = cfg->zone_status;
    uint8_t sta[ETH_ALEN], mac[ETH_ALEN];
    esp_read_mac(sta, ESP_MAC_WIFI_STA);
    et_zone_mac(sta, s_zone_mac);
    s_ip = cfg->ip;
    et_tunnel_mac(s_ip, mac);
    side_init(&s_tun, mac);
    side_init(&s_zone, s_zone_mac);

    if (s_tunnel_on) {
        tunnel_open();
        s_tun.active = s_sock >= 0 && s_ip;
        if (s_tun.active) note("tunnel", "listening for Basilisk II");
    }
    s_running = 1;
    if (s_zone_name[0]) zone_start(s_zone_name);
    if (xTaskCreatePinnedToCore(et_task, "ethertalk", 8192, NULL, 4, NULL, 0) != pdPASS) {
        s_running = 0;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "EtherTalk as %s:%s@* (tunnel %s, zone %s)", cfg->object, cfg->type,
             s_tunnel_on ? "on" : "off", s_zone_name[0] ? "set" : "off");
    return ESP_OK;
}

int ethertalk_running(void) { return s_running; }

void ethertalk_ip_changed(uint32_t ip)
{
    if (!s_running) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_new_ip = ip;
    s_ip_changed = 1;
    xSemaphoreGive(s_lock);
}

void ethertalk_net_down(void)
{
    if (!s_running) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_net_down = 1;
    xSemaphoreGive(s_lock);
}

void ethertalk_set_status(uint16_t tunnel_status, uint16_t zone_status)
{
    if (!s_running) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_tun.status = tunnel_status;
    s_zone.status = zone_status;
    lt_node_set_status(&s_tun.link.node, tunnel_status);
    lt_node_set_status(&s_zone.link.node, zone_status);
    xSemaphoreGive(s_lock);
}

// ------------------------------------------------------------- /api/et

static int side_json(char *p, size_t cap, const et_side_t *s, const char *state)
{
    const et_link_t *l = &s->link;
    const lt_node_t *n = &l->node;
    char addr[16] = "";
    if (n->phase == LT_ONLINE) snprintf(addr, sizeof addr, "%u.%u", n->net, n->node);
    int k = snprintf(p, cap,
        "{\"state\":\"%s\",\"address\":\"%s\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
        "\"frames_in\":%lu,\"frames_out\":%lu,\"bad\":%lu,\"own\":%lu,\"aarp_in\":%lu,"
        "\"aarp_out\":%lu,\"unresolved\":%lu,\"lookups\":%lu,\"replies\":%lu,\"opens\":%lu,"
        "\"busy\":%lu,\"pap\":{\"open\":%s,\"conn\":%u,\"bytes\":%lu},\"heard\":[",
        state, addr, l->mac[0], l->mac[1], l->mac[2], l->mac[3], l->mac[4], l->mac[5],
        (unsigned long)l->st.rx, (unsigned long)l->st.tx, (unsigned long)l->st.bad,
        (unsigned long)l->st.own, (unsigned long)l->st.aarp_in, (unsigned long)l->st.aarp_out,
        (unsigned long)l->st.unresolved, (unsigned long)n->st.lookups,
        (unsigned long)n->st.replies, (unsigned long)n->st.opens, (unsigned long)n->st.busy,
        n->pap.open ? "true" : "false", n->pap.connid, (unsigned long)n->pap.bytes);
    for (int id = 1, first = 1; id < 255 && k > 0 && (size_t)k < cap; id++) {
        if (!(n->heard[id / 8] & (1u << (id % 8)))) continue;
        k += snprintf(p + k, cap - k, "%s%d", first ? "" : ",", id);
        first = 0;
    }
    if (k > 0 && (size_t)k < cap) k += snprintf(p + k, cap - k, "]}");
    return k;
}

void ethertalk_json(char *buf, size_t cap)
{
    if (!s_running) {
        snprintf(buf, cap, "{\"on\":false}");
        return;
    }
    char *p = buf, *end = buf + cap - 8;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *ts = !s_tunnel_on ? "off" : s_sock < 0 ? "no socket" : !s_ip ? "no address"
                   : s_tun.link.node.phase == LT_ONLINE ? "online" : "acquiring";
    const char *zs = !s_zone_name[0] ? "off" : !s_ws_up ? "connecting"
                   : s_zone.link.node.phase == LT_ONLINE ? "online" : "acquiring";
    p += snprintf(p, end - p, "{\"on\":true,\"tunnel\":");
    if (p < end) p += side_json(p, end - p, &s_tun, ts);
    if (p < end) p += snprintf(p, end - p, ",\"zone\":");
    if (p < end) p += side_json(p, end - p, &s_zone, zs);
    xSemaphoreGive(s_lock);
    if (p < end)
        p += snprintf(p, end - p,
            ",\"tunnel_drops\":%lu,\"relay\":{\"connected\":%s,\"in_drops\":%lu,\"out_drops\":%lu,"
            "\"bad_messages\":%lu,\"pongs\":%lu,\"last_error\":\"%s\"},"
            "\"internal_ram\":{\"free\":%u,\"largest\":%u,\"lowest\":%u},",
            (unsigned long)s_tun_drops, s_ws_up ? "true" : "false",
            (unsigned long)s_zone_in_drops, (unsigned long)s_zone_out_drops,
            (unsigned long)s_zone_bad, (unsigned long)s_zone_pongs, s_zone_err,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    if (p < end) {
        at_printer_json(p, end - p);
        p += strlen(p);
    }
    if (p < end) p += snprintf(p, end - p, ",\"events\":[");
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    for (unsigned i = 0; i < s_ev_n && p < end; i++) {
        unsigned k = (s_ev_head + EV_LINES - s_ev_n + i) % EV_LINES;
        if (i && p < end) *p++ = ',';
        if (p < end) *p++ = '"';
        for (const char *c = s_ev[k]; *c && p < end - 2; c++) {
            if (*c == '"' || *c == '\\') *p++ = '\\';
            *p++ = *c;
        }
        if (p < end) *p++ = '"';
    }
    xSemaphoreGive(s_ring_lock);
    snprintf(p, buf + cap - p, "]}");
}
