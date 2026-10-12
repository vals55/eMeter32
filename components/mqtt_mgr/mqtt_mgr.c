// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "mqtt_mgr.h"
#include <stdio.h>
#include <string.h>
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "settings.h"

static const char *TAG = "mqtt";

#define MQTT_RX_MAX 96

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_chip[8];
static char s_base[128];
static char s_old_base[128];
static char s_status[160];
static mqtt_mgr_connected_cb_t s_on_connected;
static mqtt_mgr_message_cb_t s_on_message;

void mqtt_mgr_set_callbacks(mqtt_mgr_connected_cb_t on_connected, mqtt_mgr_message_cb_t on_message) {
    s_on_connected = on_connected;
    s_on_message = on_message;
}

/* префикс из настроек без хвостовых '/' */
static size_t get_prefix(char *prefix, size_t n) {
    strlcpy(prefix, settings_get()->mqtt_topic, n);
    size_t l = strlen(prefix);
    while (l > 0 && prefix[l - 1] == '/') prefix[--l] = '\0';
    return l;
}

const char *mqtt_mgr_chip_id(void) {
    if (s_chip[0] == '\0') {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_chip, sizeof(s_chip), "%02X%02X%02X", mac[3], mac[4], mac[5]);
    }
    return s_chip;
}

const char *mqtt_mgr_base_topic(void) {
    if (s_base[0] == '\0') {
        char prefix[MQTT_TOPIC_LEN];
        if (get_prefix(prefix, sizeof(prefix))) {
            snprintf(s_base, sizeof(s_base), "%s/%s", prefix, mqtt_mgr_chip_id());
        } else {
            snprintf(s_base, sizeof(s_base), "%s", mqtt_mgr_chip_id());
        }
    }
    return s_base;
}

const char *mqtt_mgr_old_base_topic(void) {
    if (s_old_base[0] == '\0') {
        char prefix[MQTT_TOPIC_LEN];
        const char *host = settings_get()->hostname;
        if (get_prefix(prefix, sizeof(prefix))) {
            snprintf(s_old_base, sizeof(s_old_base), "%s/%s", prefix, host);
        } else {
            snprintf(s_old_base, sizeof(s_old_base), "%s", host);
        }
    }
    return s_old_base;
}

bool mqtt_mgr_is_connected(void) {
    return s_client && s_connected;
}

int mqtt_mgr_publish(const char *topic, const char *payload, int qos, bool retain) {
    if (!mqtt_mgr_is_connected()) return -1;
    return esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain ? 1 : 0);
}

int mqtt_mgr_subscribe(const char *topic, int qos) {
    if (!mqtt_mgr_is_connected()) return -1;
    return esp_mqtt_client_subscribe(s_client, topic, qos);
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "connected, base topic %s", mqtt_mgr_base_topic());
        mqtt_mgr_publish(s_status, "online", 1, true);
        if (s_on_connected) s_on_connected();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected");
        break;
    case MQTT_EVENT_DATA: {
        /* принимаем только короткие неразбитые сообщения */
        if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len) break;
        char topic[192];
        char payload[MQTT_RX_MAX];
        if (ev->topic_len <= 0 || ev->topic_len >= (int)sizeof(topic) || ev->data_len >= MQTT_RX_MAX) break;
        memcpy(topic, ev->topic, (size_t)ev->topic_len);
        topic[ev->topic_len] = '\0';
        memcpy(payload, ev->data, (size_t)ev->data_len);
        payload[ev->data_len] = '\0';
        if (s_on_message) s_on_message(topic, payload, ev->data_len);
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "mqtt error");
        break;
    default:
        break;
    }
}

esp_err_t mqtt_mgr_start(void) {
    if (s_client) return ESP_OK;
    const settings_t *c = settings_get();
    if (c->mqtt_host[0] == '\0') {
        ESP_LOGI(TAG, "mqtt_host is empty, MQTT disabled");
        return ESP_OK;
    }
    snprintf(s_status, sizeof(s_status), "%s/status", mqtt_mgr_base_topic());

    esp_mqtt_client_config_t cfg = {
        .broker.address.hostname = c->mqtt_host,
        .broker.address.port = c->mqtt_port,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.client_id = c->hostname,
        .credentials.username = c->mqtt_user[0] ? c->mqtt_user : NULL,
        .credentials.authentication.password = c->mqtt_pass[0] ? c->mqtt_pass : NULL,
        .session.keepalive = 60,
        .session.last_will.topic = s_status,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .network.reconnect_timeout_ms = 10000,
        .task.stack_size = 8192,
        .buffer.size = 1536,
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) return ESP_FAIL;
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, event_handler, NULL);
    esp_err_t e = esp_mqtt_client_start(s_client);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(e));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return e;
    }
    ESP_LOGI(TAG, "connecting to %s:%u", c->mqtt_host, (unsigned)c->mqtt_port);
    return ESP_OK;
}
 
 
