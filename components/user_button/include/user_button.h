// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"

/* Кнопка между пином и GND. Включается внутренняя подтяжка к питанию (пины 34..39 ее не имеют). */
typedef void (*user_button_cb_t)(void);

/* cb вызывается один раз, когда кнопка удержана hold_ms миллисекунд.
   Следующий вызов возможен только после отпускания кнопки. */
esp_err_t user_button_start(gpio_num_t pin, uint32_t hold_ms, user_button_cb_t cb);

 
 
