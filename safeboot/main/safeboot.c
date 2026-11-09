/*
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 *
 * Safeboot firmware. Devices with the safeboot partition layout have one app slot, so WiFi Bridge can't
 * overwrite itself while running. This small image lives in the factory slot: WiFi Bridge restarts into it,
 * it joins the same Wi-Fi at the same management address, takes the new
 * firmware over HTTP and boots it.
 *
 * Any restart goes back to WiFi Bridge (the boot slot is switched back at start), and it returns there by
 * itself after 10 minutes without a visitor. If Wi-Fi isn't configured or doesn't connect, it opens an
 * access point and serves the same page on the Ethernet port.
 */
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "config.h"
#include "wb.h"
#include "web_util.h"

static const char *TAG = "safeboot";

#define IDLE_RETURN_MIN 10      /* back to WiFi Bridge after this long without a visitor */
#define FALLBACK_S      45      /* access point + Ethernet page if Wi-Fi hasn't connected by then */

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");

wb_state_t g_state;              /* shared sources (eth.c) use it */

static esp_netif_t *s_sta, *s_ap;
static volatile bool s_connected;
static bool s_fallback;
static int s_ssid_idx;
static int64_t s_last_visit_us;
static char s_ap_ssid[33];

/* ---- hooks the shared sources expect ---- */

void bridge_from_wired(uint8_t *buffer, uint16_t len) { }

uint32_t wifi_ap_ip(void)
{
    return ipaddr_addr(WB_AP_IP);
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS((uint32_t)arg));
    esp_restart();
}

void wb_restart(const char *why, uint32_t delay_ms)
{
    ESP_LOGW(TAG, "restarting: %s", why);
    xTaskCreate(restart_task, "restart", 2048, (void *)delay_ms, 10, NULL);
}

/* ---- firmware slot ---- */

static const esp_partition_t *app_slot(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
}

/* Boot WiFi Bridge on the next restart, if the slot holds a valid image */
static bool boot_firmware_next(void)
{
    const esp_partition_t *p = app_slot();
    return p && esp_ota_set_boot_partition(p) == ESP_OK;
}

static bool firmware_desc(esp_app_desc_t *d)
{
    const esp_partition_t *p = app_slot();
    return p && esp_ota_get_partition_description(p, d) == ESP_OK &&
           strncmp(d->project_name, WB_FIRMWARE_PROJECT, sizeof d->project_name) == 0;
}

/* ---- Wi-Fi ---- */

static const char *ssid_now(void)
{
    return s_ssid_idx == 1 && g_cfg.ssid2[0] ? g_cfg.ssid2 : g_cfg.ssid1;
}

static void sta_connect(void)
{
    wifi_config_t wc = { 0 };
    const char *ssid = ssid_now();
    strlcpy((char *)wc.sta.ssid, ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, ssid == g_cfg.ssid2 ? g_cfg.pass2 : g_cfg.pass1, sizeof wc.sta.password);
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    ESP_LOGW(TAG, "connecting to \"%s\"", ssid);
    esp_wifi_connect();
}

static void fallback_start(void)
{
    if (s_fallback) {
        return;
    }
    s_fallback = true;
    ESP_LOGW(TAG, "Wi-Fi not connected: starting access point \"%s\" (http://%s/) and the Ethernet page", s_ap_ssid,
             WB_AP_IP);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_config_t ap = { .ap = { .channel = 1, .max_connection = 4, .authmode = WIFI_AUTH_OPEN } };
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof ap.ap.ssid);
    ap.ap.ssid_len = strlen(s_ap_ssid);
    if (strlen(g_cfg.ap_pass) >= 8) {
        strlcpy((char *)ap.ap.password, g_cfg.ap_pass, sizeof ap.ap.password);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    captive_dns_start();
#if !CONFIG_WB_QEMU
    static bool eth_started;
    if (!eth_started && eth_hw_init() == ESP_OK) {
        eth_started = eth_start_setup() == ESP_OK;
    }
#endif
}

