// SPDX-License-Identifier: GPL-3.0-or-later
// Host stand-in for ESP-IDF's esp_err.h, so firmware headers that name esp_err_t build on the host.
#pragma once
typedef int esp_err_t;
#define ESP_OK    0
#define ESP_FAIL  (-1)
