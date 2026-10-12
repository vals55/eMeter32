// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ha_discovery.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "mqtt_mgr.h"
#include "settings.h"
#include "pzem004t.h"
#include "pulse_counter.h"
#include "wifi_mgr.h"
#include "ntp_sync.h"
#include "resources.h"
#include "ota_update.h"

static const char *TAG = "ha";

/* Иерархия топиков: <mqtt_topic>/<номер чипа>/<id>/{state|set|attributes}, статус - <base>/status.
   Основа учёта - импульсы. Канал в HA: основной сенсор = CHANNEL_ENTITIES[0] (энергия, только отображение),
   остальные записи CHANNEL_ENTITIES[1..] публикуются его атрибутами (JSON в <base>/<id>/attributes, retained).
   Число импульсов лежит в атрибуте PULSES_KEY и используется для восстановления после перезагрузки.
   Канал публикуется при изменении сотой доли кВт*ч (каждые coeff/100 импульсов) и по периоду pzem_period_s.
   "Set energy E1/E2" - только команда из HA: состояние не публикуется (в HA сущность optimistic).
   Энергия в кВт*ч везде публикуется и вводится с точностью 2 знака после запятой. */
#define PULSES_KEY       "imp"      /* id i_imp0 без цифры канала */
#define LEGACY_PULSES_ID "imp0"     /* прежний служебный топик <base>/imp<N>/state: только для миграции */

static bool s_legacy_cleared[CHANNEL_COUNT];
static bool s_pub_have[CHANNEL_COUNT];
static uint32_t s_pub_bucket[CHANNEL_COUNT];    /* последняя опубликованная сотая доля кВт*ч */

/* сотые доли кВт*ч: floor(total * 100 / coeff), без потерь для любого коэффициента */
static uint32_t energy_bucket(uint32_t total) {
    uint32_t coeff = pulse_counter_coeff();
    if (coeff == 0) coeff = 1;
    return (uint32_t)(((uint64_t)total * 100ULL) / coeff);
}

/* ---------- JSON-строитель ---------- */

typedef struct { char *buf; size_t cap, len; } sb_t;

static void sb_add(sb_t *s, const char *fmt, ...) {
    if (s->len >= s->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap);
    va_end(ap);
    if (n > 0) s->len += (size_t)n;
}

static void sb_esc(sb_t *s, const char *v) {
    for (; *v; v++) {
        unsigned char ch = (unsigned char)*v;
        if (ch == '"' || ch == '\\') sb_add(s, "\\%c", ch);
        else if (ch < 0x20) sb_add(s, "\\u%04x", ch);
        else sb_add(s, "%c", ch);
    }
}

/* "key":"value", - пропускается, если значение пустое */
static void sb_kv(sb_t *s, const char *key, const char *val) {
    if (!val || !val[0]) return;
    sb_add(s, "\"%s\":\"", key);
    sb_esc(s, val);
    sb_add(s, "\",");
}

/* убирает последнюю запятую, если она есть */
static void sb_trim_comma(sb_t *s) {
    if (s->len > 0 && s->len < s->cap && s->buf[s->len - 1] == ',') s->buf[--s->len] = '\0';
}

/* ---------- Вспомогательное ---------- */

static bool is_number(const char *const *e) { return strcmp(e[0], s_number) == 0; }

/* "Set energy E1/E2": только команда, без состояния */
static bool is_set_energy(const char *id) { return !strcmp(id, i_e1) || !strcmp(id, i_e2); }

static void chan_topic_id(const char *id, int ch, char *out, size_t n) {
    strlcpy(out, id, n);
    size_t l = strlen(out);
    if (l) out[l - 1] = (char)('0' + ch);       /* energy0 -> energy1 */
}

static void pub_state(const char *topic_id, const char *val, bool retain) {
    char t[256];
    snprintf(t, sizeof(t), "%s/%s/state", mqtt_mgr_base_topic(), topic_id);
    mqtt_mgr_publish(t, val, 0, retain);
}

/* топик атрибутов основного сенсора канала */
static void chan_attr_topic(int ch, char *out, size_t n) {
    char tid[40];
    chan_topic_id(CHANNEL_ENTITIES[0][2], ch, tid, sizeof(tid));
    snprintf(out, n, "%s/%s/attributes", mqtt_mgr_base_topic(), tid);
}