static void fallback_cb(void *arg)
{
    if (!s_connected) {
        fallback_start();
    }
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && config_is_provisioned()) {
        sta_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (config_is_provisioned()) {
            if (g_cfg.ssid2[0]) {
                s_ssid_idx ^= 1;
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            sta_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_connected = true;
        ESP_LOGW(TAG, "connected to \"%s\": update page at http://" IPSTR "/", ssid_now(), IP2STR(&e->ip_info.ip));
        if (s_fallback) {
            /* the open access point is only for when Wi-Fi is down */
            esp_wifi_set_mode(WIFI_MODE_STA);
            s_fallback = false;
            ESP_LOGW(TAG, "access point off");
        }
    }
}

static void wifi_start(void)
{
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, g_cfg.hostname);
    snprintf(s_ap_ssid, sizeof s_ap_ssid, "%.23s-safeboot", g_cfg.hostname);

    if (g_cfg.ip) {
        /* the bridge's management address */
        esp_netif_dhcpc_stop(s_sta);
        esp_netif_ip_info_t ip = { .ip.addr = g_cfg.ip, .netmask.addr = g_cfg.netmask, .gw.addr = g_cfg.gateway };
        esp_netif_set_ip_info(s_sta, &ip);
        uint32_t dns = g_cfg.dns ? g_cfg.dns : g_cfg.gateway;
        if (dns) {
            esp_netif_dns_info_t d = { .ip.type = ESP_IPADDR_TYPE_V4, .ip.u_addr.ip4.addr = dns };
            esp_netif_set_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &d);
        }
    }
    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);

    /* Keep the ESP32's station MAC, just as the bridge does. Using the wired client's
     * MAC here changes the management address's identity and leaves stale ARP entries
     * in the router and browsers' hosts across each recovery restart. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_start();

    if (!config_is_provisioned()) {
        fallback_start();
    } else {
        static esp_timer_handle_t t;
        esp_timer_create_args_t a = { .callback = fallback_cb, .name = "fallback" };
        esp_timer_create(&a, &t);
        esp_timer_start_once(t, (uint64_t)FALLBACK_S * 1000000);
    }
}

/* ---- web ---- */

static esp_err_t h_index(httpd_req_t *req)
{
    AUTH();
    s_last_visit_us = esp_timer_get_time();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    /* no framing by other sites (clickjacking) */
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "frame-ancestors 'none'");
    return httpd_resp_send(req, (const char *)index_html_gz_start, index_html_gz_end - index_html_gz_start);
}

static esp_err_t h_status(httpd_req_t *req)
{
    AUTH();
    s_last_visit_us = esp_timer_get_time();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "mode", "safeboot");
    cJSON *d = cJSON_AddObjectToObject(o, "device");
    cJSON_AddStringToObject(d, "name", g_cfg.name);
    cJSON_AddStringToObject(d, "hostname", g_cfg.hostname);
    cJSON_AddStringToObject(d, "safeboot_version", esp_app_get_description()->version);
    esp_app_desc_t fw;
    if (firmware_desc(&fw)) {
        char v[33];
        strlcpy(v, fw.version, sizeof v);
        cJSON_AddStringToObject(d, "firmware_version", v);
    } else {
        cJSON_AddNullToObject(d, "firmware_version");
    }
    cJSON_AddNumberToObject(d, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddBoolToObject(d, "auth", g_cfg.admin_pass[0] != 0);
    cJSON_AddNumberToObject(d, "return_in_s", firmware_desc(&fw) ? IDLE_RETURN_MIN * 60 : 0);
    cJSON *w = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddStringToObject(w, "state", s_connected ? "connected" : config_is_provisioned() ? "connecting" : "idle");
    cJSON_AddStringToObject(w, "ssid", ssid_now());
    esp_netif_ip_info_t ip = { 0 };
    char b[16] = "";
    if (s_connected && esp_netif_get_ip_info(s_sta, &ip) == ESP_OK) {
        esp_ip4addr_ntoa(&ip.ip, b, sizeof b);
    }
    cJSON_AddStringToObject(w, "ip", b);
    cJSON_AddBoolToObject(w, "ap_active", s_fallback);
    cJSON_AddStringToObject(w, "ap_ssid", s_ap_ssid);
    cJSON *e = cJSON_AddObjectToObject(o, "ethernet");
    eth_status_json(e);
    cJSON *ota = cJSON_AddObjectToObject(o, "ota");
    ota_status_json(ota);
    return send_json(req, o);
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    AUTH();
    const char *target = "normal";
    cJSON *j = req->content_len ? read_json(req, 256) : NULL;
    if (req->content_len && !j) {
        return ESP_OK;
    }
    const cJSON *t = cJSON_GetObjectItem(j, "target");
    if (cJSON_IsString(t)) {
        target = t->valuestring;
    }
    bool stay = !strcmp(target, "safeboot");
    cJSON_Delete(j);
    if (!stay && !boot_firmware_next()) {
        return send_err(req, "409 Conflict", "no valid WiFi Bridge firmware is installed: install it first");
    }
    if (stay) {
        esp_ota_set_boot_partition(esp_ota_get_running_partition());
    }
    wb_restart(stay ? "restart safeboot" : "back to WiFi Bridge", 1000);
    return send_ok(req);
}

