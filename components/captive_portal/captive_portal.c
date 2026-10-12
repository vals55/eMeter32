// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "captive_portal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "settings.h"
#include "portal_page.h"
#include "wifi_mgr.h"
#include "driver/gpio.h"

/* Встроенный светодиод платы (D1 mini32: GPIO2, горит при 1) */
#ifndef PORTAL_LED_GPIO
#define PORTAL_LED_GPIO      GPIO_NUM_2
#endif
#ifndef PORTAL_LED_ON_LEVEL
#define PORTAL_LED_ON_LEVEL  1
#endif
#define PORTAL_LED_ON_US     50000     /* вспышка 50 мс */
#define PORTAL_LED_PERIOD_US 1000000   /* период 1 с */

#ifndef WIFI_TX_POWER_AP
#define WIFI_TX_POWER_AP 44      /* 11 дБм, единицы по 0,25 дБм */
#endif

static const char *TAG = "portal";

#define BODY_MAX     3072
#define SCAN_MAX     20

static bool s_started;
static esp_ip4_addr_t s_ap_ip;

/* ---------- DNS: на любой A-запрос отвечаем адресом точки доступа ---------- */

static void dns_task(void *arg) {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s < 0) {
        ESP_LOGE(TAG, "dns socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        close(s);
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 17) continue;
        if (buf[2] & 0x80) continue;                       /* это ответ, не запрос */
        if (((buf[4] << 8) | buf[5]) != 1) continue;       /* ожидаем один вопрос */
        int p = 12;
        while (p < n && buf[p] != 0) {
            if (buf[p] & 0xC0) { p = n; break; }
            p += buf[p] + 1;
        }
        if (p + 5 > n) continue;
        p++;                                               /* завершающий 0 имени */
        uint16_t qtype = (uint16_t)((buf[p] << 8) | buf[p + 1]);
        int len = p + 4;
        buf[2] = 0x81;
        buf[3] = 0x80;
        buf[6] = buf[7] = buf[8] = buf[9] = buf[10] = buf[11] = 0;
        if (qtype == 1) {
            buf[7] = 1;
            buf[len++] = 0xC0; buf[len++] = 0x0C;          /* указатель на имя */
            buf[len++] = 0; buf[len++] = 1;                /* тип A */
            buf[len++] = 0; buf[len++] = 1;                /* класс IN */
            buf[len++] = 0; buf[len++] = 0; buf[len++] = 0; buf[len++] = 60;
            buf[len++] = 0; buf[len++] = 4;
            memcpy(&buf[len], &s_ap_ip.addr, 4);
            len += 4;
        }
        sendto(s, buf, len, 0, (struct sockaddr *)&from, fl);
    }
}

/* ---------- Вспомогательные функции HTML ---------- */

static void send_str(httpd_req_t *r, const char *s) {
    httpd_resp_sendstr_chunk(r, s);
}

static void send_esc(httpd_req_t *r, const char *s) {
    char buf[128];
    size_t n = 0;
    for (; *s; s++) {
        const char *rep = NULL;
        switch (*s) {
            case '&':  rep = "&amp;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
            default: break;
        }
        size_t rl = rep ? strlen(rep) : 1;
        if (n + rl >= sizeof(buf)) {
            buf[n] = 0;
            send_str(r, buf);
            n = 0;
        }
        if (rep) {
            memcpy(buf + n, rep, rl);
        } else {
            buf[n] = *s;
        }
        n += rl;
    }
    buf[n] = 0;
    if (n) send_str(r, buf);
}

static void fld(httpd_req_t *r, const char *label, const char *name, const char *type,
                const char *val, const char *attrs) {
    send_str(r, "<label>");
    send_str(r, label);
    send_str(r, "<input type='");
    send_str(r, type);
    send_str(r, "' name='");
    send_str(r, name);
    send_str(r, "' id='");
    send_str(r, name);
    send_str(r, "' value='");
    send_esc(r, val);
    send_str(r, "' ");
    send_str(r, attrs);
    send_str(r, "></label>");
}

/* Значки "показать/скрыть пароль": контурные, серые (currentColor), без фона.
   Видны либо глаз (пароль скрыт), либо перечёркнутый глаз (пароль виден) - см. CSS .eye / .sh */
