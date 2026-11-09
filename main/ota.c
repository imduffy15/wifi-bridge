/*
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 *
 * Firmware updates: streamed upload (web UI / curl) and download from a URL (http or https).
 * Images are checked before anything is written: ESP32 app image, and one of our two projects. The firmware
 * goes to the next OTA slot; the safeboot firmware (devices with a safeboot partition) to the factory slot.
 */
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_image_format.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "wb.h"

static const char *TAG = "ota";

typedef enum { OTA_IDLE, OTA_RUNNING, OTA_DONE, OTA_ERROR } ota_state_t;
static volatile ota_state_t s_state;
static volatile size_t s_bytes, s_total;
static char s_err[128];
static esp_ota_handle_t s_h;
static bool s_h_open;
static const esp_partition_t *s_part;
static ota_kind_t s_kind;
static size_t s_erased;                     /* safeboot partition: bytes erased so far */
static bool s_header_checked;
static void safeboot_invalidate(void);
/* image header + first segment header + app description: enough to tell what the image is */
static uint8_t s_hdr[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)];
static size_t s_hdr_len;
static char s_url[256];

static void fail(char *err, size_t errlen, const char *msg)
{
    strlcpy(s_err, msg, sizeof s_err);
    if (err) {
        strlcpy(err, msg, errlen);
    }
    s_state = OTA_ERROR;
    ESP_LOGE(TAG, "%s", msg);
}

static const esp_partition_t *safeboot_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
}

const char *wb_ota_mode(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next && next != run && next->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY) {
        return "dual";
    }
    if (safeboot_partition()) {
        return "safeboot";
    }
    return "none";
}

/* Whether the safeboot partition holds a complete image: checked once, and again after each install */
static int s_sb_valid = -1;

static bool safeboot_valid(const esp_partition_t *f)
{
    if (s_sb_valid < 0) {
        esp_partition_pos_t pos = { .offset = f->address, .size = f->size };
        esp_image_metadata_t md;
        s_sb_valid = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &md) == ESP_OK;
    }
    return s_sb_valid;
}

bool ota_safeboot_ready(void)
{
    const esp_partition_t *f = safeboot_partition();
    return f && f != esp_ota_get_running_partition() && safeboot_valid(f);
}

void ota_safeboot_json(cJSON *o)
{
    const esp_partition_t *f = safeboot_partition();
    if (!f || f == esp_ota_get_running_partition()) {
        cJSON_AddNullToObject(o, "safeboot");
        return;
    }
    cJSON *s = cJSON_AddObjectToObject(o, "safeboot");
    esp_app_desc_t d;
    bool ok = safeboot_valid(f) && esp_ota_get_partition_description(f, &d) == ESP_OK;
    bool ours = ok && strncmp(d.project_name, WB_SAFEBOOT_PROJECT, sizeof d.project_name) == 0;
    /* ours: our safeboot firmware, which the web UI can drive for one-click updates */
    cJSON_AddBoolToObject(s, "ours", ours);
    if (ok) {
        char v[33];
        strlcpy(v, d.version, sizeof v);
        cJSON_AddStringToObject(s, "version", v);
    } else {
        cJSON_AddNullToObject(s, "version");
    }
    cJSON_AddNumberToObject(s, "size", f->size);
}

esp_err_t ota_begin_upload(size_t total, char *err, size_t errlen)
{
    if (s_state == OTA_RUNNING) {
        strlcpy(err, "an update is already running", errlen);
        return ESP_ERR_INVALID_STATE;
    }
    /* where the image goes is decided from its header (firmware or safeboot firmware) */
    s_err[0] = 0;
    s_bytes = 0;
    s_total = total;
    s_header_checked = false;
    s_hdr_len = 0;
    s_h_open = false;
    s_part = NULL;
    s_kind = OTA_KIND_NONE;
    s_state = OTA_RUNNING;
    ESP_LOGI(TAG, "update started (%u bytes)", (unsigned)total);
    return ESP_OK;
}

