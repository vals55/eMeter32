// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "pzem004t.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "settings.h"

static const char *TAG = "pzem";

#define PZEM_BAUD       9600
#define PZEM_RX_BUF     256
#define PZEM_TIMEOUT_MS 500
#define PZEM_REGS       10          /* регистры 0x0000..0x0009 */
#define PZEM_RESP_LEN   (5 + PZEM_REGS * 2)   /* адрес, функция, число байт, 20 байт, CRC(2) = 25 */

static uart_port_t s_uart;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task;
static pzem_data_t s_last;

static uint16_t crc16(const uint8_t *d, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

static void put_crc(uint8_t *frame, size_t n) {
    uint16_t c = crc16(frame, n);
    frame[n] = (uint8_t)(c & 0xFF);      /* Modbus: сначала младший байт */
    frame[n + 1] = (uint8_t)(c >> 8);
}

static esp_err_t read_once(pzem_data_t *out) {
    uint8_t req[8] = { PZEM_ADDR, 0x04, 0x00, 0x00, 0x00, PZEM_REGS };
    put_crc(req, 6);
    uart_flush_input(s_uart);
    uart_write_bytes(s_uart, req, sizeof(req));

    uint8_t r[PZEM_RESP_LEN];
    int n = uart_read_bytes(s_uart, r, sizeof(r), pdMS_TO_TICKS(PZEM_TIMEOUT_MS));
    if (n == 5 && (r[1] & 0x80)) {
        ESP_LOGW(TAG, "modbus exception 0x%02X", r[2]);
        return ESP_FAIL;
    }
    if (n != PZEM_RESP_LEN) return ESP_ERR_TIMEOUT;
    if (r[1] != 0x04 || r[2] != PZEM_REGS * 2) return ESP_ERR_INVALID_RESPONSE;
    uint16_t c = crc16(r, PZEM_RESP_LEN - 2);
    if (r[PZEM_RESP_LEN - 2] != (c & 0xFF) || r[PZEM_RESP_LEN - 1] != (c >> 8)) return ESP_ERR_INVALID_CRC;

    const uint8_t *d = &r[3];
#define REG(i) ((uint16_t)((d[(i) * 2] << 8) | d[(i) * 2 + 1]))
    uint32_t cur = ((uint32_t)REG(2) << 16) | REG(1);     /* младшее слово первым */
    uint32_t pwr = ((uint32_t)REG(4) << 16) | REG(3);
    uint32_t en  = ((uint32_t)REG(6) << 16) | REG(5);
    out->voltage = REG(0) * 0.1f;
    out->current = cur * 0.001f;
    out->power = pwr * 0.1f;
    out->energy_wh = en;
    out->energy_kwh = en / 1000.0f;
    out->frequency = REG(7) * 0.1f;
    out->power_factor = REG(8) * 0.01f;
    out->alarm = (REG(9) == 0xFFFF);
#undef REG
    out->valid = true;
    out->timestamp_ms = esp_timer_get_time() / 1000;
    out->error_count = 0;
    return ESP_OK;
}

esp_err_t pzem_read(pzem_data_t *out) {
    esp_err_t e = ESP_FAIL;
    for (int i = 0; i < 3; i++) {
        e = read_once(out);
        if (e == ESP_OK) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return e;
}

esp_err_t pzem_reset_energy(void) {
    uint8_t req[4] = { PZEM_ADDR, 0x42 };
    put_crc(req, 2);
    uart_flush_input(s_uart);
    uart_write_bytes(s_uart, req, sizeof(req));
    uint8_t r[4];
    int n = uart_read_bytes(s_uart, r, sizeof(r), pdMS_TO_TICKS(PZEM_TIMEOUT_MS));
    return (n == 4 && r[1] == 0x42) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void pzem_get_last(pzem_data_t *out) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_last;
    xSemaphoreGive(s_mutex);
}

void pzem_poll_now(void) {
    if (s_task) xTaskNotifyGive(s_task);
}

static void pzem_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(1000));      /* дать модулю стартовать */
    for (;;) {
        pzem_data_t d = { 0 };
        esp_err_t e = pzem_read(&d);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (e == ESP_OK) {
            s_last = d;
        } else {
            s_last.error_count++;
        }
        xSemaphoreGive(s_mutex);
        if (e == ESP_OK) {
            ESP_LOGI(TAG, "U=%.1fV I=%.3fA P=%.1fW E=%luWh f=%.1fHz PF=%.2f",
                     d.voltage, d.current, d.power, (unsigned long)d.energy_wh, d.frequency, d.power_factor);
        } else {
            ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(e));
        }
        uint32_t period = settings_get()->pzem_period_s;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(period * 1000UL));
    }
}

esp_err_t pzem_start(uart_port_t uart, gpio_num_t tx, gpio_num_t rx) {
    s_uart = uart;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    uart_config_t uc = {
        .baud_rate = PZEM_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t e = uart_driver_install(uart, PZEM_RX_BUF, 0, 0, NULL, 0);
    if (e != ESP_OK) return e;
    e = uart_param_config(uart, &uc);
    if (e != ESP_OK) return e;
    e = uart_set_pin(uart, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (e != ESP_OK) return e;
    return xTaskCreate(pzem_task, "pzem", 3072, NULL, 4, &s_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

 
 