#define SVG_OPEN "<svg viewBox='0 0 24 24' width='22' height='22' fill='none' stroke='currentColor' " \
                 "stroke-width='1' stroke-linecap='round' stroke-linejoin='round'"
#define SVG_EYE  "<path d='M1 12s4-7 11-7 11 7 11 7-4 7-11 7S1 12 1 12z'/><circle cx='12' cy='12' r='3'/>"
static const char EYE_ICONS[] =
    SVG_OPEN " class='on'>" SVG_EYE "</svg>"
    SVG_OPEN " class='off'>" SVG_EYE "<path d='M3 3l18 18'/></svg>";

/* Поле пароля с кнопкой-значком внутри поля. Показывает/скрывает только то, что вводится;
   сохранённые пароли устройство не отдаёт (в поле всегда пусто, как и раньше). */
static void fld_pw(httpd_req_t *r, const char *label, const char *name, const char *attrs) {
    send_str(r, "<label>");
    send_str(r, label);
    send_str(r, "<span class='pw'><input type='password' name='");
    send_str(r, name);
    send_str(r, "' id='");
    send_str(r, name);
    send_str(r, "' value='' ");
    send_str(r, attrs);
    send_str(r, "><button type='button' class='eye' title='Показать/скрыть пароль' data-f='");
    send_str(r, name);
    send_str(r, "' onclick='eye(this)'>");
    send_str(r, EYE_ICONS);
    send_str(r, "</button></span></label>");
}

/* ---------- Обработчики HTTP ---------- */

static esp_err_t root_get(httpd_req_t *r) {
    const settings_t *c = settings_get();
    char buf[48];

    httpd_resp_set_type(r, "text/html; charset=utf-8");
    send_str(r, PAGE_HEAD);
    send_esc(r, BRAND_NAME);
    send_str(r, " — настройки</h1><p>firmware: ");
    send_esc(r, FIRMWARE_VERSION);
    send_str(r, "</p><form method='post' action='/save'>");

    send_str(r, "<fieldset><legend>Wi-Fi</legend>"
                "<button type='button' id='sb' onclick='scan()'>Сканировать сети</button>"
                "<div class='dd' id='dd'>"
                "<button type='button' class='ddb' id='ddb' onclick='tgl()'>Выберите сеть</button>"
                "<div class='ddl' id='nets'></div></div>");
    fld(r, "SSID", "ssid", "text", c->wifi_ssid, "maxlength='32' required");
    fld_pw(r, "Пароль (пусто = не менять, '-' = очистить)", "pass",
           c->wifi_pass[0] ? "placeholder='(задан)' maxlength='63'" : "placeholder='(не задан)' maxlength='63'");
    if (c->wifi_bssid_set) {
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", c->wifi_bssid[0], c->wifi_bssid[1],
                 c->wifi_bssid[2], c->wifi_bssid[3], c->wifi_bssid[4], c->wifi_bssid[5]);
    } else {
        buf[0] = 0;
    }
    fld(r, "BSSID (пусто = любой роутер)", "bssid", "text", buf,
        "placeholder='AA:BB:CC:DD:EE:FF' maxlength='17'");
    fld(r, "Имя хоста", "hostname", "text", c->hostname, "maxlength='31' required");
    send_str(r, "</fieldset>");

    snprintf(buf, sizeof(buf), "%u", (unsigned)c->mqtt_port);
    send_str(r, "<fieldset><legend>MQTT (пустой адрес = отключено)</legend>");
    fld(r, "Адрес брокера", "mhost", "text", c->mqtt_host, "maxlength='63' placeholder='192.168.10.1'");
    fld(r, "Порт (по умолчанию 1883)", "mport", "number", buf, "min='1' max='65535'");
    fld(r, "Логин", "muser", "text", c->mqtt_user, "maxlength='31'");
    fld_pw(r, "Пароль (пусто = не менять, '-' = очистить)", "mpass",
           c->mqtt_pass[0] ? "placeholder='(задан)' maxlength='31'" : "placeholder='(не задан)' maxlength='31'");
    fld(r, "Топик (префикс)", "topic", "text", c->mqtt_topic, "maxlength='63'");
    fld(r, "Топик Discovery", "dtopic", "text", c->mqtt_discovery_topic, "maxlength='63'");
    send_str(r, "<label><input type='checkbox' name='disc' value='1'");
    if (c->mqtt_auto_discovery) send_str(r, " checked");
    send_str(r, "> Автообнаружение Home Assistant</label></fieldset>");

    snprintf(buf, sizeof(buf), "%d", c->tz);
    send_str(r, "<fieldset><legend>Время</legend>");
    fld(r, "NTP-сервер", "ntp", "text", c->ntp_server, "maxlength='63'");
    fld(r, "Часовой пояс (часы от UTC, -12..14)", "tz", "number", buf, "min='-12' max='14'");
    send_str(r, "</fieldset>");

    send_str(r, "<fieldset><legend>Счётчик энергии</legend>");
    snprintf(buf, sizeof(buf), "%.2f", (double)c->initial_E1);
    fld(r, "Начальное значение E1, кВт·ч", "e1", "number", buf, "min='0' step='0.01'");
    snprintf(buf, sizeof(buf), "%.2f", (double)c->initial_E2);
    fld(r, "Начальное значение E2, кВт·ч", "e2", "number", buf, "min='0' step='0.01'");
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)c->coeff);
    fld(r, "Коэффициент, импульсов на кВт·ч", "coeff", "number", buf, "min='1'");
    send_str(r, "</fieldset>");

    snprintf(buf, sizeof(buf), "%u", (unsigned)c->pzem_period_s);
    send_str(r, "<fieldset><legend>Период опроса сенсоров тока</legend>");
    fld(r, "Период, секунд (60..3600)", "pzem", "number", buf, "min='60' max='3600'");
    send_str(r, "</fieldset>");

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)c->stats_period_s);
    send_str(r, "<fieldset><legend>Статистика (http, json) (пустой URI = отключено)</legend>");
    fld(r, "URI сервера статистики", "suri", "text", c->stats_uri,
        "maxlength='127' placeholder='http://192.168.1.10:8080/stats'");
    fld(r, "Период отправки, секунд (60..86400)", "sper", "number", buf, "min='60' max='86400'");
    send_str(r, "</fieldset>");

    send_str(r, "<button type='submit'>Сохранить и перезагрузить</button></form>");
    send_str(r, PAGE_TAIL);
    return httpd_resp_sendstr_chunk(r, NULL);
}

