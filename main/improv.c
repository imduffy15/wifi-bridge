/*
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 *
 * Improv Wi-Fi over serial (https://www.improv-wifi.com/serial/): the web installer sends the Wi-Fi network
 * right after flashing, over the same USB cable. Packets share UART0 with the console and the logs:
 *
 *   "IMPROV" | version 1 | type | length | data | checksum (sum of all previous bytes)
 *
 * Wi-Fi settings run the same connection test as the setup page; on success the bridge takes the free
 * address the test suggests as its management address, saves, and restarts into bridge mode. The installer
 * then offers a link to that address.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "config.h"
#include "wb.h"

static const char *TAG = "improv";

enum { T_STATE = 1, T_ERROR = 2, T_RPC = 3, T_RESULT = 4 };
enum { ST_READY = 2, ST_PROVISIONING = 3, ST_PROVISIONED = 4 };
enum { E_NONE = 0, E_INVALID = 1, E_UNKNOWN_CMD = 2, E_CONNECT = 3, E_UNKNOWN = 0xFF };
enum { C_WIFI = 1, C_STATE = 2, C_INFO = 3, C_SCAN = 4 };

static const char HDR[] = "IMPROV";
static uint8_t s_buf[6 + 3 + 255 + 1];
static size_t s_n, s_want;
static volatile bool s_busy;             /* a Wi-Fi settings command is running */
static volatile bool s_scanning;         /* a scan command is running */

static void send(uint8_t type, const uint8_t *data, uint8_t len)
{
    uint8_t p[6 + 3 + 255 + 2];
    memcpy(p, HDR, 6);
    p[6] = 1;
    p[7] = type;
    p[8] = len;
    memcpy(p + 9, data, len);
    uint8_t sum = 0;
    for (size_t i = 0; i < 9u + len; i++) {
        sum += p[i];
    }
    p[9 + len] = sum;
    p[10 + len] = '\n';                  /* keeps serial monitors readable; ignored by clients */
    flockfile(stdout);
    fwrite(p, 1, 11 + len, stdout);
    fflush(stdout);
    funlockfile(stdout);
}

static void send_state(uint8_t st) { send(T_STATE, &st, 1); }
static void send_error(uint8_t e) { send(T_ERROR, &e, 1); }

/* RPC result: command, total length, then length-prefixed strings */
static void send_result(uint8_t cmd, const char *const *strs, int n)
{
    uint8_t d[255];
    size_t o = 2;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(strs[i]);
        if (o + 1 + l > sizeof d) {
            break;
        }
        d[o++] = l;
        memcpy(d + o, strs[i], l);
        o += l;
    }
    d[0] = cmd;
    d[1] = o - 2;
    send(T_RESULT, d, o);
}

static bool provisioned(void)
{
    return config_is_provisioned() && g_state.mode == MODE_BRIDGE;
}

static void my_url(char *url, size_t n)
{
    ip4_addr_t a = { .addr = g_cfg.ip };
    snprintf(url, n, "http://%s/", ip4addr_ntoa(&a));
}

static void wifi_task(void *arg)
{
    char *ssid = arg, *pass = ssid + 33;
    send_state(ST_PROVISIONING);
    ESP_LOGI(TAG, "testing Wi-Fi \"%s\"", ssid);
    bool ok = false;
    cJSON *r = NULL;
    if (g_state.mode == MODE_SETUP && wifi_test_start(ssid, pass) == ESP_OK) {
        for (int i = 0; i < 70 && !ok; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            cJSON_Delete(r);
            r = wifi_test_json();
            const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(r, "state"));
            const char *sug = cJSON_GetStringValue(cJSON_GetObjectItem(r, "suggested_ip"));
            if (st && !strcmp(st, "failed")) {
                break;
            }
            /* wait for the free-address suggestion too: it becomes the management address */
            ok = st && !strcmp(st, "connected") && sug && sug[0];
        }
    }
    if (ok) {
        cJSON *c = cJSON_CreateObject();
        cJSON *w = cJSON_AddObjectToObject(c, "wifi");
        cJSON_AddStringToObject(w, "ssid1", ssid);
        cJSON_AddStringToObject(w, "password1", pass);
        cJSON *n = cJSON_AddObjectToObject(c, "network");
        cJSON_AddStringToObject(n, "ip", cJSON_GetStringValue(cJSON_GetObjectItem(r, "suggested_ip")));
        static const char *k[] = { "netmask", "gateway", "dns" };
        for (int i = 0; i < 3; i++) {
            const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(r, k[i]));
            cJSON_AddStringToObject(n, k[i], v ? v : "");
        }
        char err[128];
        ok = config_update_json(c, err, sizeof err, NULL) == ESP_OK && config_is_provisioned();
        cJSON_Delete(c);
        if (!ok) {
            ESP_LOGE(TAG, "saving failed: %s", err);
        }
    }
    cJSON_Delete(r);
    if (ok) {
        nvs_flag_set("setup_once", 0);
        char url[32];
        my_url(url, sizeof url);
        ESP_LOGI(TAG, "provisioned: %s", url);
        send_state(ST_PROVISIONED);
        const char *s[] = { url };
        send_result(C_WIFI, s, 1);
        wb_restart("provisioned over Improv", 3000);
    } else {
        send_error(E_CONNECT);
        send_state(provisioned() ? ST_PROVISIONED : ST_READY);
    }
    free(arg);
    s_busy = false;
    vTaskDelete(NULL);
}

