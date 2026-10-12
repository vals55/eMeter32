// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ota_update.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "mbedtls/base64.h"

static const char *TAG = "ota";

#define OTA_RX_CHUNK   4096
#define OTA_FIRST_READ 512          /* заголовок образа + app_desc лежат в первых 288 байтах */
#define OTA_RX_IDLE_MAX 6           /* подряд таймаутов приёма (по 10 с) до отмены */

static SemaphoreHandle_t s_lock;
static httpd_handle_t s_srv;
static esp_timer_handle_t s_timeout;
static esp_timer_handle_t s_reboot;
static ota_update_state_cb_t s_cb;
static char s_pin[8];
static char s_auth[48];             /* ожидаемое значение заголовка Authorization */
static volatile bool s_active;
static volatile bool s_busy;        /* идёт приём образа */
static volatile int s_fails;

/* ---------- Страница ---------- */

#define PAGE_HEAD \
    "<!doctype html><html lang='ru'><meta charset='utf-8'>" \
    "<meta name='viewport' content='width=device-width,initial-scale=1'><title>Обновление прошивки</title>" \
    "<style>body{font-family:system-ui,sans-serif;max-width:480px;margin:20px auto;padding:0 12px;color:#1d2733}" \
    "input,button{width:100%;padding:12px;font-size:1em;margin:8px 0;border-radius:8px;border:1px solid #c5ccd4;box-sizing:border-box}" \
    "button{background:#b6d1f3;border:0}progress{width:100%;height:18px}.s{color:#5f6368;font-size:.9em}</style>" \
    "<h2>Обновление прошивки</h2><p class='s'>Сейчас запущено: "

#define PAGE_TAIL \
    "</p><input type='file' id='f' accept='.bin'>" \
    "<button id='b' onclick='up()'>Загрузить</button>" \
    "<progress id='p' value='0' max='100' hidden></progress><p id='m'></p>" \
    "<script>" \
    "function up(){var f=document.getElementById('f').files[0],m=document.getElementById('m'),p=document.getElementById('p'),b=document.getElementById('b');" \
    "if(!f){m.textContent='Выберите файл firmware.bin';return}" \
    "var x=new XMLHttpRequest();x.open('POST','/update');b.disabled=true;p.hidden=false;m.textContent='';" \
    "x.upload.onprogress=function(e){if(e.lengthComputable)p.value=100*e.loaded/e.total};" \
    "x.onload=function(){if(x.status==200){p.value=100;m.textContent='Готово, устройство перезагружается...'}" \
    "else{b.disabled=false;m.textContent='Ошибка: '+x.responseText}};" \
    "x.onerror=function(){b.disabled=false;m.textContent='Связь прервана'};" \
    "x.send(f)}" \
    "</script></html>"

/* ---------- Авторизация ---------- */

static bool check_auth(httpd_req_t *r) {
    char hdr[80];
    if (httpd_req_get_hdr_value_str(r, "Authorization", hdr, sizeof(hdr)) == ESP_OK) {
        size_t n = strlen(s_auth);
        unsigned diff = (strlen(hdr) != n);
        for (size_t i = 0; i < n && hdr[i]; i++) diff |= (unsigned)(hdr[i] ^ s_auth[i]);   /* без раннего выхода */
        if (!diff) return true;
    }
    if (++s_fails >= OTA_MAX_AUTH_FAILS) {
        ESP_LOGW(TAG, "too many auth failures, stopping");
        esp_timer_stop(s_timeout);
        esp_timer_start_once(s_timeout, 200 * 1000);        /* остановим из таймера: из обработчика httpd_stop нельзя */
    }
    httpd_resp_set_status(r, "401 Unauthorized");
    httpd_resp_set_hdr(r, "WWW-Authenticate", "Basic realm=\"eMeter32 OTA\"");
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    httpd_resp_sendstr(r, "Нужен PIN");
    return false;
}

