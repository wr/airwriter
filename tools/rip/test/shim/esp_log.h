// SPDX-License-Identifier: GPL-3.0-or-later
// Host stand-in for ESP-IDF's esp_log.h: logging says nothing, but the tag is still used.
#pragma once
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
