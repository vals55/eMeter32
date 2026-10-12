// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "wifi_mgr.h"
#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "settings.h"

static const char *TAG = "wifi_mgr";

static wifi_mgr_cb_t s_cb;
static void *s_cb_arg;
static volatile wifi_mgr_state_t s_state = WIFI_MGR_CONNECTING;
static esp_netif_t *s_netif;
static esp_timer_handle_t s_timer;
static uint32_t s_attempt;
static bool s_ever_connected;
static bool s_gave_up;

static void set_state(wifi_mgr_state_t st) {
    s_state = st;
    if (s_cb) s_cb(st, s_cb_arg);
}

static void retry_timer_cb(void *arg) {
    if (s_gave_up) return;
    set_state(WIFI_MGR_CONNECTING);
    esp_wifi_connect();
}

static uint32_t backoff_s(uint32_t attempt) {
    uint32_t shift = attempt > 0 ? attempt - 1 : 0;
    if (shift > 5) shift = 5;
    uint32_t d = WIFI_BACKOFF_BASE_S << shift;
    return d > WIFI_BACKOFF_MAX_S ? WIFI_BACKOFF_MAX_S : d;
}

/* Расшифровка частых кодов отключения (wifi_err_reason_t) */
static const char *reason_str(uint8_t r) {
    switch (r) {
        case 2:   return "auth expire";
        case 15:  return "4way handshake timeout (неверный пароль?)";
        case 200: return "beacon timeout (слабый сигнал)";
        case 201: return "no AP found (сеть не найдена)";
        case 202: return "auth fail";
        case 203: return "assoc fail";
        case 204: return "handshake timeout (неверный пароль?)";
        case 205: return "connection fail";
        case 210: return "no AP with compatible security";
        case 211: return "no AP in authmode threshold";
        case 212: return "no AP in RSSI threshold";
        default:  return "other";
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_gave_up) return;
        s_attempt = 1;
        set_state(WIFI_MGR_CONNECTING);
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_gave_up) return;
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "disconnected: reason=%d (%s), rssi=%d, attempt %u",
                 d->reason, reason_str(d->reason), d->rssi, (unsigned)s_attempt);

        bool fallback_allowed = !s_ever_connected || WIFI_AP_AFTER_LINK_LOSS;
        if (fallback_allowed && s_attempt >= WIFI_MAX_ATTEMPTS) {
            s_gave_up = true;
            ESP_LOGE(TAG, "%d attempts failed -> AP required", WIFI_MAX_ATTEMPTS);
            set_state(WIFI_MGR_AP_REQUIRED);
            return;
        }
        uint32_t wait = backoff_s(s_attempt);
        s_attempt++;
        ESP_LOGI(TAG, "next attempt in %us", (unsigned)wait);
        set_state(WIFI_MGR_RETRY_WAIT);
        esp_timer_stop(s_timer);
        esp_timer_start_once(s_timer, (uint64_t)wait * 1000000ULL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        esp_timer_stop(s_timer);
        s_attempt = 1;   /* следующий обрыв начнет отсчет заново */
        s_ever_connected = true;
        set_state(WIFI_MGR_CONNECTED);
    }
}

esp_err_t wifi_mgr_start(wifi_mgr_cb_t cb, void *arg) {
    s_cb = cb;
    s_cb_arg = arg;
    const settings_t *cfg = settings_get();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, cfg->hostname);

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&icfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));  /* не хранить режим/AP во флеше */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));        /* рабочий режим - только клиент */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    const esp_timer_create_args_t targs = { .callback = retry_timer_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_timer));

    if (cfg->wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "SSID not configured -> AP required");
        s_gave_up = true;
        set_state(WIFI_MGR_AP_REQUIRED);
        return ESP_OK;
    }

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, cfg->wifi_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, cfg->wifi_pass, sizeof(wc.sta.password));
    if (cfg->wifi_bssid_set) {
        wc.sta.bssid_set = true;
        memcpy(wc.sta.bssid, cfg->wifi_bssid, 6);
    }
    wc.sta.threshold.authmode = cfg->wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    /* PMF: без него роутеры в режиме WPA2/WPA3 могут отклонять подключение */
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
    /* Искать на всех каналах и выбирать самую сильную точку с этим SSID */
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    if (cfg->wifi_bssid_set) {
        ESP_LOGI(TAG, "connect: ssid='%s' pass_len=%d bssid=%02X:%02X:%02X:%02X:%02X:%02X",
                 cfg->wifi_ssid, (int)strlen(cfg->wifi_pass),
                 cfg->wifi_bssid[0], cfg->wifi_bssid[1], cfg->wifi_bssid[2],
                 cfg->wifi_bssid[3], cfg->wifi_bssid[4], cfg->wifi_bssid[5]);
    } else {
        ESP_LOGI(TAG, "connect: ssid='%s' pass_len=%d bssid=any",
                 cfg->wifi_ssid, (int)strlen(cfg->wifi_pass));
    }

    /* Режим STA уже задан выше, после esp_wifi_init() */
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    esp_err_t e = esp_wifi_start();

    if (e == ESP_OK) {
        esp_err_t pe = esp_wifi_set_max_tx_power(WIFI_TX_POWER_STA);
        if (pe != ESP_OK) {
            ESP_LOGW(TAG, "set_max_tx_power failed: %s", esp_err_to_name(pe));
        }
        int8_t p = 0;
        if (esp_wifi_get_max_tx_power(&p) == ESP_OK) {
            ESP_LOGI(TAG, "tx power: %d (x0.25 dBm)", p);
        }
    }
    return e;
}

wifi_mgr_state_t wifi_mgr_state(void) {
    return s_state;
}

bool wifi_mgr_is_connected(void) {
    return s_state == WIFI_MGR_CONNECTED;
}

int wifi_mgr_rssi(void) {
    wifi_ap_record_t ap;
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

esp_err_t wifi_mgr_ip(char *buf, size_t len) {
    esp_netif_ip_info_t ip;
    esp_err_t e = esp_netif_get_ip_info(s_netif, &ip);
    if (e == ESP_OK) snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    return e;
}

void wifi_mgr_suspend(void) {
    s_gave_up = true;                 /* обработчик событий перестанет перезапускать подключение */
    if (s_timer) esp_timer_stop(s_timer);
    esp_wifi_disconnect();
    set_state(WIFI_MGR_AP_REQUIRED);  /* вызовет captive_portal_start() через callback в main */
}

 
 
