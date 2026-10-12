// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Версия формата структуры. Менять при любом изменении settings_t. */
#define SETTINGS_VERSION 10

#define FIRMWARE_VERSION "0.2"
#define HARDWARE_VERSION "0.1"
#define BRAND_NAME "eMeter32"
/* В имени хоста нельзя использовать точку, поэтому версия прошивки в него не входит */
#define HOST_NAME BRAND_NAME "-%02X%02X"
#define CLASSIC 0
#define LITE 1
#define MODEL CLASSIC
#define MANUFACTURER "Vals home Lab ©"

/* Длины буферов включают завершающий ноль */
#define SSID_LEN 33
#define PASSW_LEN 64
#define MQTT_HOST_LEN 64
#define MQTT_LOGIN_LEN 32
#define MQTT_PASSWORD_LEN 32
#define MQTT_TOPIC_LEN 64
#define NTP_HOST_LEN 64
#define HOSTNAME_LEN 32
#define STATS_URI_LEN 128

#define DEFAULT_STATS_PERIOD_S 300
#define STATS_PERIOD_MIN_S     60
#define STATS_PERIOD_MAX_S     86400

#ifndef DISCOVERY_TOPIC
#define DISCOVERY_TOPIC "homeassistant"
#endif

#ifndef MQTT_AUTO_DISCOVERY
#define MQTT_AUTO_DISCOVERY true
#endif

#define MQTT_DEFAULT_TOPIC_PREFIX BRAND_NAME
#define MQTT_DEFAULT_PORT 1883

#define DEFAULT_NTP_SERVER "ru.pool.ntp.org"
#define DEFAULT_TZ 3          /* часовой пояс, часы от UTC */

#define DEFAULT_COEFF 3200    /* импульсов на кВт*ч */

typedef struct {
    uint8_t  version;
    /* Wi-Fi */
    char     wifi_ssid[SSID_LEN];
    char     wifi_pass[PASSW_LEN];
    uint8_t  wifi_bssid[6];
    bool     wifi_bssid_set;     /* false = любой роутер с этим SSID */
    char     hostname[HOSTNAME_LEN];
    /* MQTT */
    char     mqtt_host[MQTT_HOST_LEN];  /* IP или имя брокера, пусто = MQTT выключен */
    uint16_t mqtt_port;                 /* 1..65535 */
    char     mqtt_user[MQTT_LOGIN_LEN];
    char     mqtt_pass[MQTT_PASSWORD_LEN];
    char     mqtt_topic[MQTT_TOPIC_LEN];
    char     mqtt_discovery_topic[MQTT_TOPIC_LEN];
    uint8_t  mqtt_auto_discovery;
    /* NTP */
    char     ntp_server[NTP_HOST_LEN];
    int8_t   tz;                 /* -12..14 */
    /* Начальные значения счетчиков, кВт*ч */
    float    initial_E1;
    float    initial_E2;
    uint32_t coeff;              /* импульсов на кВт*ч, > 0 */
    /* Опрос */
    uint16_t pzem_period_s;      /* 60..3600 */
    /* Статистика (HTTP POST JSON) */
    char     stats_uri[STATS_URI_LEN];  /* пусто = передача выключена */
    uint32_t stats_period_s;            /* 60..86400 */
    /* Коррекция показаний PZEM, кВт*ч (может быть < 0). Меняется только из HA, в портале нет */
    float    energy_offset;
    /* Новые поля добавлять ПЕРЕД crc и поднимать SETTINGS_VERSION */
    uint32_t crc;                /* всегда последнее поле */
} settings_t;

esp_err_t settings_init(void);                  /* читает NVS в RAM; при ошибке - defaults */
const settings_t *settings_get(void);           /* только чтение */
esp_err_t settings_save(const settings_t *s);   /* проверка + NVS + обновление RAM */
esp_err_t settings_reset_defaults(void);
void settings_fill_defaults(settings_t *s);
bool settings_validate(const settings_t *s);

 
 
