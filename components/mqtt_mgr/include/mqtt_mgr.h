// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Тонкая обёртка над esp-mqtt. Пустой mqtt_host в настройках = MQTT выключен.
   Иерархия топиков: <mqtt_topic>/<номер чипа>/..., где номер чипа - последние 3 байта MAC (6 hex-символов, как сенсор chip_id).
   Статус (LWT): <base>/status = online/offline */

typedef void (*mqtt_mgr_connected_cb_t)(void);
/* topic и data - копии с завершающим нулём, живут только во время вызова */
typedef void (*mqtt_mgr_message_cb_t)(const char *topic, const char *data, int len);

/* Регистрировать до mqtt_mgr_start() */
void mqtt_mgr_set_callbacks(mqtt_mgr_connected_cb_t on_connected, mqtt_mgr_message_cb_t on_message);

/* Запускает клиент (один раз; повторные вызовы игнорируются). Вызывать после получения IP. */
esp_err_t mqtt_mgr_start(void);

bool mqtt_mgr_is_connected(void);

/* Базовый топик устройства: <mqtt_topic>/<номер чипа> */
const char *mqtt_mgr_base_topic(void);
/* Номер чипа: 6 hex-символов (MAC[3..5]) */
const char *mqtt_mgr_chip_id(void);
/* Прежний базовый топик <mqtt_topic>/<hostname> - только для миграции данных */
const char *mqtt_mgr_old_base_topic(void);

/* Возвращает id сообщения (>=0) или -1, если нет соединения */
int mqtt_mgr_publish(const char *topic, const char *payload, int qos, bool retain);
int mqtt_mgr_subscribe(const char *topic, int qos);
 
 