/* Check the image and pick its partition. Returns ESP_ERR_NOT_SUPPORTED for a valid image that has to be
 * installed from the safeboot firmware. */
static esp_err_t check_header(char *err, size_t errlen)
{
    const esp_image_header_t *ih = (const esp_image_header_t *)s_hdr;
    const esp_app_desc_t *d = (const esp_app_desc_t *)(s_hdr + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (ih->magic != ESP_IMAGE_HEADER_MAGIC) {
        fail(err, errlen, "not an ESP32 firmware image (wrong magic byte); did you pick the .factory.bin? use the .ota.bin");
        return ESP_ERR_INVALID_ARG;
    }
    if (ih->chip_id != ESP_CHIP_ID_ESP32) {
        fail(err, errlen, "image is for a different chip");
        return ESP_ERR_INVALID_ARG;
    }
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        fail(err, errlen, "image has no app description");
        return ESP_ERR_INVALID_ARG;
    }
    const esp_partition_t *run = esp_ota_get_running_partition();
    char m[128];
    if (strncmp(d->project_name, WB_SAFEBOOT_PROJECT, sizeof d->project_name) == 0) {
        const esp_partition_t *f = safeboot_partition();
        if (!f) {
            fail(err, errlen, "this device has no safeboot partition, so it doesn't use the safeboot firmware");
            return ESP_ERR_INVALID_ARG;
        }
        if (f == run) {
            fail(err, errlen, "the safeboot firmware can't replace itself: restart into WiFi Bridge and install it there");
            return ESP_ERR_NOT_SUPPORTED;
        }
        s_part = f;
        s_kind = OTA_KIND_SAFEBOOT;
    } else if (strncmp(d->project_name, WB_FIRMWARE_PROJECT, sizeof d->project_name) == 0) {
        const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
        if (!next || next == run || next->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
            fail(err, errlen, "the firmware can't overwrite itself on this device: restart into the safeboot firmware to install it");
            return ESP_ERR_NOT_SUPPORTED;
        }
        s_part = next;
        s_kind = OTA_KIND_FIRMWARE;
    } else {
        snprintf(m, sizeof m, "image is \"%.32s\", not WiFi Bridge firmware", d->project_name);
        fail(err, errlen, m);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_total && s_total > s_part->size) {
        snprintf(m, sizeof m, "image (%u bytes) is larger than the %s partition (%u bytes)", (unsigned)s_total,
                 s_part->label, (unsigned)s_part->size);
        fail(err, errlen, m);
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "image: %s %.32s (%s %s) -> %s", d->project_name, d->version, d->date, d->time, s_part->label);
    if (s_kind == OTA_KIND_FIRMWARE) {
        esp_err_t e = esp_ota_begin(s_part, OTA_WITH_SEQUENTIAL_WRITES, &s_h);
        if (e != ESP_OK) {
            fail(err, errlen, esp_err_to_name(e));
            return e;
        }
        s_h_open = true;
    } else {
        s_erased = 0;
        ESP_LOGW(TAG, "replacing the safeboot firmware in %s", s_part->label);
    }
    return ESP_OK;
}

static esp_err_t sink(const void *data, size_t len, char *err, size_t errlen)
{
    esp_err_t e;
    if (s_kind == OTA_KIND_FIRMWARE) {
        e = esp_ota_write(s_h, data, len);
    } else {
        e = ESP_OK;
        while (e == ESP_OK && s_bytes + len > s_erased) {
            if (s_erased >= s_part->size) {
                e = ESP_ERR_INVALID_SIZE;
                break;
            }
            /* 64 KB blocks erase much faster than sectors */
            size_t n = (s_erased % 0x10000 == 0 && s_erased + 0x10000 <= s_part->size) ? 0x10000 : 0x1000;
            e = esp_partition_erase_range(s_part, s_erased, n);
            s_erased += n;
        }
        if (e == ESP_OK) {
            e = esp_partition_write(s_part, s_bytes, data, len);
        }
    }
    if (e != ESP_OK) {
        ota_abort();
        fail(err, errlen, e == ESP_ERR_INVALID_SIZE ? "image is larger than the partition" : esp_err_to_name(e));
        return e;
    }
    s_bytes += len;
    return ESP_OK;
}