static void json_esc(const char *s, char *out, size_t ol) {
    size_t o = 0;
    for (; *s && o + 7 < ol; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') {
            out[o++] = '\\';
            out[o++] = (char)ch;
        } else if (ch < 0x20) {
            o += (size_t)snprintf(out + o, ol - o, "\\u%04x", ch);
        } else {
            out[o++] = (char)ch;
        }
    }
    out[o] = 0;
}

static esp_err_t scan_get(httpd_req_t *r) {
    httpd_resp_set_type(r, "application/json");
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        return httpd_resp_sendstr(r, "[]");
    }
    wifi_ap_record_t *recs = calloc(SCAN_MAX, sizeof(wifi_ap_record_t));
    if (!recs) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    uint16_t n = SCAN_MAX;
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) n = 0;

    send_str(r, "[");
    bool first = true;
    for (uint16_t i = 0; i < n; i++) {
        if (recs[i].ssid[0] == 0) continue;
        char ss[200];
        char line[340];
        json_esc((const char *)recs[i].ssid, ss, sizeof(ss));
        snprintf(line, sizeof(line),
                 "%s{\"ssid\":\"%s\",\"bssid\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"rssi\":%d,\"ch\":%d,\"auth\":%d}",
                 first ? "" : ",", ss, recs[i].bssid[0], recs[i].bssid[1], recs[i].bssid[2],
                 recs[i].bssid[3], recs[i].bssid[4], recs[i].bssid[5], recs[i].rssi, recs[i].primary,
                 (int)recs[i].authmode);
        send_str(r, line);
        first = false;
    }
    free(recs);
    send_str(r, "]");
    return httpd_resp_sendstr_chunk(r, NULL);
}

/* ---------- Разбор формы ---------- */

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Сырое значение поля формы (с декодированием %XX и +), без обрезки пробелов */
static bool form_get(const char *body, const char *key, char *out, size_t outlen) {
    size_t kl = strlen(key);
    const char *p = body;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > kl && p[kl] == '=' && strncmp(p, key, kl) == 0) {
            const char *v = p + kl + 1;
            const char *end = p + seg;
            size_t o = 0;
            while (v < end && o + 1 < outlen) {
                char c = *v;
                if (c == '+') {
                    c = ' ';
                    v++;
                } else if (c == '%' && v + 2 < end && hexv(v[1]) >= 0 && hexv(v[2]) >= 0) {
                    c = (char)((hexv(v[1]) << 4) | hexv(v[2]));
                    v += 3;
                } else {
                    v++;
                }
                out[o++] = c;
            }
            out[o] = 0;
            return true;
        }
        p += seg;
        if (*p == '&') p++;
    }
    return false;
}

