// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include "esp_err.h"

/* Запускает периодическую отправку статистики (HTTP POST, JSON).
   Если stats_uri пуст, задача не создаётся. */
esp_err_t stats_push_start(void);
 
 