/* то же в прежней иерархии <mqtt_topic>/<hostname>/... (миграция) */
static void old_attr_topic(int ch, char *out, size_t n) {
    char tid[40];
    chan_topic_id(CHANNEL_ENTITIES[0][2], ch, tid, sizeof(tid));
    snprintf(out, n, "%s/%s/attributes", mqtt_mgr_old_base_topic(), tid);
}

/* ---------- Значения ---------- */

static bool get_general(const char *id, char *out, size_t n) {
    const settings_t *c = settings_get();
    pzem_data_t p;
    pzem_get_last(&p);

    if (!strcmp(id, i_voltage)) { if (!p.valid) return false; snprintf(out, n, "%.1f", p.voltage); return true; }
    if (!strcmp(id, i_current)) { if (!p.valid) return false; snprintf(out, n, "%.3f", p.current); return true; }
    if (!strcmp(id, i_power))   { if (!p.valid) return false; snprintf(out, n, "%.1f", p.power); return true; }
    if (!strcmp(id, i_freq))    { if (!p.valid) return false; snprintf(out, n, "%.1f", p.frequency); return true; }
    if (!strcmp(id, i_pf))      { if (!p.valid) return false; snprintf(out, n, "%.2f", p.power_factor); return true; }
    if (!strcmp(id, i_energy))  {   /* показания PZEM с коррекцией energy_offset, 2 знака */
        if (!p.valid) return false;
        snprintf(out, n, "%.2f", (double)p.energy_kwh + (double)c->energy_offset);
        return true;
    }
    if (!strcmp(id, i_time)) {      /* время последнего удачного опроса PZEM, ISO 8601 с поясом */
        if (!p.valid || !ntp_sync_is_synced()) return false;
        int64_t age_s = (esp_timer_get_time() / 1000 - p.timestamp_ms) / 1000;
        time_t tt = time(NULL) - (time_t)age_s;
        struct tm tm;
        localtime_r(&tt, &tm);
        size_t l = strftime(out, n, "%Y-%m-%dT%H:%M:%S", &tm);
        snprintf(out + l, n - l, "%+03d:00", (int)c->tz);
        return true;
    }
    /* i_e1 / i_e2 ("Set energy"): состояния нет - значение задаётся только из HA */
    if (!strcmp(id, i_e0))       { snprintf(out, n, "%.2f", (double)c->energy_offset); return true; }
    if (!strcmp(id, i_constant)) { snprintf(out, n, "%lu", (unsigned long)c->coeff); return true; }
    if (!strcmp(id, i_mqtt_per)) { snprintf(out, n, "%u", (unsigned)c->pzem_period_s); return true; }
    if (!strcmp(id, i_stat_per)) { snprintf(out, n, "%lu", (unsigned long)c->stats_period_s); return true; }
    if (!strcmp(id, i_rssi)) {
        int r = wifi_mgr_rssi();
        if (r == 0) return false;
        snprintf(out, n, "%d", r);
        return true;
    }
    return false;
}

/* Значение записи CHANNEL_ENTITIES (числовое): энергия кВт*ч (2 знака) для отображения, импульсы - как есть */
static bool get_channel(const char *id, int ch, char *out, size_t n) {
    pulse_counter_data_t d;
    if (pulse_counter_get((pulse_ch_t)ch, &d) != ESP_OK || !d.restored) return false;
    if (!strcmp(id, i_energy0)) { snprintf(out, n, "%.2f", d.kwh); return true; }
    if (!strcmp(id, i_imp0))    { snprintf(out, n, "%lu", (unsigned long)d.total); return true; }
    return false;
}

