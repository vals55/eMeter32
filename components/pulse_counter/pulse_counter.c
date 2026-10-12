// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "pulse_counter.h"
#include <math.h>
#include <string.h>
#include "driver/pulse_cnt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "pulses";

#define PCNT_HIGH_LIMIT    32767
#define PCNT_LOW_LIMIT    -32768
#define GLITCH_NS_MAX      12000      /* предел аппаратного фильтра ~12,8 мкс */
#define NOTIFY_PERIOD_MS   1000

static pcnt_unit_handle_t s_unit[PULSE_CH_COUNT];
static uint32_t s_base[PULSE_CH_COUNT];
static bool s_restored[PULSE_CH_COUNT];
static bool s_dirty[PULSE_CH_COUNT];          /* нужно уведомить даже если total не менялся */
static uint32_t s_last_total[PULSE_CH_COUNT];
static uint32_t s_coeff;
static bool s_started;
static SemaphoreHandle_t s_lock;
static pulse_counter_cb_t s_cb;
static void *s_cb_arg;

static uint32_t read_session(pulse_ch_t ch) {
    int c = 0;
    if (pcnt_unit_get_count(s_unit[ch], &c) != ESP_OK || c < 0) return 0;
    return (uint32_t)c;
}

esp_err_t pulse_counter_get(pulse_ch_t ch, pulse_counter_data_t *out) {
    if (!s_started || ch >= PULSE_CH_COUNT || !out) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t session = read_session(ch);
    uint32_t base = s_base[ch];
    bool restored = s_restored[ch];
    uint32_t coeff = s_coeff;
    xSemaphoreGive(s_lock);

    uint64_t total = (uint64_t)base + session;
    if (total > UINT32_MAX) total = UINT32_MAX;
    out->session = session;
    out->base = base;
    out->total = (uint32_t)total;
    out->kwh = (double)(uint32_t)total / (double)coeff;
    out->restored = restored;
    return ESP_OK;
}

esp_err_t pulse_counter_restore(pulse_ch_t ch, uint32_t pulses) {
    if (!s_started || ch >= PULSE_CH_COUNT) return ESP_ERR_INVALID_ARG;
    esp_err_t e = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t session = read_session(ch);
    if (s_restored[ch]) {
        e = ESP_ERR_INVALID_STATE;
    } else if ((uint64_t)pulses + session > UINT32_MAX) {
        e = ESP_ERR_INVALID_ARG;
    } else {
        s_base[ch] = pulses;
        s_restored[ch] = true;
        s_dirty[ch] = true;
    }
    xSemaphoreGive(s_lock);
    if (e == ESP_OK) ESP_LOGI(TAG, "E%d restored: base=%lu session=%lu", ch + 1, (unsigned long)pulses, (unsigned long)session);
    return e;
}

esp_err_t pulse_counter_set_total(pulse_ch_t ch, uint32_t pulses) {
    if (!s_started || ch >= PULSE_CH_COUNT) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_base[ch] = pulses;
    s_restored[ch] = true;
    s_dirty[ch] = true;
    esp_err_t e = pcnt_unit_clear_count(s_unit[ch]);      /* session = 0 */
    xSemaphoreGive(s_lock);
    if (e == ESP_OK) ESP_LOGI(TAG, "E%d corrected: base=%lu, session reset", ch + 1, (unsigned long)pulses);
    return e;
}

esp_err_t pulse_counter_set_kwh(pulse_ch_t ch, double kwh) {
    if (!s_started || ch >= PULSE_CH_COUNT) return ESP_ERR_INVALID_ARG;
    if (!isfinite(kwh) || kwh < 0) return ESP_ERR_INVALID_ARG;
    double p = round(kwh * (double)s_coeff);
    if (p > (double)UINT32_MAX) return ESP_ERR_INVALID_ARG;
    return pulse_counter_set_total(ch, (uint32_t)p);
}

esp_err_t pulse_counter_set_coeff(uint32_t coeff) {
    if (!s_started || coeff < PULSE_COEFF_MIN || coeff > PULSE_COEFF_MAX) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_coeff = coeff;
    for (int ch = 0; ch < PULSE_CH_COUNT; ch++) s_dirty[ch] = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "coeff changed: %lu imp/kWh", (unsigned long)coeff);
    return ESP_OK;
}

void pulse_counter_set_callback(pulse_counter_cb_t cb, void *arg) {
    s_cb_arg = arg;
    s_cb = cb;
}

uint32_t pulse_counter_coeff(void) {
    return s_coeff;
}

