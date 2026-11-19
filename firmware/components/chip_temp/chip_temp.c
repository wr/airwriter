// SPDX-License-Identifier: GPL-3.0-or-later
#include "chip_temp.h"

#include <limits.h>
#include <stdio.h>
#include "driver/temperature_sensor.h"
#include "esp_log.h"

static const char *TAG = "chip_temp";

#define NONE INT_MIN

static temperature_sensor_handle_t s_sensor;
// Tenths of a degree, whole words, so the web server's task reads them without a lock.
static volatile int s_now = NONE, s_max = NONE;

static const char *tenths(char *buf, size_t cap, int t)
{
    int a = t < 0 ? -t : t;
    if (t == NONE) snprintf(buf, cap, "null");
    else snprintf(buf, cap, "%s%d.%d", t < 0 ? "-" : "", a / 10, a % 10);
    return buf;
}

void chip_temp_start(void)
{
    // -10 to 80 °C is the sensor's most exact range (under 1 °C off). A reading past its
    // end moves the driver to the next range by itself.
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    temperature_sensor_handle_t h = NULL;
    esp_err_t err = temperature_sensor_install(&cfg, &h);
    if (err == ESP_OK && (err = temperature_sensor_enable(h)) != ESP_OK) temperature_sensor_uninstall(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no temperature sensor: %s", esp_err_to_name(err));
        return;
    }
    s_sensor = h;
    chip_temp_sample();
    char now[12];
    ESP_LOGI(TAG, "the chip is at %s C", tenths(now, sizeof now, s_now));
}

void chip_temp_sample(void)
{
    float c;
    if (!s_sensor) return;
    if (temperature_sensor_get_celsius(s_sensor, &c) != ESP_OK) { s_now = NONE; return; }
    int t = (int)(c * 10.0f + (c < 0 ? -0.5f : 0.5f));
    s_now = t;
    if (s_max == NONE || t > s_max) s_max = t;
}

size_t chip_temp_json(char *buf, size_t cap)
{
    char now[12], max[12];
    int n = snprintf(buf, cap, ",\"temp\":%s,\"temp_max\":%s",
                     tenths(now, sizeof now, s_now), tenths(max, sizeof max, s_max));
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap ? cap - 1 : 0;
}