/* Атрибуты сенсора RSSI: ip, mac, chip_id */
static void publish_rssi_attributes(void) {
    char buf[160], t[256], ip[20], m[20];
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(m, sizeof(m), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    sb_t s = { buf, sizeof(buf), 0 };
    sb_add(&s, "{");
    if (wifi_mgr_ip(ip, sizeof(ip)) == ESP_OK) sb_kv(&s, i_ip, ip);
    sb_kv(&s, i_mac, m);
    sb_kv(&s, i_chip, mqtt_mgr_chip_id());
    sb_trim_comma(&s);
    sb_add(&s, "}");
    if (s.len >= s.cap) return;
    snprintf(t, sizeof(t), "%s/%s/attributes", mqtt_mgr_base_topic(), i_rssi);
    mqtt_mgr_publish(t, buf, 0, true);
}

static void publish_channel(int ch) {
    char v[40], tid[40], t[256], buf[160];
    pulse_counter_data_t d;
    if (pulse_counter_get((pulse_ch_t)ch, &d) != ESP_OK || !d.restored) return;   /* пока не восстановлено - не публикуем */

    s_pub_bucket[ch] = energy_bucket(d.total);      /* запоминаем, что опубликовано */
    s_pub_have[ch] = true;

    /* основной сенсор: CHANNEL_ENTITIES[0] */
    const char *main_id = CHANNEL_ENTITIES[0][2];
    if (get_channel(main_id, ch, v, sizeof(v))) {
        chan_topic_id(main_id, ch, tid, sizeof(tid));
        pub_state(tid, v, true);
    }

    /* атрибуты: CHANNEL_ENTITIES[1..], ключ = id без цифры канала, значение - число */
    sb_t s = { buf, sizeof(buf), 0 };
    sb_add(&s, "{");
    for (size_t j = 1; j < CHANNEL_ENTITY_COUNT; j++) {
        const char *id = CHANNEL_ENTITIES[j][2];
        char key[24];
        strlcpy(key, id, sizeof(key));
        size_t kl = strlen(key);
        if (kl && key[kl - 1] >= '0' && key[kl - 1] <= '9') key[kl - 1] = '\0';
        if (get_channel(id, ch, v, sizeof(v))) sb_add(&s, "\"%s\":%s,", key, v);
    }
    sb_trim_comma(&s);
    sb_add(&s, "}");
    if (s.len < s.cap) {
        chan_attr_topic(ch, t, sizeof(t));
        mqtt_mgr_publish(t, buf, 0, true);
    }

    /* миграция: служебный топик <base>/imp<N>/state больше не нужен, очищаем (один раз за запуск) */
    if (!s_legacy_cleared[ch]) {
        s_legacy_cleared[ch] = true;
        chan_topic_id(LEGACY_PULSES_ID, ch, tid, sizeof(tid));
        snprintf(t, sizeof(t), "%s/%s/state", mqtt_mgr_base_topic(), tid);
        mqtt_mgr_publish(t, "", 1, true);
    }
}

static void publish_all_states(void) {
    char v[48];
    for (size_t i = 0; i < GENERAL_ENTITY_COUNT; i++) {
        const char *const *e = GENERAL_ENTITIES[i];
        if (is_set_energy(e[2])) continue;                 /* состояние не публикуется */
        if (get_general(e[2], v, sizeof(v))) pub_state(e[2], v, is_number(e));
    }
    publish_rssi_attributes();
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) publish_channel(ch);
}

/* ---------- Discovery ---------- */

static const number_limits_t *find_limits(const char *id) {
    for (size_t i = 0; i < sizeof(NUMBER_LIMITS) / sizeof(NUMBER_LIMITS[0]); i++) {
        if (!strcmp(NUMBER_LIMITS[i].id, id)) return &NUMBER_LIMITS[i];
    }
    return NULL;
}

/* блок "device" в конце JSON discovery */
static void add_device(sb_t *s) {
    const settings_t *c = settings_get();
    sb_add(s, "\"device\":{\"identifiers\":[\"");
    sb_esc(s, c->hostname);
    sb_add(s, "\"],");
    sb_kv(s, "name", c->hostname);
    sb_kv(s, "manufacturer", MANUFACTURER);
    char model[40];
    snprintf(model, sizeof(model), "%s %s", BRAND_NAME, MODEL_NAMES[MODEL]);
    sb_kv(s, "model", model);
    sb_kv(s, "sw_version", FIRMWARE_VERSION);
    char ip[20];
    if (wifi_mgr_ip(ip, sizeof(ip)) == ESP_OK) {
        char url[40];
        snprintf(url, sizeof(url), "http://%s", ip);
        sb_kv(s, "configuration_url", url);
    }
    sb_kv(s, "hw_version", HARDWARE_VERSION);
    sb_trim_comma(s);
    sb_add(s, "}}");
}

