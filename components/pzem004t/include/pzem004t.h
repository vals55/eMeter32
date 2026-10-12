// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/uart.h"
#include "driver/gpio.h"

/* Адрес Modbus: 0xF8 - универсальный адрес, работает с одним модулем на линии */
#define PZEM_ADDR 0xF8

typedef struct {
    bool     valid;          /* хотя бы одно успешное чтение */
    bool     alarm;          /* сработала тревога по мощности */
    float    voltage;        /* В */
    float    current;        /* А */
    float    power;          /* Вт */
    uint32_t energy_wh;      /* Вт*ч, накопленная энергия модуля */
    float    energy_kwh;     /* кВт*ч */
    float    frequency;      /* Гц */
    float    power_factor;   /* 0..1 */
    int64_t  timestamp_ms;   /* время последнего успешного чтения (esp_timer) */
    uint32_t error_count;    /* число неудачных циклов чтения подряд */
} pzem_data_t;

/* Запускает задачу опроса. Период берется из settings (pzem_period_s). */
esp_err_t pzem_start(uart_port_t uart, gpio_num_t tx, gpio_num_t rx);
/* Копия последних данных (потокобезопасно) */
void pzem_get_last(pzem_data_t *out);
/* Запросить внеочередной опрос */
void pzem_poll_now(void);
/* Однократное блокирующее чтение (до 3 попыток) */
esp_err_t pzem_read(pzem_data_t *out);
/* Сброс накопленной энергии в модуле */
esp_err_t pzem_reset_energy(void);

 
 
