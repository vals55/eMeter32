// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "settings.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_rom_crc.h"
#include "esp_log.h"
#include "esp_mac.h"

static const char *TAG = "settings";
#define NS  "cfg"
#define KEY "main"

/* true, если строка-массив содержит завершающий ноль */
#define STR_OK(a) (memchr((a), 0, sizeof(a)) != NULL)

static settings_t s_cfg;
static bool s_stored;       /* s_cfg совпадает с тем, что лежит в NVS (загружено или записано) */

static uint32_t calc_crc(const settings_t *s) {
    return esp_rom_crc32_le(0, (const uint8_t *)s, offsetof(settings_t, crc));
}

void settings_fill_defaults(settings_t *s) {
    memset(s, 0, sizeof(*s));   /* обнуляем и padding, чтобы CRC был стабильным */
    s->version = SETTINGS_VERSION;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s->hostname, sizeof(s->hostname), HOST_NAME, mac[4], mac[5]);
    s->mqtt_port = MQTT_DEFAULT_PORT;
    strlcpy(s->mqtt_topic, MQTT_DEFAULT_TOPIC_PREFIX, sizeof(s->mqtt_topic));
    strlcpy(s->mqtt_discovery_topic, DISCOVERY_TOPIC, sizeof(s->mqtt_discovery_topic));
    s->mqtt_auto_discovery = MQTT_AUTO_DISCOVERY;
    strlcpy(s->ntp_server, DEFAULT_NTP_SERVER, sizeof(s->ntp_server));
    s->tz = DEFAULT_TZ;
    s->coeff = DEFAULT_COEFF;
    s->pzem_period_s = 60;
    s->stats_uri[0] = '\0';
    s->stats_period_s = DEFAULT_STATS_PERIOD_S;
    s->energy_offset = 0.0f;
}

bool settings_validate(const settings_t *s) {
    if (s->version != SETTINGS_VERSION) return false;
    if (!STR_OK(s->wifi_ssid)) return false;
    if (!STR_OK(s->wifi_pass)) return false;
    if (!STR_OK(s->hostname)) return false;
    if (!STR_OK(s->mqtt_host)) return false;
    if (!STR_OK(s->mqtt_user)) return false;
    if (!STR_OK(s->mqtt_pass)) return false;
    if (!STR_OK(s->mqtt_topic)) return false;
    if (!STR_OK(s->mqtt_discovery_topic)) return false;
    if (!STR_OK(s->ntp_server)) return false;
    if (s->mqtt_port == 0) return false;
    if (s->mqtt_auto_discovery > 1) return false;
    if (s->tz < -12 || s->tz > 14) return false;
    if (!isfinite(s->initial_E1) || s->initial_E1 < 0) return false;
    if (!isfinite(s->initial_E2) || s->initial_E2 < 0) return false;
    if (s->coeff == 0) return false;
    if (s->pzem_period_s < 60 || s->pzem_period_s > 3600) return false;
    if (!STR_OK(s->stats_uri)) return false;
    if (s->stats_period_s < STATS_PERIOD_MIN_S || s->stats_period_s > STATS_PERIOD_MAX_S) return false;
    if (!isfinite(s->energy_offset)) return false;
    return true;
}

static esp_err_t write_nvs(const settings_t *s) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, KEY, s, sizeof(*s));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t settings_save(const settings_t *in) {
    settings_t tmp = *in;
    tmp.version = SETTINGS_VERSION;
    tmp.crc = calc_crc(&tmp);
    if (!settings_validate(&tmp)) return ESP_ERR_INVALID_ARG;
    /* значение не изменилось - не расходуем ресурс flash */
    if (s_stored && memcmp(&tmp, &s_cfg, sizeof(tmp)) == 0) return ESP_OK;
    esp_err_t e = write_nvs(&tmp);
    if (e == ESP_OK) {
        s_cfg = tmp;
        s_stored = true;
    }
    return e;
}

esp_err_t settings_reset_defaults(void) {
    settings_t d;
    settings_fill_defaults(&d);
    return settings_save(&d);
}

esp_err_t settings_init(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    if (e != ESP_OK) return e;

    settings_t tmp;
    size_t len = sizeof(tmp);
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, KEY, &tmp, &len) == ESP_OK && len == sizeof(tmp)
            && tmp.crc == calc_crc(&tmp) && settings_validate(&tmp)) {
            ok = true;
        }
        nvs_close(h);
    }
    if (ok) {
        s_cfg = tmp;
        s_stored = true;
        ESP_LOGI(TAG, "loaded from NVS (ssid='%s')", s_cfg.wifi_ssid);
    } else {
        ESP_LOGW(TAG, "no valid settings, using defaults");
        settings_fill_defaults(&s_cfg);
        s_cfg.crc = calc_crc(&s_cfg);
    }
    return ESP_OK;
}

const settings_t *settings_get(void) {
    return &s_cfg;
}

 
 