static void publish_discovery(const char *const *e, int ch) {
    const settings_t *c = settings_get();
    const char *type = e[0], *name = e[1], *id = e[2], *scls = e[3], *dcls = e[4];
    const char *unit = e[5], *cat = e[6], *icon = e[7];
    const char *base = mqtt_mgr_base_topic();

    char tid[40], full_name[64], uid[96], topic[200], buf[1100];
    if (ch >= 0) {
        chan_topic_id(id, ch, tid, sizeof(tid));
        snprintf(full_name, sizeof(full_name), "%s %s", CHANNEL_NAMES[ch], name);
    } else {
        strlcpy(tid, id, sizeof(tid));
        strlcpy(full_name, name, sizeof(full_name));
    }
    snprintf(uid, sizeof(uid), "%s_%s", c->hostname, tid);
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", c->mqtt_discovery_topic, type, c->hostname, tid);

    char t[256];
    bool no_state = ch < 0 && is_set_energy(id);        /* команда без состояния */
    sb_t s = { buf, sizeof(buf), 0 };
    sb_add(&s, "{");
    sb_kv(&s, "name", full_name);
    sb_kv(&s, "unique_id", uid);
    if (!no_state) {
        snprintf(t, sizeof(t), "%s/%s/state", base, tid);
        sb_kv(&s, "state_topic", t);
    } else {
        sb_add(&s, "\"optimistic\":true,");             /* HA показывает последнее введённое значение */
    }
    snprintf(t, sizeof(t), "%s/status", base);
    sb_kv(&s, "availability_topic", t);
    if (ch >= 0) {                               /* основной сенсор канала: атрибуты = остальные записи CHANNEL_ENTITIES */
        chan_attr_topic(ch, t, sizeof(t));
        sb_kv(&s, "json_attributes_topic", t);
    } else if (!strcmp(id, i_rssi)) {            /* ip, mac, chip_id - атрибуты RSSI */
        snprintf(t, sizeof(t), "%s/%s/attributes", base, id);
        sb_kv(&s, "json_attributes_topic", t);
    }
    sb_kv(&s, "device_class", dcls);
    sb_kv(&s, "state_class", scls);
    sb_kv(&s, "unit_of_measurement", unit);
    sb_kv(&s, "entity_category", cat);
    sb_kv(&s, "icon", icon);
    if (ch < 0 && !strcmp(id, i_energy)) sb_add(&s, "\"suggested_display_precision\":2,");
    if (ch >= 0 && !strcmp(id, i_energy0)) sb_add(&s, "\"suggested_display_precision\":2,");
    if (is_number(e)) {
        snprintf(t, sizeof(t), "%s/%s/set", base, tid);
        sb_kv(&s, "command_topic", t);
        const number_limits_t *lim = find_limits(id);
        if (lim) sb_add(&s, "\"min\":%.2f,\"max\":%.2f,\"step\":%.2f,", (double)lim->min, (double)lim->max, (double)lim->step);
        sb_add(&s, "\"mode\":\"box\",");
    }
    add_device(&s);
    if (s.len >= s.cap) {
        ESP_LOGW(TAG, "discovery payload too long: %s", tid);
        return;
    }
    mqtt_mgr_publish(topic, buf, 1, true);
}

/* Удаление того, чего больше нет: пустой retained-payload.
   Discovery: записи CHANNEL_ENTITIES[1..] (теперь атрибуты), прежние current/power, ip/mac/chip_id (атрибуты RSSI).
   Состояния: retained <base>/set_energy1|2/state от прежних версий.
   Прежний статус <mqtt_topic>/<hostname>/status (до перехода на номер чипа). */
static void remove_obsolete_discovery(void) {
    static const char *const old_general[] = { "ip", "mac", "chip_id" };
    static const char *const old_chan[] = { "current", "power" };
    const settings_t *c = settings_get();
    char topic[200], tid[40];
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        for (size_t j = 1; j < CHANNEL_ENTITY_COUNT; j++) {      /* записи-атрибуты больше не сенсоры */
            chan_topic_id(CHANNEL_ENTITIES[j][2], ch, tid, sizeof(tid));
            snprintf(topic, sizeof(topic), "%s/sensor/%s/%s/config", c->mqtt_discovery_topic, c->hostname, tid);
            mqtt_mgr_publish(topic, "", 1, true);
        }
        for (size_t i = 0; i < sizeof(old_chan) / sizeof(old_chan[0]); i++) {
            snprintf(topic, sizeof(topic), "%s/sensor/%s/%s%d/config", c->mqtt_discovery_topic, c->hostname, old_chan[i], ch);
            mqtt_mgr_publish(topic, "", 1, true);
        }
    }
    for (size_t i = 0; i < sizeof(old_general) / sizeof(old_general[0]); i++) {
        snprintf(topic, sizeof(topic), "%s/sensor/%s/%s/config", c->mqtt_discovery_topic, c->hostname, old_general[i]);
        mqtt_mgr_publish(topic, "", 1, true);
    }
    snprintf(topic, sizeof(topic), "%s/%s/state", mqtt_mgr_base_topic(), i_e1);
    mqtt_mgr_publish(topic, "", 1, true);
    snprintf(topic, sizeof(topic), "%s/%s/state", mqtt_mgr_base_topic(), i_e2);
    mqtt_mgr_publish(topic, "", 1, true);
    if (strcmp(mqtt_mgr_old_base_topic(), mqtt_mgr_base_topic()) != 0) {
        snprintf(topic, sizeof(topic), "%s/status", mqtt_mgr_old_base_topic());
        mqtt_mgr_publish(topic, "", 1, true);
    }
}

