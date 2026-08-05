// SPDX-License-Identifier: GPL-3.0-or-later
#include "net_provision.h"

#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "provision";

#define AP_IP        "192.168.4.1"
#define DNS_PORT     53
#define DNS_MAX      512

static int s_dns_fd = -1;
static int s_running;
static esp_netif_t *s_ap_netif;

// A DNS header is 12 bytes: id, flags, then four 16-bit counts.
typedef struct {
    uint16_t id, flags, qd, an, ns, ar;
} dns_hdr_t;

// Answers every A query with our own address, whatever was asked for. That is
// what makes the phone's connectivity check fail in the particular way that
// makes it open a browser at the setup page.
static void dns_task(void *arg)
{
    (void)arg;
    uint8_t buf[DNS_MAX];
    struct sockaddr_in peer;
    socklen_t plen;

    while (s_running) {
        plen = sizeof peer;
        int n = recvfrom(s_dns_fd, buf, sizeof buf, 0, (struct sockaddr *)&peer, &plen);
        if (n < (int)sizeof(dns_hdr_t) + 5) continue;

        // Walk the question's labels to find where it ends: length-prefixed
        // segments terminated by a zero byte, then QTYPE and QCLASS.
        int q = sizeof(dns_hdr_t);
        while (q < n && buf[q]) {
            q += buf[q] + 1;
            if (q > n - 1) break;
        }
        q += 1 + 4;
        if (q > n || (size_t)(q + 16) > sizeof buf) continue;

        dns_hdr_t *h = (dns_hdr_t *)buf;
        h->flags = htons(0x8180);      // response, recursion available, no error
        h->an    = h->qd;              // one answer per question asked
        h->ns    = 0;
        h->ar    = 0;

        uint8_t *a = buf + q;
        *a++ = 0xC0; *a++ = 0x0C;                      // pointer back to the name
        *a++ = 0x00; *a++ = 0x01;                      // type A
        *a++ = 0x00; *a++ = 0x01;                      // class IN
        *a++ = 0x00; *a++ = 0x00; *a++ = 0x00; *a++ = 0x0A;   // ttl 10s
        *a++ = 0x00; *a++ = 0x04;                      // 4 bytes of address

        uint32_t ip = inet_addr(AP_IP);
        memcpy(a, &ip, 4);
        a += 4;

        sendto(s_dns_fd, buf, a - buf, 0, (struct sockaddr *)&peer, plen);
    }

    close(s_dns_fd);
    s_dns_fd = -1;
    vTaskDelete(NULL);
}

static esp_err_t start_dns(void)
{
    s_dns_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_fd < 0) return ESP_FAIL;

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_dns_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        ESP_LOGE(TAG, "dns bind: errno %d", errno);
        close(s_dns_fd);
        s_dns_fd = -1;
        return ESP_FAIL;
    }

    s_running = 1;
    xTaskCreate(dns_task, "captivedns", 3072, NULL, 4, NULL);
    return ESP_OK;
}

esp_err_t net_provision_start(const char *ap_name)
{
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t wc = {0};
    strncpy((char *)wc.ap.ssid, ap_name, sizeof wc.ap.ssid - 1);
    wc.ap.ssid_len       = strlen((char *)wc.ap.ssid);
    wc.ap.channel        = 1;
    wc.ap.max_connection = 4;
    wc.ap.authmode       = WIFI_AUTH_OPEN;

    // The station half has to be told to stop before the access point comes
    // up. Left half-alive it keeps retrying and rescanning in the background,
    // and in APSTA mode the access point follows the station's channel -- so
    // the beacon wanders off the channel it advertised and the network quietly
    // vanishes from every phone in the room, minutes after it appeared.
    esp_wifi_disconnect();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));   // STA stays up to scan
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Nothing here may doze: the beacon has to be continuous for the network
    // to stay listed, and this device is mains powered.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    esp_err_t err = start_dns();
    if (err != ESP_OK) return err;

    ESP_LOGW(TAG, "no network yet: join \"%s\" and the setup page opens", ap_name);
    ESP_LOGW(TAG, "or browse to http://%s/", AP_IP);
    return ESP_OK;
}

void net_provision_stop(void)
{
    s_running = 0;
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
}

int net_provision_done(void) { return 0; }
