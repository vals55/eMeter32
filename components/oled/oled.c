// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "oled.h"
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "oled_font.h"

static const char *TAG = "oled";

static uint8_t s_fb[OLED_W * OLED_H / 8];
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

static esp_err_t send_cmds(const uint8_t *c, size_t n) {
    uint8_t buf[32];
    if (n > sizeof(buf) - 1) return ESP_ERR_INVALID_SIZE;
    buf[0] = 0x00;                       /* управляющий байт: далее команды */
    memcpy(buf + 1, c, n);
    return i2c_master_transmit(s_dev, buf, n + 1, 100);
}

esp_err_t oled_init(gpio_num_t sda, gpio_num_t scl, uint8_t addr) {
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_master_bus(&bc, &s_bus);
    if (e != ESP_OK) return e;

    e = i2c_master_probe(s_bus, addr, 100);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "display not found at 0x%02X (check wiring/address)", addr);
        return e;
    }
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    e = i2c_master_bus_add_device(s_bus, &dc, &s_dev);
    if (e != ESP_OK) return e;

    const uint8_t init_seq[] = {
        0xAE,             /* display off */
        0xD5, 0x80,       /* clock divide */
        0xA8, 0x3F,       /* multiplex 64 */
        0xD3, 0x00,       /* display offset */
        0x40,             /* start line 0 */
        0x8D, 0x14,       /* charge pump on */
        0x20, 0x02,       /* page addressing mode */
#if OLED_ROTATE_180
        0xA0, 0xC0,
#else
        0xA1, 0xC8,
#endif
        0xDA, 0x12,       /* COM pins */
        0x81, 0xCF,       /* contrast */
        0xD9, 0xF1,       /* pre-charge */
        0xDB, 0x40,       /* VCOMH */
        0xA4,             /* follow RAM */
        0xA6,             /* normal (not inverted) */
        0xAF,             /* display on */
    };
    e = send_cmds(init_seq, sizeof(init_seq));
    if (e != ESP_OK) return e;
    oled_clear();
    return oled_flush();
}

void oled_clear(void) {
    memset(s_fb, 0, sizeof(s_fb));
}

void oled_pixel(int x, int y, bool on) {
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
    uint8_t *p = &s_fb[(y >> 3) * OLED_W + x];
    if (on) {
        *p |= (uint8_t)(1u << (y & 7));
    } else {
        *p &= (uint8_t)~(1u << (y & 7));
    }
}

void oled_fill_rect(int x, int y, int w, int h, bool on) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            oled_pixel(x + i, y + j, on);
        }
    }
}

static void draw_char(int x, int y, char ch, int scale) {
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t *g = FONT5X7[ch - 32];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            if (g[col] & (1u << row)) {
                oled_fill_rect(x + col * scale, y + row * scale, scale, scale, true);
            }
        }
    }
}

void oled_text(int x, int y, const char *s, int scale) {
    if (scale < 1) scale = 1;
    for (; *s; s++) {
        draw_char(x, y, *s, scale);
        x += 6 * scale;
    }
}

int oled_text_width(const char *s, int scale) {
    return (int)strlen(s) * 6 * (scale < 1 ? 1 : scale);
}

esp_err_t oled_flush(void) {
    for (int p = 0; p < OLED_H / 8; p++) {
        const uint8_t pos[3] = { (uint8_t)(0xB0 | p), 0x00, 0x10 };
        esp_err_t e = send_cmds(pos, sizeof(pos));
        if (e != ESP_OK) return e;
        uint8_t d[1 + OLED_W];
        d[0] = 0x40;                     /* управляющий байт: далее данные */
        memcpy(d + 1, &s_fb[p * OLED_W], OLED_W);
        e = i2c_master_transmit(s_dev, d, sizeof(d), 100);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}

 
 
