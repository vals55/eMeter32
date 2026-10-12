// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "stats_push.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "settings.h"
#include "wifi_mgr.h"
#include "pzem004t.h"
#include "pulse_counter.h"
#include "ntp_sync.h"

static const char *TAG = "stats";

#define STATS_TIMEOUT_MS  10000
#define STATS_RETRY_MS    15000
#define STATS_FIRST_MS    10000

static char s_body[640];

static int build_json(char *out, size_t n) {
    const settings_t *c = settings_get();
    pzem_data_t p;
    pzem_get_last(&p);
    pulse_counter_data_t e1 = { 0 }, e2 = { 0 };
    bool e1ok = pulse_counter_get(PULSE_CH_E1, &e1) == ESP_OK;
    bool e2ok = pulse_counter_get(PULSE_CH_E2, &e2) == ESP_OK;
    time_t ts = ntp_sync_is_synced() ? time(NULL) : 0;

    int o = snprintf(out, n,
        "{\"device\":\"%s\",\"fw\":\"%s\",\"ts\":%lld,\"uptime_s\":%lld,\"rssi\":%d,\"pzem\":",
        c->hostname, FIRMWARE_VERSION, (long long)ts,
        (long long)(esp_timer_get_time() / 1000000), wifi_mgr_rssi());
    if (p.valid) {
        o += snprintf(out + o, n - o,
            "{\"valid\":true,\"voltage\":%.1f,\"current\":%.3f,\"power\":%.1f,"
            "\"energy_kwh\":%.2f,\"frequency\":%.1f,\"pf\":%.2f,\"errors\":%lu}",
            p.voltage, p.current, p.power, p.energy_kwh + c->energy_offset, p.frequency, p.power_factor,
            (unsigned long)p.error_count);
    } else {
        o += snprintf(out + o, n - o, "{\"valid\":false,\"errors\":%lu}", (unsigned long)p.error_count);
    }
    o += snprintf(out + o, n - o, ",\"e1_kwh\":");
    o += e1ok ? snprintf(out + o, n - o, "%.2f", e1.kwh) : snprintf(out + o, n - o, "null");
    o += snprintf(out + o, n - o, ",\"e2_kwh\":");
    o += e2ok ? snprintf(out + o, n - o, "%.2f", e2.kwh) : snprintf(out + o, n - o, "null");
    o += snprintf(out + o, n - o, "}");
    return o >= (int)n ? -1 : o;
}

static esp_err_t stats_send_once(void) {
    const settings_t *c = settings_get();
    if (c->stats_uri[0] == '\0') return ESP_ERR_INVALID_STATE;

    int len = build_json(s_body, sizeof(s_body));
    if (len < 0) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url = c->stats_uri,
        .method = HTTP_METHOD_POST,
        .timeout_ms = STATS_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* нужен только для https:// */
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_post_field(h, s_body, len);

    esp_err_t e = esp_http_client_perform(h);
    if (e == ESP_OK) {
        int code = esp_http_client_get_status_code(h);
        if (code < 200 || code >= 300) {
            ESP_LOGW(TAG, "server answered HTTP %d", code);
            e = ESP_FAIL;
        } else {
            ESP_LOGI(TAG, "sent %d bytes, HTTP %d", len, code);
        }
    } else {
        ESP_LOGW(TAG, "post failed: %s", esp_err_to_name(e));
    }
    esp_http_client_cleanup(h);
    return e;
}

static void stats_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(STATS_FIRST_MS));
    for (;;) {
        uint32_t wait_ms;
        if (!wifi_mgr_is_connected()) {
            wait_ms = STATS_RETRY_MS;
        } else {
            wait_ms = stats_send_once() == ESP_OK ? settings_get()->stats_period_s * 1000UL
                                                  : STATS_RETRY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

esp_err_t stats_push_start(void) {
    const settings_t *c = settings_get();
    if (c->stats_uri[0] == '\0') {
        ESP_LOGI(TAG, "stats_uri is empty, statistics push disabled");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "push to %s every %lu s", c->stats_uri, (unsigned long)c->stats_period_s);
    return xTaskCreate(stats_task, "stats", 6144, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
 
 
