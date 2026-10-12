// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "display_ui.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board_config.h"
#include "oled.h"
#include "wifi_mgr.h"
#include "pzem004t.h"
#include "settings.h"

static const char *TAG = "display";

#define REFRESH_MS 1000

static int rssi_level(int rssi) {
    if (rssi == 0) return 0;
    return rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : rssi > -88 ? 1 : 0;
}

/* Полоски уровня сигнала в правой части желтой зоны */
static void draw_bars(int level) {
    for (int i = 0; i < 4; i++) {
        int h = 2 + 2 * i;
        int x = 104 + i * 5;
        if (i < level) {
            oled_fill_rect(x, 8 - h, 3, h, true);
        } else {
            oled_fill_rect(x, 7, 3, 1, true);
        }
    }
}

static void fmt_age(char *out, size_t n, int64_t ms) {
    int64_t s = ms / 1000;
    if (s < 60) {
        snprintf(out, n, "%ds", (int)s);
    } else if (s < 3600) {
        snprintf(out, n, "%dm", (int)(s / 60));
    } else {
        snprintf(out, n, "%dh", (int)(s / 3600));
    }
}

/* Желтая зона (y 0..15): статус Wi-Fi */
static void draw_wifi_status(void) {
    char a[32];
    char b[32];
    const settings_t *c = settings_get();
    switch (wifi_mgr_state()) {
        case WIFI_MGR_CONNECTED: {
            snprintf(a, sizeof(a), "WiFi OK %ddBm", wifi_mgr_rssi());
            if (wifi_mgr_ip(b, sizeof(b)) != ESP_OK) strcpy(b, "no IP");
            draw_bars(rssi_level(wifi_mgr_rssi()));
            break;
        }
        case WIFI_MGR_CONNECTING:
            strcpy(a, "WiFi connecting...");
            snprintf(b, sizeof(b), "%.20s", c->wifi_ssid);
            break;
        case WIFI_MGR_RETRY_WAIT:
            strcpy(a, "WiFi retry wait");
            snprintf(b, sizeof(b), "%.20s", c->wifi_ssid);
            break;
        default:
            snprintf(a, sizeof(a), "SETUP %.14s", c->hostname);
            strcpy(b, "http://192.168.4.1");
            break;
    }
    oled_text(0, 1, a, 1);
    oled_text(0, 9, b, 1);
}

/* Синяя зона (y 16..63): показания сети */
static void draw_power(void) {
    pzem_data_t d;
    pzem_get_last(&d);
    char l[32];
    if (!d.valid) {
        oled_text(0, 30, d.error_count ? "PZEM: no answer" : "PZEM: waiting...", 1);
        return;
    }
    snprintf(l, sizeof(l), "%5.1fV  %4.1fHz", d.voltage, d.frequency);
    oled_text(0, 19, l, 1);
    snprintf(l, sizeof(l), "%6.3fA  PF %.2f", d.current, d.power_factor);
    oled_text(0, 29, l, 1);
    snprintf(l, sizeof(l), "%.1fW", d.power);
    oled_text(0, 39, l, 2);
    snprintf(l, sizeof(l), "%.3fkWh", d.energy_kwh);
    oled_text(0, 56, l, 1);

    char age[12];
    fmt_age(age, sizeof(age), esp_timer_get_time() / 1000 - d.timestamp_ms);
    if (d.error_count) {
        snprintf(l, sizeof(l), "ERR %s", age);
    } else {
        snprintf(l, sizeof(l), "%s", age);
    }
    oled_text(OLED_W - oled_text_width(l, 1), 56, l, 1);
}

static void ui_task(void *arg) {
    for (;;) {
        oled_clear();
        draw_wifi_status();
        draw_power();
        if (oled_flush() != ESP_OK) {
            ESP_LOGW(TAG, "flush failed");
        }
        vTaskDelay(pdMS_TO_TICKS(REFRESH_MS));
    }
}

esp_err_t display_ui_start(void) {
    esp_err_t e = oled_init(PIN_I2C_SDA, PIN_I2C_SCL, OLED_I2C_ADDR);
    if (e != ESP_OK) return e;
    return xTaskCreate(ui_task, "display", 4096, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

 
 
