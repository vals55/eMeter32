// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include "driver/gpio.h"
#include "driver/uart.h"

/* Все назначения выводов в одном месте. Проверьте по маркировке своей платы! */

/* Дисплей SSD1306 (I2C) */
#define PIN_I2C_SDA          GPIO_NUM_21
#define PIN_I2C_SCL          GPIO_NUM_22
#define OLED_I2C_ADDR        0x3C          /* 0x78 в 8-битной записи */

/* PZEM-004T (UART, 9600 8N1) */
#define PZEM_UART            UART_NUM_1
#define PIN_PZEM_TX          GPIO_NUM_17   /* TX платы -> RX PZEM */
#define PIN_PZEM_RX          GPIO_NUM_16   /* RX платы <- TX PZEM (через делитель/конвертер уровней!) */

/* Кнопка вызова портала настроек (между пином и GND) */
#define PIN_PORTAL_BUTTON    GPIO_NUM_27
#define PORTAL_BUTTON_HOLD_MS 3000

/* Входы импульсов от компараторов LM393 (каналы энергии E1 и E2) */
#define PIN_PULSE_E1         GPIO_NUM_32
#define PIN_PULSE_E2         GPIO_NUM_33
#define PULSE_GLITCH_NS      10000         /* фильтр помех, не более ~12800 нс */
#define PULSE_COUNT_FALLING  0             /* 0 - считать по переднему фронту (low->high), 1 - по заднему */
#define PULSE_PULL_UP        1             /* внутренняя подтяжка (нужна для выхода LM393 с открытым коллектором) */

 
 
