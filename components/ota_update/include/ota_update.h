// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Обновление прошивки по Wi-Fi: HTTP-сервер по требованию.
   Сервер запускается командой (кнопка в HA), на ota_update_start(), и сам выключается через OTA_SERVER_TIMEOUT_S.
   Страница: http://<IP>/update, доступ - Basic-авторизация: логин OTA_USER, пароль - одноразовый PIN (6 цифр),
   который генерируется при каждом запуске сервера. Загружается app-образ (firmware.bin из .pio/build/<env>/). */

#define OTA_SERVER_TIMEOUT_S 600      /* через сколько секунд бездействия сервер выключается */
#define OTA_USER             "ota"
#define OTA_MAX_AUTH_FAILS   5        /* столько неверных паролей - сервер выключается */

/* Вызывается при включении и выключении сервера (из любой задачи) */
typedef void (*ota_update_state_cb_t)(bool active);

/* Один раз при старте, до остальных вызовов */
void ota_update_init(void);
void ota_update_set_state_cb(ota_update_state_cb_t cb);

/* Запустить сервер; если уже работает - продлить таймаут. Нужен IP (STA подключён). */
esp_err_t ota_update_start(void);
/* Остановить сервер (не прерывает идущую загрузку) */
void ota_update_stop(void);
bool ota_update_is_active(void);
/* PIN текущего сеанса; пустая строка, если сервер выключен */
const char *ota_update_pin(void);

/* Подтвердить новую прошивку (отключить откат). Безопасно вызывать многократно и на прошивке, залитой по кабелю. */
void ota_update_mark_valid(void);
