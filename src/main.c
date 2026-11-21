// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "esp_log.h"
#include "esp_system.h"
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
    /* MQTT включен: счет идет в RAM, base придет с retained-значением. Запасной вариант
     * (начальные E1/E2 из настроек) включается в ha_discovery только после подключения к брокеру. */
}

static void on_portal_button(void) {
    wifi_mgr_suspend();   /* остановить попытки подключения и перейти в AP_REQUIRED */
}

void app_main(void) {
    ESP_LOGW(TAG, "reset reason: %d", (int)esp_reset_reason());   /* 1=poweron 3=sw 4=panic 5=int_wdt 6=task_wdt 15=brownout */
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

 
 
 
 
 