/* ---------- OTA: кнопка запуска сервера обновления и его состояние ---------- */

#define OTA_ID "ota"        /* команда: <base>/ota/set = PRESS; состояние: <base>/ota/state, <base>/ota/attributes */

static void publish_ota_discovery(void) {
    const settings_t *c = settings_get();
    const char *base = mqtt_mgr_base_topic();
    char topic[200], t[256], uid[96], buf[1100];

    /* кнопка */
    snprintf(topic, sizeof(topic), "%s/button/%s/%s/config", c->mqtt_discovery_topic, c->hostname, OTA_ID);
    snprintf(uid, sizeof(uid), "%s_%s", c->hostname, OTA_ID);
    sb_t s = { buf, sizeof(buf), 0 };
    sb_add(&s, "{");
    sb_kv(&s, "name", "Firmware update");
    sb_kv(&s, "unique_id", uid);
    snprintf(t, sizeof(t), "%s/%s/set", base, OTA_ID);
    sb_kv(&s, "command_topic", t);
    sb_kv(&s, "payload_press", "PRESS");
    snprintf(t, sizeof(t), "%s/status", base);
    sb_kv(&s, "availability_topic", t);
    sb_kv(&s, "entity_category", "config");
    sb_kv(&s, "icon", "mdi:cloud-upload");
    add_device(&s);
    if (s.len >= s.cap) ESP_LOGW(TAG, "ota button discovery too long");
    else mqtt_mgr_publish(topic, buf, 1, true);

    /* состояние сервера: ON/OFF, в атрибутах - ссылка и PIN */
    snprintf(topic, sizeof(topic), "%s/binary_sensor/%s/%s_server/config", c->mqtt_discovery_topic, c->hostname, OTA_ID);
    snprintf(uid, sizeof(uid), "%s_%s_server", c->hostname, OTA_ID);
    s = (sb_t){ buf, sizeof(buf), 0 };
    sb_add(&s, "{");
    sb_kv(&s, "name", "Firmware update server");
    sb_kv(&s, "unique_id", uid);
    snprintf(t, sizeof(t), "%s/%s/state", base, OTA_ID);
    sb_kv(&s, "state_topic", t);
    snprintf(t, sizeof(t), "%s/%s/attributes", base, OTA_ID);
    sb_kv(&s, "json_attributes_topic", t);
    sb_add(&s, "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",");
    snprintf(t, sizeof(t), "%s/status", base);
    sb_kv(&s, "availability_topic", t);
    sb_kv(&s, "entity_category", "diagnostic");
    sb_kv(&s, "icon", "mdi:server-network");
    add_device(&s);
    if (s.len >= s.cap) ESP_LOGW(TAG, "ota server discovery too long");
    else mqtt_mgr_publish(topic, buf, 1, true);
}

static void publish_ota_state(void) {
    const char *base = mqtt_mgr_base_topic();
    bool on = ota_update_is_active();
    char t[256], v[160], ip[20];
    snprintf(t, sizeof(t), "%s/%s/state", base, OTA_ID);
    mqtt_mgr_publish(t, on ? "ON" : "OFF", 1, true);
    if (on && wifi_mgr_ip(ip, sizeof(ip)) == ESP_OK) {
        snprintf(v, sizeof(v), "{\"url\":\"http://%s/update\",\"user\":\"%s\",\"pin\":\"%s\"}", ip, OTA_USER, ota_update_pin());
    } else {
        strlcpy(v, "{}", sizeof(v));
    }
    snprintf(t, sizeof(t), "%s/%s/attributes", base, OTA_ID);
    mqtt_mgr_publish(t, v, 0, false);       /* PIN не retained */
}

static void on_ota_state(bool active) {
    (void)active;
    publish_ota_state();
}

static void handle_ota_command(const char *val) {
    if (strcmp(val, "PRESS") != 0) return;
    esp_err_t e = ota_update_start();
    ESP_LOGI(TAG, "ota start -> %s", esp_err_to_name(e));
}

static void publish_all_discovery(void) {
    for (size_t i = 0; i < GENERAL_ENTITY_COUNT; i++) publish_discovery(GENERAL_ENTITIES[i], -1);
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) publish_discovery(CHANNEL_ENTITIES[0], ch);   /* только основной сенсор */
    publish_ota_discovery();
    remove_obsolete_discovery();
}