/* Убирает пробельные символы в начале и в конце строки */
static void trim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

/* Значение поля с обрезкой пробелов по краям */
static bool form_get_t(const char *body, const char *key, char *out, size_t outlen) {
    if (!form_get(body, key, out, outlen)) return false;
    trim(out);
    return true;
}

/* Текстовое поле: если есть в форме - копируем (с обрезкой пробелов) */
static bool set_str(const char *b, const char *k, char *dst, size_t dl) {
    char t[128];
    if (!form_get_t(b, k, t, sizeof(t))) return true;
    if (strlen(t) >= dl) return false;
    strcpy(dst, t);
    return true;
}

/* Пароль: пробелы значимы, пусто = не менять, '-' = очистить */
static bool set_secret(const char *b, const char *k, char *dst, size_t dl) {
    char t[128];
    if (!form_get(b, k, t, sizeof(t)) || t[0] == 0) return true;
    if (strcmp(t, "-") == 0) {
        dst[0] = 0;
        return true;
    }
    if (strlen(t) >= dl) return false;
    strcpy(dst, t);
    return true;
}

static bool get_long(const char *b, const char *k, long mn, long mx, long *out) {
    char t[32];
    if (!form_get_t(b, k, t, sizeof(t))) return true;
    char *e;
    long v = strtol(t, &e, 10);
    if (t[0] == 0 || *e != 0 || v < mn || v > mx) return false;
    *out = v;
    return true;
}

static bool get_float(const char *b, const char *k, float *out) {
    char t[32];
    if (!form_get_t(b, k, t, sizeof(t))) return true;
    for (char *p = t; *p; p++) {
        if (*p == ',') *p = '.';
    }
    char *e;
    float v = strtof(t, &e);
    if (t[0] == 0 || *e != 0) return false;
    *out = v;
    return true;
}

static bool host_chars_ok(const char *h) {
    for (; *h; h++) {
        if (!isalnum((unsigned char)*h) && *h != '.' && *h != '-' && *h != '_') return false;
    }
    return true;
}