static esp_err_t page_get(httpd_req_t *r) {
    if (!check_auth(r)) return ESP_OK;
    s_fails = 0;
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_app_desc_t d;
    char info[96] = "";
    if (run && esp_ota_get_partition_description(run, &d) == ESP_OK) {
        snprintf(info, sizeof(info), "%.31s %.31s (раздел %s)", d.project_name, d.version, run->label);
    }
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(r, PAGE_HEAD);
    httpd_resp_sendstr_chunk(r, info);
    httpd_resp_sendstr_chunk(r, PAGE_TAIL);
    return httpd_resp_sendstr_chunk(r, NULL);
}

/* ---------- Приём образа ---------- */

static esp_err_t fail(httpd_req_t *r, const char *status, const char *msg) {
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(r, msg);
}

/* прочитать ровно len байт; 0 = успех */
static int recv_full(httpd_req_t *r, char *buf, size_t len) {
    size_t got = 0;
    int idle = 0;
    while (got < len) {
        int n = httpd_req_recv(r, buf + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++idle >= OTA_RX_IDLE_MAX) return -1;
            continue;
        }
        if (n <= 0) return -1;
        idle = 0;
        got += (size_t)n;
    }
    return 0;
}

static esp_err_t update_post(httpd_req_t *r) {
    if (!check_auth(r)) return ESP_OK;
    s_fails = 0;
    if (s_busy) return fail(r, "409 Conflict", "Загрузка уже идёт");

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return fail(r, "500 Internal Server Error", "Нет раздела для обновления");
    size_t total = r->content_len;
    if (total < OTA_FIRST_READ) return fail(r, "400 Bad Request", "Файл слишком мал");
    if (total > part->size) return fail(r, "413 Payload Too Large", "Файл больше раздела обновления");

    char *buf = malloc(OTA_RX_CHUNK);
    if (!buf) return fail(r, "500 Internal Server Error", "Не хватает памяти");

    s_busy = true;
    const char *err = NULL;
    esp_ota_handle_t h = 0;
    bool begun = false;
    size_t got = 0;

    if (recv_full(r, buf, OTA_FIRST_READ) != 0) {
        err = "Связь прервана";
    } else {
        const esp_image_header_t *ih = (const esp_image_header_t *)buf;
        const esp_app_desc_t *nd = (const esp_app_desc_t *)(buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
        esp_app_desc_t cur;
        const esp_partition_t *run = esp_ota_get_running_partition();
        if (ih->magic != ESP_IMAGE_HEADER_MAGIC || nd->magic_word != ESP_APP_DESC_MAGIC_WORD) {
            err = "Это не образ приложения ESP32 (нужен firmware.bin)";
        } else if (run && esp_ota_get_partition_description(run, &cur) == ESP_OK &&
                   strncmp(nd->project_name, cur.project_name, sizeof(nd->project_name)) != 0) {
            err = "Образ от другого проекта";
        } else {
            ESP_LOGI(TAG, "receiving %s %s, %u bytes -> %s", nd->project_name, nd->version, (unsigned)total, part->label);
            if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
                err = "Не удалось начать запись";
            } else {
                begun = true;
                if (esp_ota_write(h, buf, OTA_FIRST_READ) != ESP_OK) err = "Ошибка записи во flash";
                got = OTA_FIRST_READ;
            }
        }
    }

    int idle = 0;
    while (!err && got < total) {
        size_t want = total - got < OTA_RX_CHUNK ? total - got : OTA_RX_CHUNK;
        int n = httpd_req_recv(r, buf, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++idle >= OTA_RX_IDLE_MAX) err = "Таймаут приёма";
            continue;
        }
        if (n <= 0) { err = "Связь прервана"; break; }
        idle = 0;
        if (esp_ota_write(h, buf, (size_t)n) != ESP_OK) { err = "Ошибка записи во flash"; break; }
        got += (size_t)n;
    }

    if (begun) {
        if (err) {
            esp_ota_abort(h);
        } else {
            esp_err_t e = esp_ota_end(h);            /* проверяет целостность образа */
            if (e != ESP_OK) {
                err = "Образ повреждён";
                ESP_LOGW(TAG, "esp_ota_end: %s", esp_err_to_name(e));
            } else if (esp_ota_set_boot_partition(part) != ESP_OK) {
                err = "Не удалось выбрать новый раздел для загрузки";
            }
        }
    }
    free(buf);
    s_busy = false;

    if (err) {
        ESP_LOGW(TAG, "update failed: %s", err);
        return fail(r, "400 Bad Request", err);
    }
    ESP_LOGI(TAG, "update OK, rebooting");
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    httpd_resp_sendstr(r, "OK");
    esp_timer_start_once(s_reboot, 1500 * 1000);
    return ESP_OK;
}