/* ---------- Команды из HA ---------- */

static bool parse_num(const char *s, double *out) {
    char *end;
    double v = strtod(s, &end);
    if (end == s || !isfinite(v)) return false;
    *out = v;
    return true;
}

static void handle_command(const char *id, const char *val) {
    double v;
    if (!parse_num(val, &v)) {
        ESP_LOGW(TAG, "bad value '%s' for %s", val, id);
        return;
    }
    settings_t s = *settings_get();
    esp_err_t e = ESP_ERR_INVALID_ARG;
    bool force_e1 = false, force_e2 = false;
    double kwh2 = round(v * 100.0) / 100.0;      /* энергия вводится с точностью 0.01 кВт*ч */

    if (is_set_energy(id)) {
        /* коррекция: кВт*ч (2 знака) из HA -> импульсы (round(kwh * coeff)), счёт продолжается от них.
           Состояние "Set energy" не публикуется и больше не трогается до следующей коррекции. */
        bool second = !strcmp(id, i_e2);
        e = pulse_counter_set_kwh(second ? PULSE_CH_E2 : PULSE_CH_E1, kwh2);
        if (e == ESP_OK) { force_e1 = !second; force_e2 = second; }
    } else if (!strcmp(id, i_e0)) {
        if (fabs(kwh2) <= 10000000.0) {
            s.energy_offset = (float)kwh2;
            e = settings_save(&s);
        }
    } else if (!strcmp(id, i_constant)) {
        if (v >= PULSE_COEFF_MIN && v <= PULSE_COEFF_MAX) {
            uint32_t k = (uint32_t)(v + 0.5);
            e = pulse_counter_set_coeff(k);     /* импульсы не меняются, пересчитывается только энергия */
            if (e == ESP_OK) {
                s.coeff = k;
                e = settings_save(&s);
                force_e1 = force_e2 = true;
            }
        }
    } else if (!strcmp(id, i_mqtt_per)) {
        if (v >= 60 && v <= 3600) {
            s.pzem_period_s = (uint16_t)(v + 0.5);
            e = settings_save(&s);
            if (e == ESP_OK) pzem_poll_now();   /* применить новый период сразу */
        }
    } else if (!strcmp(id, i_stat_per)) {
        if (v >= STATS_PERIOD_MIN_S && v <= STATS_PERIOD_MAX_S) {
            s.stats_period_s = (uint32_t)(v + 0.5);
            e = settings_save(&s);
        }
    } else {
        return;
    }
    ESP_LOGI(TAG, "command %s=%s -> %s", id, val, esp_err_to_name(e));

    char out[40];
    if (!is_set_energy(id) && get_general(id, out, sizeof(out))) pub_state(id, out, true);   /* вернуть фактическое значение в HA */
    if (force_e1) publish_channel(0);       /* ручное изменение публикуем сразу, не дожидаясь сотой доли */
    if (force_e2) publish_channel(1);
}

static void restore_pulses(int ch, const char *digits) {
    char *end;
    unsigned long long pulses = strtoull(digits, &end, 10);
    if (end == digits || pulses > UINT32_MAX) return;
    /* применяется один раз за запуск; повторная доставка вернёт ESP_ERR_INVALID_STATE */
    if (pulse_counter_restore((pulse_ch_t)ch, (uint32_t)pulses) == ESP_OK) {
        ESP_LOGI(TAG, "E%d restored from MQTT: %llu pulses", ch + 1, pulses);
    }
}