/* Captive-portal probes and anything else: send the browser to the page */
static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t err)
{
    char me[16], loc[40];
    local_ip(req, me, sizeof me);
    snprintf(loc, sizeof loc, "http://%s/", me);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void sb_web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start web server");
        return;
    }
    const httpd_uri_t uris[] = {
        { "/",               HTTP_GET,  h_index,      NULL },
        { "/api/status",     HTTP_GET,  h_status,     NULL },
        { "/api/reboot",     HTTP_POST, h_reboot,     NULL },
        { "/api/ota/upload", HTTP_POST, h_ota_upload, NULL },
        { "/api/ota/url",    HTTP_POST, h_ota_url,    NULL },
        { "/api/ota",        HTTP_GET,  h_ota_get,    NULL },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, h_404);
}

/* ---- main ---- */

static void idle_check(void *arg)
{
    cJSON *o = cJSON_CreateObject();
    ota_status_json(o);
    const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(o, "state"));
    bool busy = st && (!strcmp(st, "running") || !strcmp(st, "done"));
    cJSON_Delete(o);
    esp_app_desc_t d;
    if (!busy && firmware_desc(&d) && esp_timer_get_time() - s_last_visit_us > (int64_t)IDLE_RETURN_MIN * 60 * 1000000) {
        if (boot_firmware_next()) {
            wb_restart("no visitor for 10 minutes: back to WiFi Bridge", 100);
        }
    }
}

void app_main(void)
{
    const esp_app_desc_t *me = esp_app_get_description();
    printf("\n==== WiFi Bridge safeboot %s ====\n", me->version);

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* don't erase: the settings belong to WiFi Bridge. Run with defaults (access point). */
        ESP_LOGE(TAG, "settings storage unreadable (%s): using defaults", esp_err_to_name(e));
    }
    config_init();

    esp_app_desc_t fw;
    bool have_fw = firmware_desc(&fw);
    /* one visit only: the next restart (power cut, crash, or done) runs WiFi Bridge again */
    if (have_fw && boot_firmware_next()) {
        ESP_LOGW(TAG, "installed firmware: WiFi Bridge %.32s (boots on the next restart)", fw.version);
    } else {
        ESP_LOGW(TAG, "no valid WiFi Bridge firmware installed: staying in safeboot");
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if CONFIG_WB_QEMU
    /* no Wi-Fi in QEMU: the page is on the emulated Ethernet */
    if (eth_hw_init() == ESP_OK) {
        eth_start_setup();
    }
#else
    wifi_start();
#endif
    sb_web_start();
    s_last_visit_us = esp_timer_get_time();

    static esp_timer_handle_t t;
    esp_timer_create_args_t a = { .callback = idle_check, .name = "idle" };
    esp_timer_create(&a, &t);
    esp_timer_start_periodic(t, 30 * 1000000ULL);

    char ip[16] = "(DHCP)";
    if (g_cfg.ip) {
        esp_ip4_addr_t i = { .addr = g_cfg.ip };
        esp_ip4addr_ntoa(&i, ip, sizeof ip);
    }
    printf("Update page: http://%s/ (Wi-Fi \"%s\"), or join \"%s\" -> http://%s/ if Wi-Fi fails,\n"
           "or plug a computer into Ethernet -> http://%s/. Returns to WiFi Bridge after %d minutes idle.\n",
           ip, g_cfg.ssid1, s_ap_ssid, WB_AP_IP, WB_ETH_SETUP_IP, IDLE_RETURN_MIN);
}