esp_err_t ota_write(const void *data, size_t len, char *err, size_t errlen)
{
    if (s_state != OTA_RUNNING) {
        strlcpy(err, s_err[0] ? s_err : "no update running", errlen);
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t *p = data;
    if (!s_header_checked) {
        size_t n = sizeof s_hdr - s_hdr_len < len ? sizeof s_hdr - s_hdr_len : len;
        memcpy(s_hdr + s_hdr_len, p, n);
        s_hdr_len += n;
        p += n;
        len -= n;
        if (s_hdr_len < sizeof s_hdr) {
            return ESP_OK;
        }
        esp_err_t e = check_header(err, errlen);
        if (e != ESP_OK) {
            return e;
        }
        s_header_checked = true;
        e = sink(s_hdr, s_hdr_len, err, errlen);
        if (e != ESP_OK) {
            return e;
        }
    }
    return len ? sink(p, len, err, errlen) : ESP_OK;
}

esp_err_t ota_finish(char *err, size_t errlen)
{
    if (s_state != OTA_RUNNING) {
        strlcpy(err, s_err[0] ? s_err : "no update running", errlen);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_header_checked) {
        fail(err, errlen, "image too short");
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_kind == OTA_KIND_SAFEBOOT) {
        esp_partition_pos_t pos = { .offset = s_part->address, .size = s_part->size };
        esp_image_metadata_t md;
        if (esp_image_verify(ESP_IMAGE_VERIFY, &pos, &md) != ESP_OK) {
            safeboot_invalidate();
            fail(err, errlen, "safeboot image is corrupt (checksum/validation failed); upload it again");
            return ESP_ERR_INVALID_CRC;
        }
        s_sb_valid = 1;
        s_state = OTA_DONE;
        ESP_LOGW(TAG, "safeboot firmware installed (%u bytes in %s)", (unsigned)s_bytes, s_part->label);
        return ESP_OK;
    }
    s_h_open = false;
    esp_err_t e = esp_ota_end(s_h);
    if (e != ESP_OK) {
        fail(err, errlen, e == ESP_ERR_OTA_VALIDATE_FAILED ? "image is corrupt (checksum/validation failed)" : esp_err_to_name(e));
        return e;
    }
    e = esp_ota_set_boot_partition(s_part);
    if (e != ESP_OK) {
        fail(err, errlen, esp_err_to_name(e));
        return e;
    }
    nvs_flag_set("new_fw", 1);         /* rollback applies to this image only (see main.c) */
    s_state = OTA_DONE;
    ESP_LOGI(TAG, "update written (%u bytes), next boot from %s", (unsigned)s_bytes, s_part->label);
    return ESP_OK;
}

ota_kind_t ota_installed_kind(void)
{
    return s_state == OTA_DONE ? s_kind : OTA_KIND_NONE;
}

/* A half-written safeboot image must not look installed: erase its header */
static void safeboot_invalidate(void)
{
    if (s_kind == OTA_KIND_SAFEBOOT && s_part && s_header_checked) {
        esp_partition_erase_range(s_part, 0, 0x1000);
        s_sb_valid = 0;
        ESP_LOGE(TAG, "safeboot install failed: the safeboot partition is empty, install it again");
    }
}

void ota_abort(void)
{
    if (s_state == OTA_RUNNING) {
        safeboot_invalidate();
        if (s_h_open) {
            esp_ota_abort(s_h);
            s_h_open = false;
        }
        fail(NULL, 0, "upload interrupted");
    }
}

/* ---- download from URL ---- */

static void url_task(void *arg)
{
    char err[96] = "";
    esp_http_client_config_t hc = {
        .url = s_url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) {
        fail(NULL, 0, "http client init failed");
        vTaskDelete(NULL);
        return;
    }
    /* follow redirects ourselves: open()/fetch_headers() don't (GitHub release assets always redirect) */
    esp_err_t e;
    int64_t len = -1;
    int status = 0;
    for (int hop = 0; hop < 6; hop++) {
        e = esp_http_client_open(c, 0);
        len = e == ESP_OK ? esp_http_client_fetch_headers(c) : -1;
        status = esp_http_client_get_status_code(c);
        if (e != ESP_OK || (status != 301 && status != 302 && status != 303 && status != 307 && status != 308)) {
            break;
        }
        esp_http_client_flush_response(c, NULL);
        if (esp_http_client_set_redirection(c) != ESP_OK) {
            break;
        }
        esp_http_client_close(c);
        ESP_LOGI(TAG, "redirected (HTTP %d)", status);
    }
    if (e != ESP_OK || status != 200) {
        char m[96];
        if (e != ESP_OK) {
            snprintf(m, sizeof m, "download failed: %s", esp_err_to_name(e));
        } else {
            snprintf(m, sizeof m, "download failed: HTTP %d", status);
        }
        fail(NULL, 0, m);
        esp_http_client_cleanup(c);
        vTaskDelete(NULL);
        return;
    }
    s_state = OTA_IDLE;
    if (ota_begin_upload(len > 0 ? (size_t)len : 0, err, sizeof err) != ESP_OK) {
        if (s_state != OTA_ERROR) {
            fail(NULL, 0, err);
        }
        esp_http_client_cleanup(c);
        vTaskDelete(NULL);
        return;
    }
    char *buf = malloc(4096);
    if (!buf) {
        ota_abort();
        fail(NULL, 0, "out of memory");
    }
    while (buf) {
        int n = esp_http_client_read(c, buf, 4096);
        if (n < 0) {
            ota_abort();
            fail(NULL, 0, "download interrupted");
            break;
        }
        if (n == 0) {
            if (esp_http_client_is_complete_data_received(c) || len <= 0) {
                if (ota_finish(err, sizeof err) == ESP_OK && s_kind == OTA_KIND_FIRMWARE) {
                    wb_restart("firmware update", 2000);
                }
            } else {
                ota_abort();
                fail(NULL, 0, "download incomplete");
            }
            break;
        }
        if (ota_write(buf, n, err, sizeof err) != ESP_OK) {
            break;
        }
    }
    free(buf);
    esp_http_client_cleanup(c);
    vTaskDelete(NULL);
}

esp_err_t ota_start_url(const char *url, char *err, size_t errlen)
{
    if (s_state == OTA_RUNNING) {
        strlcpy(err, "an update is already running", errlen);
        return ESP_ERR_INVALID_STATE;
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        strlcpy(err, "URL must start with http:// or https://", errlen);
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_url, url, sizeof s_url);
    s_state = OTA_RUNNING;            /* claimed until the task starts the real upload */
    s_bytes = s_total = 0;
    s_err[0] = 0;
    ESP_LOGI(TAG, "downloading %s", s_url);
    if (xTaskCreate(url_task, "ota_url", 8192, NULL, 5, NULL) != pdPASS) {
        s_state = OTA_ERROR;
        strlcpy(err, "out of memory", errlen);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ota_status_json(cJSON *o)
{
    static const char *st[] = { "idle", "running", "done", "error" };
    cJSON_AddStringToObject(o, "state", st[s_state]);
    cJSON_AddNumberToObject(o, "progress", s_total ? (int)(s_bytes * 100 / s_total) : (s_state == OTA_DONE ? 100 : 0));
    cJSON_AddStringToObject(o, "error", s_err);
    cJSON_AddNumberToObject(o, "bytes", s_bytes);
    cJSON_AddNumberToObject(o, "total", s_total);
    static const char *kind[] = { NULL, "firmware", "safeboot" };
    if (s_state == OTA_DONE && kind[s_kind]) {
        cJSON_AddStringToObject(o, "installed", kind[s_kind]);
    }
}