static void on_message(const char *topic, const char *data, int len) {
    /* миграция: атрибуты в прежней иерархии <mqtt_topic>/<hostname>/... - берём импульсы и очищаем топик */
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        char t[256];
        old_attr_topic(ch, t, sizeof(t));
        if (strcmp(topic, t) == 0) {
            const char *p = strstr(data, "\"" PULSES_KEY "\":");
            if (p) {
                restore_pulses(ch, p + strlen("\"" PULSES_KEY "\":"));
                mqtt_mgr_publish(t, "", 1, true);
            }
            return;
        }
    }

    const char *base = mqtt_mgr_base_topic();
    size_t bl = strlen(base);
    if (strncmp(topic, base, bl) != 0 || topic[bl] != '/') return;
    const char *rest = topic + bl + 1;

    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        char tid[40], want[64];

        /* основной путь: атрибуты основного сенсора канала, ключ "imp" */
        chan_topic_id(CHANNEL_ENTITIES[0][2], ch, tid, sizeof(tid));
        snprintf(want, sizeof(want), "%s/attributes", tid);
        if (strcmp(rest, want) == 0) {
            const char *p = strstr(data, "\"" PULSES_KEY "\":");
            if (p) restore_pulses(ch, p + strlen("\"" PULSES_KEY "\":"));
            return;
        }
        /* миграция: служебный топик <base>/imp<N>/state */
        chan_topic_id(LEGACY_PULSES_ID, ch, tid, sizeof(tid));
        snprintf(want, sizeof(want), "%s/state", tid);
        if (strcmp(rest, want) == 0) {
            restore_pulses(ch, data);
            return;
        }
    }

    const char *slash = strchr(rest, '/');
    if (!slash || strcmp(slash, "/set") != 0) return;
    char id[40];
    size_t il = (size_t)(slash - rest);
    if (il == 0 || il >= sizeof(id)) return;
    memcpy(id, rest, il);
    id[il] = '\0';
    if (strcmp(id, OTA_ID) == 0) {
        handle_ota_command(data);
        return;
    }
    handle_command(id, data);
}

static void on_connected(void) {
    char t[256];
    snprintf(t, sizeof(t), "%s/+/set", mqtt_mgr_base_topic());
    mqtt_mgr_subscribe(t, 1);
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        char tid[40];
        chan_attr_topic(ch, t, sizeof(t));
        mqtt_mgr_subscribe(t, 1);
        chan_topic_id(LEGACY_PULSES_ID, ch, tid, sizeof(tid));
        snprintf(t, sizeof(t), "%s/%s/state", mqtt_mgr_base_topic(), tid);
        mqtt_mgr_subscribe(t, 1);
        if (strcmp(mqtt_mgr_old_base_topic(), mqtt_mgr_base_topic()) != 0) {
            old_attr_topic(ch, t, sizeof(t));       /* перенос накопленных импульсов из прежней иерархии */
            mqtt_mgr_subscribe(t, 1);
        }
    }
    if (settings_get()->mqtt_auto_discovery) publish_all_discovery();
    publish_all_states();   /* каналы без восстановленного значения пропускаются */
    publish_ota_state();
}

/* ---------- Счётчик импульсов и периодическая публикация ---------- */

/* Публикуем не на каждый импульс, а когда меняется сотая доля кВт*ч (coeff/100 импульсов).
   Остальное уходит по периоду pzem_period_s (ha_task). */
static void on_pulse(pulse_ch_t ch, const pulse_counter_data_t *d, void *arg) {
    if (!d->restored || !mqtt_mgr_is_connected()) return;
    if (s_pub_have[ch] && energy_bucket(d->total) == s_pub_bucket[ch]) return;
    publish_channel((int)ch);
}

static void ha_task(void *arg) {
    for (;;) {
        uint32_t p = settings_get()->pzem_period_s;
        vTaskDelay(pdMS_TO_TICKS(p * 1000UL));
        if (mqtt_mgr_is_connected()) publish_all_states();
    }
}

esp_err_t ha_discovery_start(void) {
    mqtt_mgr_set_callbacks(on_connected, on_message);
    if (settings_get()->mqtt_host[0] == '\0') return ESP_OK;
    pulse_counter_set_callback(on_pulse, NULL);
    ota_update_set_state_cb(on_ota_state);
    return xTaskCreate(ha_task, "ha", 5120, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
 
 
