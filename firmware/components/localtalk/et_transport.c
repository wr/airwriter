// SPDX-License-Identifier: GPL-3.0-or-later
// The tunnel's and the relay's rules; see et_transport.h.
#include "et_transport.h"
#include <stdio.h>
#include <string.h>

void et_tunnel_mac(uint32_t ip, uint8_t mac[ETH_ALEN])
{
    mac[0] = 'B';
    mac[1] = '2';
    mac[2] = (uint8_t)(ip >> 24);
    mac[3] = (uint8_t)(ip >> 16);
    mac[4] = (uint8_t)(ip >> 8);
    mac[5] = (uint8_t)ip;
}

int et_tunnel_dest(const uint8_t dst[ETH_ALEN], uint32_t *ip)
{
    if (dst[0] == 'B' && dst[1] == '2') {
        *ip = (uint32_t)dst[2] << 24 | (uint32_t)dst[3] << 16 | (uint32_t)dst[4] << 8 | dst[5];
        return 1;
    }
    if (!memcmp(dst, ET_BCAST, ETH_ALEN) || !memcmp(dst, ET_AT_MCAST, ETH_ALEN)) {
        *ip = 0xFFFFFFFFu;
        return 1;
    }
    return 0;
}

void et_zone_mac(const uint8_t base[ETH_ALEN], uint8_t mac[ETH_ALEN])
{
    memcpy(mac, base, ETH_ALEN);
    mac[0] = 0x0A;
}

int et_zone_name_ok(const char *zone)
{
    if (!zone || !*zone) return 0;
    size_t n = 0;
    for (const char *p = zone; *p; p++, n++) {
        char c = *p;
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_' || c == '.';
        if (!ok || n >= ET_ZONE_MAX) return 0;
    }
    return 1;
}

static void mac_str(char out[18], const uint8_t mac[ETH_ALEN])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

size_t et_zone_init_json(char *out, size_t cap, const uint8_t mac[ETH_ALEN])
{
    char m[18];
    mac_str(m, mac);
    int k = snprintf(out, cap, "{\"type\":\"init\",\"macAddress\":\"%s\"}", m);
    return k > 0 && (size_t)k < cap ? (size_t)k : 0;
}

size_t et_zone_send_json(char *out, size_t cap, const uint8_t *frame, size_t n)
{
    if (n < ETH_ALEN) return 0;
    char d[18];
    if (!memcmp(frame, ET_AT_MCAST, ETH_ALEN)) strcpy(d, "AT");
    else if (!memcmp(frame, ET_BCAST, ETH_ALEN)) strcpy(d, "*");
    else mac_str(d, frame);
    int k = snprintf(out, cap, "{\"type\":\"send\",\"destination\":\"%s\",\"packetArray\":[", d);
    if (k <= 0 || (size_t)k >= cap) return 0;
    size_t at = (size_t)k;
    for (size_t i = 0; i < n; i++) {
        k = snprintf(out + at, cap - at, i ? ",%u" : "%u", frame[i]);
        if (k <= 0 || (size_t)k >= cap - at) return 0;
        at += (size_t)k;
    }
    if (at + 2 >= cap) return 0;
    out[at++] = ']';
    out[at++] = '}';
    out[at] = '\0';
    return at;
}

// ------------------------------------------------------------- parsing
// Just enough JSON for the relay's two keys: find a key at the top level of
// the object by its quoted name, then read a string or an array of bytes.

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

// The value after "key": (whitespace allowed around the colon), or NULL.
static const char *find_key(const char *msg, const char *end, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = msg; p + kl + 2 <= end; p++) {
        if (*p != '"' || memcmp(p + 1, key, kl) || p[1 + kl] != '"') continue;
        const char *q = skip_ws(p + kl + 2, end);
        if (q < end && *q == ':') return skip_ws(q + 1, end);
    }
    return NULL;
}

int et_zone_parse(const char *msg, size_t len, uint8_t *frame, size_t cap, size_t *n)
{
    const char *end = msg + len;
    const char *t = find_key(msg, end, "type");
    if (!t || t >= end || *t != '"') return -1;
    static const char receive[] = "\"receive\"";
    if ((size_t)(end - t) < sizeof receive - 1 || memcmp(t, receive, sizeof receive - 1)) return 0;

    const char *p = find_key(msg, end, "packetArray");
    if (!p || p >= end || *p != '[') return -1;
    p = skip_ws(p + 1, end);
    size_t k = 0;
    if (p < end && *p == ']') {
        *n = 0;
        return 1;
    }
    for (;;) {
        if (p >= end || *p < '0' || *p > '9') return -1;
        unsigned v = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            v = v * 10 + (unsigned)(*p - '0');
            if (v > 255) return -1;
            p++;
        }
        if (k >= cap) return -1;
        frame[k++] = (uint8_t)v;
        p = skip_ws(p, end);
        if (p >= end) return -1;
        if (*p == ']') break;
        if (*p != ',') return -1;
        p = skip_ws(p + 1, end);
    }
    *n = k;
    return 1;
}

int et_zone_pong(const uint8_t *frame, size_t n, const uint8_t mac[ETH_ALEN],
                 uint8_t pong[ET_PONG_LEN])
{
    if (n != 26 || memcmp(frame + 14, "pingPING", 8)) return 0;
    memcpy(pong, frame + 6, ETH_ALEN);
    memcpy(pong + 6, mac, ETH_ALEN);
    pong[12] = 0;
    pong[13] = 13;
    memcpy(pong + 14, "pingPONG!", 9);
    memcpy(pong + 23, frame + 22, 4);
    return 1;
}
