// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "esp_log.h"
#include "board_config.h"
#include "settings.h"
#include "wifi_mgr.h"
#include "captive_portal.h"
#include "user_button.h"
#include "display_ui.h"
#include "pzem004t.h"
#include "ntp_sync.h"
#include "esp_timer.h"
#include "stats_push.h"
#include "pulse_counter.h"
#include "mqtt_mgr.h"
#include "ha_discovery.h"
#include "ota_update.h"

static const char *TAG = "main";

static void on_wifi(wifi_mgr_state_t st, void *arg) {
    ESP_LOGI(TAG, "wifi state = %d", st);
    switch (st) {
    case WIFI_MGR_CONNECTED:
        ota_update_mark_valid();        /* новая прошивка дошла до сети - откат не нужен */
        ntp_sync_on_wifi_connected();   /* сверка времени после подключения */
        mqtt_mgr_start();               /* идемпотентно; без mqtt_host ничего не делает */
        break;
    case WIFI_MGR_AP_REQUIRED:
        ota_update_mark_valid();        /* Wi-Fi работает, прошивка жива (устройство ещё не настроено) */
        ota_update_stop();              /* порт 80 нужен порталу */
        captive_portal_start();
        break;
    default:
        break;
    }
}

/* Ждать retained-значение с MQTT, затем взять начальные значения из настроек */
#define PULSE_RESTORE_WAIT_US (60ULL * 1000000ULL)

static void pulses_restore_initial(void *arg) {
    const settings_t *c = settings_get();
    /* для уже восстановленных каналов вернёт ESP_ERR_INVALID_STATE - это нормально */
    pulse_counter_restore(PULSE_CH_E1, (uint32_t)((double)c->initial_E1 * c->coeff + 0.5));
    pulse_counter_restore(PULSE_CH_E2, (uint32_t)((double)c->initial_E2 * c->coeff + 0.5));
}

static void pulses_start(void) {
    const settings_t *c = settings_get();
    pulse_counter_cfg_t pc = {
        .gpio = { PIN_PULSE_E1, PIN_PULSE_E2 },
        .coeff = c->coeff,
        .glitch_ns = PULSE_GLITCH_NS,
        .count_falling = PULSE_COUNT_FALLING,
        .pull_up = PULSE_PULL_UP,
    };
    if (pulse_counter_start(&pc) != ESP_OK) {
        ESP_LOGW(TAG, "pulse counter start failed");
        return;
    }
    if (c->mqtt_host[0] == '\0') {
        pulses_restore_initial(NULL);          /* MQTT выключен - восстанавливать не откуда */
        return;
    }
    esp_timer_handle_t tm;
    const esp_timer_create_args_t ta = { .callback = pulses_restore_initial, .name = "pulse_fb" };
    if (esp_timer_create(&ta, &tm) == ESP_OK) esp_timer_start_once(tm, PULSE_RESTORE_WAIT_US);
}

static void on_portal_button(void) {
    wifi_mgr_suspend();   /* остановить попытки подключения и перейти в AP_REQUIRED */
}

void app_main(void) {
    ESP_ERROR_CHECK(settings_init());
    ntp_sync_init();      /* часовой пояс из настроек, сеть не нужна */

    /* PZEM раньше дисплея: задача экрана сразу читает pzem_get_last(), мьютекс должен уже существовать */
    if (pzem_start(PZEM_UART, PIN_PZEM_TX, PIN_PZEM_RX) != ESP_OK) {
        ESP_LOGW(TAG, "pzem start failed");
    }
    if (display_ui_start() != ESP_OK) {
        ESP_LOGW(TAG, "display not available, continuing without it");
    }
    pulses_start();
    ota_update_init();
    ha_discovery_start();   /* до Wi-Fi: регистрирует обработчики MQTT и счётчика */
    ESP_ERROR_CHECK(wifi_mgr_start(on_wifi, NULL));
    if (stats_push_start() != ESP_OK) {
        ESP_LOGW(TAG, "stats push start failed");
    }
    ESP_ERROR_CHECK(user_button_start(PIN_PORTAL_BUTTON, PORTAL_BUTTON_HOLD_MS, on_portal_button));
}

 
 
 
 
