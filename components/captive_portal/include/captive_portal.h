// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include "esp_err.h"

/* Запускает точку доступа, DNS-перехват и веб-сервер настроек.
   Можно вызывать из любого контекста (в т.ч. из callback wifi_mgr). */
esp_err_t captive_portal_start(void);

 
 
