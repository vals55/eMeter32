// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

/* Периодичность повторной сверки времени */
#define NTP_SYNC_INTERVAL_MS   (24UL * 60 * 60 * 1000)   /* раз в сутки */
/* Не дергать сервер при частых переподключениях Wi-Fi: если время сверено
   менее этого числа секунд назад, повторный запрос при подключении пропускается */
#define NTP_MIN_RESYNC_S       3600

/* Устанавливает часовой пояс из settings (tz, часы от UTC). Сети не требует: вызовите после settings_init(). */
void ntp_sync_init(void);

/* Вызвать, когда Wi-Fi подключился и получил IP. Первый вызов инициализирует SNTP-клиент
   (адрес сервера берется из settings), затем время сверяется сразу и далее раз в сутки. */
void ntp_sync_on_wifi_connected(void);

bool   ntp_sync_is_synced(void);      /* время хотя бы раз сверено */
time_t ntp_sync_last_sync(void);      /* момент последней сверки (unix), 0 - не было */

 
 
