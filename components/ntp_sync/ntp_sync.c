// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ntp_sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "settings.h"

static const char *TAG = "ntp";

static bool s_init;
static volatile bool s_synced;
static volatile time_t s_last;

static void on_sync(struct timeval *tv) {
    s_last = tv->tv_sec;
    s_synced = true;
    struct tm tm;
    char buf[32];
    localtime_r(&tv->tv_sec, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    ESP_LOGI(TAG, "time synchronized: %s", buf);
}

void ntp_sync_init(void) {
    int tz = settings_get()->tz;
    char buf[16];
    /* В POSIX-формате знак смещения обратный: UTC+3 записывается как "UTC-3" */
    snprintf(buf, sizeof(buf), "UTC%+d", -tz);
    setenv("TZ", buf, 1);
    tzset();
    ESP_LOGI(TAG, "timezone set: %s", buf);
}

void ntp_sync_on_wifi_connected(void) {
    const char *srv = settings_get()->ntp_server;
    if (srv[0] == '\0') {
        ESP_LOGW(TAG, "NTP server not configured, skipping");
        return;
    }
    if (!s_init) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(srv);
        cfg.start = false;            /* запускаем явно после подключения */
        cfg.sync_cb = on_sync;
        esp_err_t e = esp_netif_sntp_init(&cfg);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "sntp init failed: %s", esp_err_to_name(e));
            return;
        }
        sntp_set_sync_interval(NTP_SYNC_INTERVAL_MS);
        s_init = true;
    }
    if (s_synced && (time(NULL) - s_last) < NTP_MIN_RESYNC_S) {
        return;                       /* время только что сверено, переподключение не повод для нового запроса */
    }
    ESP_LOGI(TAG, "starting SNTP, server %s", srv);
    esp_netif_sntp_start();
}

bool ntp_sync_is_synced(void) {
    return s_synced;
}

time_t ntp_sync_last_sync(void) {
    return s_last;
}

 
 