static bool take_dirty(int ch) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool d = s_dirty[ch];
    s_dirty[ch] = false;
    xSemaphoreGive(s_lock);
    return d;
}

static void notify_task(void *arg) {
    for (;;) {
        for (int ch = 0; ch < PULSE_CH_COUNT; ch++) {
            pulse_counter_data_t d;
            if (pulse_counter_get((pulse_ch_t)ch, &d) != ESP_OK) continue;
            bool dirty = take_dirty(ch);
            if (d.total != s_last_total[ch] || dirty) {
                s_last_total[ch] = d.total;
                ESP_LOGI(TAG, "E%d session=%lu base=%lu total=%lu (%.3f kWh)", ch + 1,
                         (unsigned long)d.session, (unsigned long)d.base,
                         (unsigned long)d.total, d.kwh);
                pulse_counter_cb_t cb = s_cb;
                if (cb) cb((pulse_ch_t)ch, &d, s_cb_arg);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(NOTIFY_PERIOD_MS));
    }
}

static esp_err_t setup_channel(pulse_ch_t ch, const pulse_counter_cfg_t *cfg) {
    pcnt_unit_config_t uc = {
        .low_limit = PCNT_LOW_LIMIT,
        .high_limit = PCNT_HIGH_LIMIT,
        .flags.accum_count = true,      /* накапливать счет при переполнении 16-битного счетчика */
    };
    esp_err_t e = pcnt_new_unit(&uc, &s_unit[ch]);
    if (e != ESP_OK) return e;

    if (cfg->glitch_ns > 0) {
        pcnt_glitch_filter_config_t fc = {
            .max_glitch_ns = cfg->glitch_ns > GLITCH_NS_MAX ? GLITCH_NS_MAX : cfg->glitch_ns,
        };
        e = pcnt_unit_set_glitch_filter(s_unit[ch], &fc);
        if (e != ESP_OK) return e;
    }

    pcnt_chan_config_t cc = {
        .edge_gpio_num = cfg->gpio[ch],
        .level_gpio_num = -1,
    };
    pcnt_channel_handle_t chan;
    e = pcnt_new_channel(s_unit[ch], &cc, &chan);
    if (e != ESP_OK) return e;

    pcnt_channel_edge_action_t pos = cfg->count_falling ? PCNT_CHANNEL_EDGE_ACTION_HOLD : PCNT_CHANNEL_EDGE_ACTION_INCREASE;
    pcnt_channel_edge_action_t neg = cfg->count_falling ? PCNT_CHANNEL_EDGE_ACTION_INCREASE : PCNT_CHANNEL_EDGE_ACTION_HOLD;
    e = pcnt_channel_set_edge_action(chan, pos, neg);
    if (e != ESP_OK) return e;

    if (cfg->pull_up) gpio_set_pull_mode(cfg->gpio[ch], GPIO_PULLUP_ONLY);

    /* Для flags.accum_count переполнение копится только при watch point на обоих пределах */
    e = pcnt_unit_add_watch_point(s_unit[ch], PCNT_HIGH_LIMIT);
    if (e != ESP_OK) return e;
    e = pcnt_unit_add_watch_point(s_unit[ch], PCNT_LOW_LIMIT);
    if (e != ESP_OK) return e;

    e = pcnt_unit_enable(s_unit[ch]);
    if (e != ESP_OK) return e;
    e = pcnt_unit_clear_count(s_unit[ch]);
    if (e != ESP_OK) return e;
    return pcnt_unit_start(s_unit[ch]);
}

esp_err_t pulse_counter_start(const pulse_counter_cfg_t *cfg) {
    if (s_started || !cfg || cfg->coeff < PULSE_COEFF_MIN || cfg->coeff > PULSE_COEFF_MAX) return ESP_ERR_INVALID_ARG;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    s_coeff = cfg->coeff;
    for (int ch = 0; ch < PULSE_CH_COUNT; ch++) {
        s_last_total[ch] = UINT32_MAX;     /* чтобы первое значение ушло в callback */
        esp_err_t e = setup_channel((pulse_ch_t)ch, cfg);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "channel E%d setup failed: %s", ch + 1, esp_err_to_name(e));
            return e;
        }
        ESP_LOGI(TAG, "E%d on GPIO%d, coeff=%lu, %s edge", ch + 1, (int)cfg->gpio[ch],
                 (unsigned long)cfg->coeff, cfg->count_falling ? "falling" : "rising");
    }
    s_started = true;
    return xTaskCreate(notify_task, "pulses", 3072, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

 
 
