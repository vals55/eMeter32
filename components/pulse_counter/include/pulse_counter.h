// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"

/*
 * Счетчик импульсов на аппаратном PCNT. Каждый канал хранится в двух частях:
 *   session - импульсы с момента старта МК (аппаратный счетчик);
 *   base    - значение, восстановленное с MQTT-сервера после старта;
 *   total   = base + session   (в импульсах),  kWh = total / coeff.
 * Благодаря этому поздний приход значения с MQTT не портит счет.
 */

typedef enum {
    PULSE_CH_E1 = 0,
    PULSE_CH_E2 = 1,
    PULSE_CH_COUNT
} pulse_ch_t;

#define PULSE_COEFF_MIN 1
#define PULSE_COEFF_MAX 1000000

typedef struct {
    gpio_num_t gpio[PULSE_CH_COUNT];
    uint32_t   coeff;          /* импульсов на кВт*ч (3200 и т.п.), PULSE_COEFF_MIN..MAX */
    uint32_t   glitch_ns;      /* аппаратный фильтр помех, 0 = выключен */
    bool       count_falling;  /* false - по переднему фронту, true - по заднему */
    bool       pull_up;        /* включить внутреннюю подтяжку к питанию */
} pulse_counter_cfg_t;

typedef struct {
    uint32_t session;          /* с момента старта МК (или с последней коррекции) */
    uint32_t base;             /* восстановленное с MQTT или заданное коррекцией */
    uint32_t total;            /* base + session, импульсы */
    double   kwh;              /* total / coeff */
    bool     restored;         /* base уже получен с MQTT (или задан коррекцией) */
} pulse_counter_data_t;

/* Вызывается из служебной задачи, когда total или коэффициент изменились (не чаще раза в секунду) */
typedef void (*pulse_counter_cb_t)(pulse_ch_t ch, const pulse_counter_data_t *d, void *arg);

esp_err_t pulse_counter_start(const pulse_counter_cfg_t *cfg);
esp_err_t pulse_counter_get(pulse_ch_t ch, pulse_counter_data_t *out);

/*
 * Восстановление base после старта. Передавайте сюда только значение, сохраненное
 * ПРЕДЫДУЩИМ запуском (первое retained-сообщение). Применяется один раз за запуск:
 * повторный вызов вернет ESP_ERR_INVALID_STATE, чтобы повторная доставка retained-сообщения
 * (уже включающего session) не удвоила счет. Если коррекция уже была, восстановление игнорируется.
 */
esp_err_t pulse_counter_restore(pulse_ch_t ch, uint32_t pulses);

/*
 * Коррекция текущего значения канала (команда из HA): base = pulses, session = 0.
 * Можно вызывать в любой момент, любое число раз. После вызова поздний restore не применяется.
 */
esp_err_t pulse_counter_set_total(pulse_ch_t ch, uint32_t pulses);

/* То же в киловатт-часах: pulses = round(kwh * coeff). Возвращает ESP_ERR_INVALID_ARG при выходе за диапазон. */
esp_err_t pulse_counter_set_kwh(pulse_ch_t ch, double kwh);

/*
 * Смена коэффициента (команда из HA). Число импульсов не меняется, пересчитываются киловатт-часы.
 * Сохранение в NVS - на вызывающей стороне: settings_t s = *settings_get(); s.coeff = coeff; settings_save(&s);
 */
esp_err_t pulse_counter_set_coeff(uint32_t coeff);

/* Регистрирует обработчик изменений (для MQTT). Вызывайте до или после start. */
void pulse_counter_set_callback(pulse_counter_cb_t cb, void *arg);

uint32_t pulse_counter_coeff(void);

 
 