static const char *apply_form(const char *b, settings_t *s) {
    char t[128];

    /* SSID: пробелы внутри и по краям допустимы, поэтому без trim */
    if (!form_get(b, "ssid", t, sizeof(t)) || t[0] == 0) return "Укажите SSID";
    if (strlen(t) >= sizeof(s->wifi_ssid)) return "SSID слишком длинный";
    strcpy(s->wifi_ssid, t);
    if (!set_secret(b, "pass", s->wifi_pass, sizeof(s->wifi_pass))) return "Пароль Wi-Fi слишком длинный";

    if (form_get_t(b, "bssid", t, sizeof(t))) {
        if (t[0] == 0) {
            s->wifi_bssid_set = false;
            memset(s->wifi_bssid, 0, sizeof(s->wifi_bssid));
        } else {
            unsigned v[6];
            if (strlen(t) != 17 ||
                sscanf(t, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
                return "Неверный формат BSSID (нужно AA:BB:CC:DD:EE:FF)";
            }
            for (int i = 0; i < 6; i++) s->wifi_bssid[i] = (uint8_t)v[i];
            s->wifi_bssid_set = true;
        }
    }

    if (form_get_t(b, "hostname", t, sizeof(t))) {
        size_t n = strlen(t);
        if (n == 0 || n >= sizeof(s->hostname)) return "Неверная длина имени хоста";
        for (size_t i = 0; i < n; i++) {
            if (!isalnum((unsigned char)t[i]) && t[i] != '-') {
                return "Имя хоста: только латинские буквы, цифры и дефис";
            }
        }
        strcpy(s->hostname, t);
    }

    /* MQTT: адрес и порт хранятся отдельно */
    if (form_get_t(b, "mhost", t, sizeof(t))) {
        if (strlen(t) >= sizeof(s->mqtt_host)) return "Адрес брокера слишком длинный";
        if (!host_chars_ok(t)) return "Адрес брокера: только имя или IP, без mqtt:// и порта";
        strcpy(s->mqtt_host, t);
    }
    long port = s->mqtt_port;
    if (form_get_t(b, "mport", t, sizeof(t)) && t[0] == 0) {
        port = MQTT_DEFAULT_PORT;                      /* пустой порт = по умолчанию */
    } else if (!get_long(b, "mport", 1, 65535, &port)) {
        return "Порт MQTT: число от 1 до 65535";
    }
    s->mqtt_port = (uint16_t)port;
    if (!set_str(b, "muser", s->mqtt_user, sizeof(s->mqtt_user))) return "Логин MQTT слишком длинный";
    if (!set_secret(b, "mpass", s->mqtt_pass, sizeof(s->mqtt_pass))) return "Пароль MQTT слишком длинный";
    if (!set_str(b, "topic", s->mqtt_topic, sizeof(s->mqtt_topic))) return "Топик слишком длинный";
    if (!set_str(b, "dtopic", s->mqtt_discovery_topic, sizeof(s->mqtt_discovery_topic))) return "Топик Discovery слишком длинный";
    s->mqtt_auto_discovery = form_get(b, "disc", t, sizeof(t)) ? 1 : 0;

    if (!set_str(b, "ntp", s->ntp_server, sizeof(s->ntp_server))) return "Адрес NTP слишком длинный";
    long tz = s->tz;
    if (!get_long(b, "tz", -12, 14, &tz)) return "Часовой пояс: число от -12 до 14";
    s->tz = (int8_t)tz;

    if (!get_float(b, "e1", &s->initial_E1)) return "Неверное значение E1";
    if (!get_float(b, "e2", &s->initial_E2)) return "Неверное значение E2";
    long coeff = (long)s->coeff;
    if (!get_long(b, "coeff", 1, 1000000, &coeff)) return "Коэффициент: число от 1 до 1000000";
    s->coeff = (uint32_t)coeff;
    long pz = s->pzem_period_s;
    if (!get_long(b, "pzem", 60, 3600, &pz)) return "Период опроса: от 60 до 3600 секунд";
    s->pzem_period_s = (uint16_t)pz;

    /* Статистика: пустой URI = выключено */
    char u[STATS_URI_LEN + 1];
    if (form_get_t(b, "suri", u, sizeof(u))) {
        if (strlen(u) >= sizeof(s->stats_uri)) return "URI статистики слишком длинный";
        if (u[0] && strncmp(u, "http://", 7) != 0 && strncmp(u, "https://", 8) != 0) {
            return "URI статистики должен начинаться с http:// или https://";
        }
        strcpy(s->stats_uri, u);
    }
    long sp = (long)s->stats_period_s;
    if (!get_long(b, "sper", STATS_PERIOD_MIN_S, STATS_PERIOD_MAX_S, &sp)) {
        return "Период статистики: от 60 до 86400 секунд";
    }
    s->stats_period_s = (uint32_t)sp;
    return NULL;
}

static esp_err_t send_msg(httpd_req_t *r, const char *status, const char *cls, const char *msg, bool back) {
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    send_str(r, PAGE_HEAD);
    send_esc(r, BRAND_NAME);
    send_str(r, "</h1><p class='");
    send_str(r, cls);
    send_str(r, "'>");
    send_esc(r, msg);
    send_str(r, "</p>");
    if (back) send_str(r, "<p><a href='/'>Назад к настройкам</a></p>");
    send_str(r, "</body></html>");
    return httpd_resp_sendstr_chunk(r, NULL);
}

static void restart_cb(void *arg) {
    esp_restart();
}

static esp_err_t save_post(httpd_req_t *r) {
    size_t total = r->content_len;
    if (total == 0 || total > BODY_MAX) {
        return send_msg(r, "400 Bad Request", "err", "Неверный размер запроса", true);
    }
    char *body = malloc(total + 1);
    if (!body) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    size_t got = 0;
    while (got < total) {
        int n = httpd_req_recv(r, body + got, total - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += (size_t)n;
    }
    body[total] = 0;

    settings_t s = *settings_get();
    const char *err = apply_form(body, &s);
    free(body);
    if (err) return send_msg(r, "400 Bad Request", "err", err, true);
    if (settings_save(&s) != ESP_OK) {
        return send_msg(r, "400 Bad Request", "err", "Значения вне допустимого диапазона или ошибка записи", true);
    }

    ESP_LOGI(TAG, "settings saved, restarting");
    esp_timer_handle_t t;
    const esp_timer_create_args_t ta = { .callback = restart_cb, .name = "restart" };
    if (esp_timer_create(&ta, &t) == ESP_OK) esp_timer_start_once(t, 2000000);
    return send_msg(r, "200 OK", "ok", "Настройки сохранены. Устройство перезагружается и подключится к выбранной сети.", false);
}

/* Любой неизвестный адрес (проверки доступа Android/iOS/Windows) -> на главную */
static esp_err_t redirect_404(httpd_req_t *r, httpd_err_code_t err) {
    char loc[40];
    snprintf(loc, sizeof(loc), "http://" IPSTR "/", IP2STR(&s_ap_ip));
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", loc);
    return httpd_resp_send(r, NULL, 0);
}

/* ---------- Маячок: вспышка 50 мс раз в секунду, пока работает портал ---------- */

static esp_timer_handle_t s_led_timer;
static bool s_led_on;

static void led_timer_cb(void *arg) {
    s_led_on = !s_led_on;
    gpio_set_level(PORTAL_LED_GPIO, s_led_on ? PORTAL_LED_ON_LEVEL : !PORTAL_LED_ON_LEVEL);
    esp_timer_start_once(s_led_timer,
                         s_led_on ? PORTAL_LED_ON_US : (PORTAL_LED_PERIOD_US - PORTAL_LED_ON_US));
}

static void led_beacon_start(void) {
    gpio_reset_pin(PORTAL_LED_GPIO);
    gpio_set_direction(PORTAL_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(PORTAL_LED_GPIO, !PORTAL_LED_ON_LEVEL);

    const esp_timer_create_args_t ta = { .callback = led_timer_cb, .name = "portal_led" };
    if (esp_timer_create(&ta, &s_led_timer) == ESP_OK) {
        s_led_on = false;
        esp_timer_start_once(s_led_timer, 1000);   /* первая вспышка почти сразу */
    } else {
        ESP_LOGW(TAG, "led timer create failed");
    }
}

static esp_err_t favicon_get(httpd_req_t *r) {
    httpd_resp_set_status(r, "204 No Content");
    return httpd_resp_send(r, NULL, 0);
}

/* ---------- Запуск ---------- */

static void start_task(void *arg) {
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    wifi_config_t ac = { 0 };
    strlcpy((char *)ac.ap.ssid, settings_get()->hostname, sizeof(ac.ap.ssid));
    ac.ap.ssid_len = (uint8_t)strlen((char *)ac.ap.ssid);
    ac.ap.channel = 1;
    ac.ap.authmode = WIFI_AUTH_OPEN;
    ac.ap.max_connection = 4;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ac));
    esp_err_t we = esp_wifi_start();
    if (we != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_start: %s", esp_err_to_name(we));
    }
    esp_wifi_set_max_tx_power(WIFI_TX_POWER_AP);

    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(ap, &ip);
    s_ap_ip = ip.ip;
    ESP_LOGI(TAG, "AP '%s' started, IP " IPSTR, ac.ap.ssid, IP2STR(&s_ap_ip));
    
    led_beacon_start();
    
    xTaskCreate(dns_task, "dns", 4096, NULL, 5, NULL);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;
    httpd_handle_t srv = NULL;
    
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);   /* не засорять лог запросами проверки сети */
    
    if (httpd_start(&srv, &cfg) == ESP_OK) {
        const httpd_uri_t u_root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
        const httpd_uri_t u_scan = { .uri = "/scan", .method = HTTP_GET, .handler = scan_get };
        const httpd_uri_t u_save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
        const httpd_uri_t u_fav = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get };
        httpd_register_uri_handler(srv, &u_fav);
        httpd_register_uri_handler(srv, &u_root);
        httpd_register_uri_handler(srv, &u_scan);
        httpd_register_uri_handler(srv, &u_save);
        httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, redirect_404);
        ESP_LOGI(TAG, "http server started");
    } else {
        ESP_LOGE(TAG, "http server start failed");
    }
    vTaskDelete(NULL);
}

esp_err_t captive_portal_start(void) {
    if (s_started) return ESP_OK;
    s_started = true;
    return xTaskCreate(start_task, "portal_start", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

 
 