/* ---------- Запуск и остановка ---------- */

static void notify(void) {
    if (s_cb) s_cb(s_active);
}

static void do_stop(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_active) { xSemaphoreGive(s_lock); return; }
    esp_timer_stop(s_timeout);
    httpd_stop(s_srv);
    s_srv = NULL;
    s_active = false;
    s_pin[0] = '\0';
    s_auth[0] = '\0';
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "server stopped");
    notify();
}

static void timeout_cb(void *arg) {
    if (s_busy) {                                   /* не обрываем идущую загрузку */
        esp_timer_start_once(s_timeout, 30ULL * 1000000ULL);
        return;
    }
    do_stop();
}

static void reboot_cb(void *arg) {
    esp_restart();
}

void ota_update_init(void) {
    s_lock = xSemaphoreCreateMutex();
    const esp_timer_create_args_t t1 = { .callback = timeout_cb, .name = "ota_to" };
    const esp_timer_create_args_t t2 = { .callback = reboot_cb, .name = "ota_rb" };
    esp_timer_create(&t1, &s_timeout);
    esp_timer_create(&t2, &s_reboot);
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run) ESP_LOGI(TAG, "running from %s", run->label);
}

void ota_update_set_state_cb(ota_update_state_cb_t cb) {
    s_cb = cb;
}

bool ota_update_is_active(void) { return s_active; }
const char *ota_update_pin(void) { return s_pin; }

esp_err_t ota_update_start(void) {
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active) {                                 /* повторная команда продлевает сеанс */
        esp_timer_stop(s_timeout);
        esp_timer_start_once(s_timeout, (uint64_t)OTA_SERVER_TIMEOUT_S * 1000000ULL);
        xSemaphoreGive(s_lock);
        notify();
        return ESP_OK;
    }

    snprintf(s_pin, sizeof(s_pin), "%06u", (unsigned)(esp_random() % 1000000U));
    char cred[24], b64[32];
    size_t olen = 0;
    snprintf(cred, sizeof(cred), "%s:%s", OTA_USER, s_pin);
    if (mbedtls_base64_encode((unsigned char *)b64, sizeof(b64) - 1, &olen, (const unsigned char *)cred, strlen(cred)) != 0) {
        s_pin[0] = '\0';
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    b64[olen] = '\0';
    snprintf(s_auth, sizeof(s_auth), "Basic %s", b64);
    s_fails = 0;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 4;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    esp_err_t e = httpd_start(&s_srv, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(e));
        s_srv = NULL;
        s_pin[0] = '\0';
        s_auth[0] = '\0';
        xSemaphoreGive(s_lock);
        return e;
    }
    const httpd_uri_t u_root = { .uri = "/", .method = HTTP_GET, .handler = page_get };
    const httpd_uri_t u_page = { .uri = "/update", .method = HTTP_GET, .handler = page_get };
    const httpd_uri_t u_post = { .uri = "/update", .method = HTTP_POST, .handler = update_post };
    httpd_register_uri_handler(s_srv, &u_root);
    httpd_register_uri_handler(s_srv, &u_page);
    httpd_register_uri_handler(s_srv, &u_post);

    s_active = true;
    esp_timer_start_once(s_timeout, (uint64_t)OTA_SERVER_TIMEOUT_S * 1000000ULL);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "server started: /update, user '%s', PIN %s, %d s", OTA_USER, s_pin, OTA_SERVER_TIMEOUT_S);
    notify();
    return ESP_OK;
}

void ota_update_stop(void) {
    if (!s_lock || s_busy) return;
    do_stop();
}

void ota_update_mark_valid(void) {
    static bool done;
    if (done) return;
    done = true;
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) ESP_LOGI(TAG, "new firmware confirmed");
    }
}
