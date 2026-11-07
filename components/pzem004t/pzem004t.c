 
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

/* Границы правдоподобия (в единицах регистров). PZEM сразу после включения может
 * вернуть кадр с верным CRC, но с мусорными значениями - такой кадр отбрасываем целиком. */
#define LIM_VOLTAGE_MIN   500      /* 50.0 В */
#define LIM_VOLTAGE_MAX   3000     /* 300.0 В */
#define LIM_CURRENT_MAX   100000   /* 100.000 А */
#define LIM_POWER_MAX     300000   /* 30000.0 Вт */
#define LIM_ENERGY_MAX    9999999  /* 9999.999 кВт*ч - предел счетчика PZEM */
/* Частота сети - самый надежный признак: 49.0..51.0 Гц.
 * Для генератора/инвертора с другой частотой переопределите LIM_FREQ_MIN/MAX. */
#ifndef LIM_FREQ_MIN
#define LIM_FREQ_MIN      490      /* 49.0 Гц */
#endif
#ifndef LIM_FREQ_MAX
#define LIM_FREQ_MAX      510      /* 51.0 Гц */
#endif
#define LIM_PF_MAX        100      /* 1.00 */
#define BAL_MIN_CURRENT_A 0.05f    /* ниже этого тока баланс мощности не проверяем */
#define BAL_ABS_TOL_W     2.0f
#define BAL_REL_TOL       0.05f

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
    uint16_t r_v = REG(0), r_f = REG(7), r_pf = REG(8), r_al = REG(9);
#undef REG

    /* 1) диапазоны (в первую очередь частота) */
    bool ok = r_f >= LIM_FREQ_MIN && r_f <= LIM_FREQ_MAX &&
              r_v >= LIM_VOLTAGE_MIN && r_v <= LIM_VOLTAGE_MAX &&
              cur <= LIM_CURRENT_MAX && pwr <= LIM_POWER_MAX && en <= LIM_ENERGY_MAX &&
              r_pf <= LIM_PF_MAX && (r_al == 0x0000 || r_al == 0xFFFF);
    /* 2) баланс мощности: P ~ U*I*PF (допуск на разрешение PF 0.01 и округление) */
    if (ok) {
        float v = r_v * 0.1f, i = cur * 0.001f, p = pwr * 0.1f;
        if (i >= BAL_MIN_CURRENT_A) {
            float p_exp = v * i * (r_pf * 0.01f);
            float tol = BAL_REL_TOL * p_exp + 0.005f * v * i;
            if (tol < BAL_ABS_TOL_W) tol = BAL_ABS_TOL_W;
            float diff = p > p_exp ? p - p_exp : p_exp - p;
            if (diff > tol) ok = false;
        }
    }
    if (!ok) {
        ESP_LOGW(TAG, "implausible frame dropped: U=%u I=%lu P=%lu E=%lu F=%u PF=%u AL=0x%04X (raw)",
                 r_v, (unsigned long)cur, (unsigned long)pwr, (unsigned long)en, r_f, r_pf, r_al);
        return ESP_ERR_INVALID_RESPONSE;
    }

    out->voltage = r_v * 0.1f;
    out->current = cur * 0.001f;
    out->power = pwr * 0.1f;
    out->energy_wh = en;
    out->energy_kwh = en / 1000.0f;
    out->frequency = r_f * 0.1f;
    out->power_factor = r_pf * 0.01f;
    out->alarm = (r_al == 0xFFFF);
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
    if (!s_mutex) {                 /* pzem_start() ещё не вызывался */
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_last;
    xSemaphoreGive(s_mutex);
}

bool pzem_data_fresh(const pzem_data_t *d) {
    if (!d->valid || d->error_count > 0) return false;
    int64_t age_ms = esp_timer_get_time() / 1000 - d->timestamp_ms;
    int64_t limit_ms = (int64_t)settings_get()->pzem_period_s * 2000 + 10000;
    return age_ms <= limit_ms;
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
