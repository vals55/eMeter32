// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include "esp_err.h"

/* Discovery для Home Assistant, публикация состояний и обработка команд (number).
   Вызвать один раз при старте, до mqtt_mgr_start(). Без MQTT (пустой mqtt_host) ничего не публикует.
   Топики: <mqtt_topic>/<номер чипа>/<id>/{state|set|attributes}.
   Основной сенсор канала - CHANNEL_ENTITIES[0] (энергия), остальные записи - его атрибуты;
   число импульсов (атрибут "imp", retained) используется для восстановления счётчиков.
   Кнопка "Firmware update" (<base>/ota/set = PRESS) запускает сервер обновления (ota_update). */
esp_err_t ha_discovery_start(void);
 
 
