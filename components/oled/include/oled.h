// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"

#define OLED_W 128
#define OLED_H 64
/* Двухцветный модуль: строки y = 0..15 желтые, y = 16..63 синие */
#define OLED_YELLOW_H 16

/* 1 - перевернуть изображение на 180 градусов */
#define OLED_ROTATE_180 0

esp_err_t oled_init(gpio_num_t sda, gpio_num_t scl, uint8_t addr);
void oled_clear(void);
void oled_pixel(int x, int y, bool on);
void oled_fill_rect(int x, int y, int w, int h, bool on);
/* scale = 1 -> символ 6x8 пикселей (5 + пробел), scale = 2 -> 12x16 */
void oled_text(int x, int y, const char *s, int scale);
int  oled_text_width(const char *s, int scale);
esp_err_t oled_flush(void);

 
 
