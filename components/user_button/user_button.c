// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "user_button.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "button";

#define POLL_MS 50

static gpio_num_t s_pin;
static uint32_t s_hold_ms;
static user_button_cb_t s_cb;

static void button_task(void *arg) {
    uint32_t held = 0;
    bool fired = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        if (gpio_get_level(s_pin) == 0) {          /* нажата (замкнута на GND) */
            held += POLL_MS;
            if (!fired && held >= s_hold_ms) {
                fired = true;
                ESP_LOGI(TAG, "long press detected");
                if (s_cb) s_cb();
            }
        } else {
            held = 0;
            fired = false;
        }
    }
}

esp_err_t user_button_start(gpio_num_t pin, uint32_t hold_ms, user_button_cb_t cb) {
    if (pin >= GPIO_NUM_34 && pin <= GPIO_NUM_39) return ESP_ERR_INVALID_ARG;   /* нет внутренней подтяжки */
    s_pin = pin;
    s_hold_ms = hold_ms;
    s_cb = cb;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t e = gpio_config(&cfg);
    if (e != ESP_OK) return e;
    return xTaskCreate(button_task, "button", 2048, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

 
 