static void scan_task(void *arg)
{
    wifi_scan_start();
    cJSON *r = NULL;
    for (int i = 0; i < 40; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        cJSON_Delete(r);
        r = wifi_scan_json();
        if (!cJSON_IsTrue(cJSON_GetObjectItem(r, "scanning"))) {
            break;
        }
    }
    const cJSON *n;
    cJSON_ArrayForEach(n, cJSON_GetObjectItem(r, "networks")) {
        const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(n, "ssid"));
        if (!ssid || !ssid[0]) {
            continue;
        }
        char rssi[8];
        snprintf(rssi, sizeof rssi, "%d", (int)cJSON_GetNumberValue(cJSON_GetObjectItem(n, "rssi")));
        const char *s[] = { ssid, rssi, cJSON_IsTrue(cJSON_GetObjectItem(n, "secure")) ? "YES" : "NO" };
        send_result(C_SCAN, s, 3);
    }
    cJSON_Delete(r);
    send_result(C_SCAN, NULL, 0);
    s_scanning = false;
    vTaskDelete(NULL);
}

static void rpc(const uint8_t *d, uint8_t len)
{
    if (len < 2 || d[1] != len - 2) {
        send_error(E_INVALID);
        return;
    }
    switch (d[0]) {
    case C_STATE: {
        send_state(provisioned() ? ST_PROVISIONED : ST_READY);
        if (provisioned()) {
            char url[32];
            my_url(url, sizeof url);
            const char *s[] = { url };
            send_result(C_STATE, s, 1);
        }
        break;
    }
    case C_INFO: {
        const char *s[] = { "WiFi Bridge", esp_app_get_description()->version, "ESP32",
                            g_cfg.name[0] ? g_cfg.name : g_cfg.hostname };
        send_result(C_INFO, s, 4);
        break;
    }
    case C_SCAN:
        if (s_scanning) {
            break;                       /* the running scan answers */
        }
        s_scanning = true;
        if (xTaskCreate(scan_task, "improv_scan", 4096, NULL, 3, NULL) != pdPASS) {
            s_scanning = false;
            send_error(E_UNKNOWN);
        }
        break;
    case C_WIFI: {
        /* ssid length, ssid, password length, password */
        const uint8_t *p = d + 2, *end = d + len;
        if (p >= end || p[0] > 32 || p + 1 + p[0] >= end) {
            send_error(E_INVALID);
            break;
        }
        uint8_t sl = p[0], pl = p[1 + sl];
        if (pl > 64 || p + 2 + sl + pl > end) {
            send_error(E_INVALID);
            break;
        }
        if (g_state.mode != MODE_SETUP) {
            /* bridging: change networks from the setup page (it keeps the management address in step) */
            send_error(E_UNKNOWN_CMD);
            break;
        }
        if (s_busy) {
            break;
        }
        char *buf = calloc(1, 33 + 65);
        if (!buf) {
            send_error(E_UNKNOWN);
            break;
        }
        memcpy(buf, p + 1, sl);
        memcpy(buf + 33, p + 2 + sl, pl);
        s_busy = true;
        if (xTaskCreate(wifi_task, "improv_wifi", 4096, buf, 3, NULL) != pdPASS) {
            free(buf);
            s_busy = false;
            send_error(E_UNKNOWN);
        }
        break;
    }
    default:
        send_error(E_UNKNOWN_CMD);
    }
}

int improv_feed(uint8_t c)
{
    if (s_n < 6) {
        if (c == (uint8_t)HDR[s_n]) {
            s_buf[s_n++] = c;
            return s_n == 6 ? 2 : 0;     /* 2: the header just completed (the console drops it) */
        }
        s_n = c == 'I';
        s_buf[0] = c;
        return 0;
    }
    s_buf[s_n++] = c;
    if (s_n == 9) {
        s_want = 9 + s_buf[8] + 1;       /* header + version/type/length + data + checksum */
    }
    if (s_n < 9 || s_n < s_want) {
        return 1;
    }
    uint8_t sum = 0;
    for (size_t i = 0; i < s_want - 1; i++) {
        sum += s_buf[i];
    }
    size_t n = s_n;
    s_n = 0;
    if (sum != s_buf[n - 1] || s_buf[6] != 1) {
        send_error(E_INVALID);
        return 1;
    }
    if (s_buf[7] == T_RPC) {
        rpc(s_buf + 9, s_buf[8]);
    }
    return 1;
}
