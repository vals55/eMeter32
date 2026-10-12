// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define WIFI_MAX_ATTEMPTS       10
#define WIFI_BACKOFF_BASE_S     2
#define WIFI_BACKOFF_MAX_S      60
/* 0: AP только если ни разу не подключились; после потери связи пробуем вечно.
   1: AP и после 10 неудач при потере связи. */
#define WIFI_AP_AFTER_LINK_LOSS 0

#define WIFI_TX_POWER_STA   60   /* рабочий режим: 15 дБм (для проверки; было 34 = 8,5 дБм) */
#define WIFI_TX_POWER_AP    34   /* портал: 8,5 дБм */

typedef enum {
    WIFI_MGR_CONNECTING,
    WIFI_MGR_CONNECTED,
    WIFI_MGR_RETRY_WAIT,
    WIFI_MGR_AP_REQUIRED,    /* пора запускать captive portal */
} wifi_mgr_state_t;

typedef void (*wifi_mgr_cb_t)(wifi_mgr_state_t state, void *arg);

esp_err_t wifi_mgr_start(wifi_mgr_cb_t cb, void *arg);
wifi_mgr_state_t wifi_mgr_state(void);
bool wifi_mgr_is_connected(void);
int  wifi_mgr_rssi(void);                        /* 0, если нет связи */
esp_err_t wifi_mgr_ip(char *buf, size_t len);    /* строка IP */
void wifi_mgr_suspend(void);   /* остановить подключение, перейти в AP_REQUIRED */

 
 
