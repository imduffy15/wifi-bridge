/*
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi manager.
 *   setup mode : AP+STA. The AP (SSID = hostname) hosts the captive portal; the STA scans and runs
 *                connection tests (DHCP) for the setup wizard.
 *   bridge mode: STA with the static management address, primary/secondary SSID with back-off,
 *                and a fallback AP after N minutes without a connection.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "lwip/etharp.h"
#include "lwip/tcpip.h"
#include "ping/ping_sock.h"
#include "config.h"
#include "wb.h"

static const char *TAG = "wifi";

#define MAX_SCAN 24

static wb_mode_t s_mode;
static esp_netif_t *s_sta, *s_ap;
static volatile wb_wifi_state_t s_state = WIFI_ST_IDLE;
static volatile bool s_ap_on, s_ap_forced;
static volatile int s_ap_clients;
static int64_t s_connected_at, s_down_since, s_ap_idle_since;
static uint32_t s_disconnects;
static uint8_t s_last_reason;
static int s_fail_count, s_ssid_idx;          /* bridge mode: which configured SSID we're trying */
static esp_timer_handle_t s_retry_timer, s_tick_timer, s_test_timer;

/* scan */
static SemaphoreHandle_t s_lock;
static wifi_ap_record_t *s_scan;
static int s_scan_n;
static volatile bool s_scanning;
static int64_t s_scan_at;

/* connection test (setup mode) */
typedef enum { T_IDLE, T_CONNECTING, T_CONNECTED, T_FAILED } test_state_t;
static volatile test_state_t s_test = T_IDLE;
static volatile bool s_suggesting;
static char s_test_err[48];
static int s_test_attempts;
static esp_netif_ip_info_t s_test_ip;
static uint32_t s_test_dns, s_suggest;
static uint32_t s_ap_ip;                       /* setup/fallback AP address (network order) */

uint32_t wifi_ap_ip(void) { return s_ap_ip; }

int wifi_quality(int rssi)
{
    return rssi <= -100 ? 0 : rssi >= -50 ? 100 : 2 * (rssi + 100);
}

static const char *reason_str(uint8_t r)
{
    switch (r) {
    case 0: return "";
    case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE: return "AUTH_LEAVE";
    case WIFI_REASON_ASSOC_LEAVE: return "ASSOC_LEAVE";
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY: return "INACTIVITY";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
    case WIFI_REASON_CONNECTION_FAIL: return "CONNECTION_FAIL";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "NO_AP_COMPATIBLE_SECURITY";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "NO_AP_IN_AUTHMODE_THRESHOLD";
    default: {
        static char buf[12];
        snprintf(buf, sizeof buf, "REASON_%u", r);
        return buf;
    }
    }
}

static const char *reason_human(uint8_t r)
{
    switch (r) {
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_MIC_FAILURE:
        return "wrong password";
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_BEACON_TIMEOUT:
        return "network not found";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "network security not supported (needs a password?)";
    default:
        return "could not connect";
    }
}

static void set_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t w = { 0 };
    strlcpy((char *)w.sta.ssid, ssid, sizeof w.sta.ssid);
    strlcpy((char *)w.sta.password, pass, sizeof w.sta.password);
    w.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    w.sta.pmf_cfg.capable = true;
    w.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    w.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;      /* pick the strongest AP of a mesh/multi-AP network */
    w.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    w.sta.failure_retry_cnt = 2;
    esp_wifi_set_config(WIFI_IF_STA, &w);
}

/* Fast connect: remember the access point (BSSID + channel) of the last good connection. At boot the first
 * attempt goes straight to it instead of scanning every channel; if that fails, retries scan as usual
 * (the strongest AP of a mesh may have changed). */
typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
} fast_ap_t;

static bool fast_config(const char *ssid)
{
    fast_ap_t f;
    if (!nvs_blob_get("fast_ap", &f, sizeof f) || strncmp(f.ssid, ssid, sizeof f.ssid) != 0 || !f.channel ||
        f.channel > 14) {
        return false;
    }
    wifi_config_t w;
    if (esp_wifi_get_config(WIFI_IF_STA, &w) != ESP_OK) {
        return false;
    }
    memcpy(w.sta.bssid, f.bssid, 6);
    w.sta.bssid_set = true;
    w.sta.channel = f.channel;
    w.sta.scan_method = WIFI_FAST_SCAN;
    esp_wifi_set_config(WIFI_IF_STA, &w);
    ESP_LOGI(TAG, "fast connect: %02x:%02x:%02x:%02x:%02x:%02x on channel %u", f.bssid[0], f.bssid[1], f.bssid[2],
             f.bssid[3], f.bssid[4], f.bssid[5], f.channel);
    return true;
}

static void fast_remember(const wifi_event_sta_connected_t *c)
{
    fast_ap_t f = { 0 }, old;
    memcpy(f.ssid, c->ssid, c->ssid_len < 32 ? c->ssid_len : 32);
    memcpy(f.bssid, c->bssid, 6);
    f.channel = c->channel;
    if (!nvs_blob_get("fast_ap", &old, sizeof old) || memcmp(&old, &f, sizeof f) != 0) {
        nvs_blob_set("fast_ap", &f, sizeof f);       /* only on change: spares the flash */
    }
}

static void ap_set(bool on)
{
    if (on == s_ap_on) {
        return;
    }
    if (on) {
        wifi_config_t a = { 0 };
        strlcpy((char *)a.ap.ssid, g_cfg.hostname, sizeof a.ap.ssid);
        a.ap.ssid_len = strlen(g_cfg.hostname);
        a.ap.max_connection = 4;
        a.ap.channel = 1;
        if (g_cfg.ap_pass[0]) {
            strlcpy((char *)a.ap.password, g_cfg.ap_pass, sizeof a.ap.password);
            a.ap.authmode = WIFI_AUTH_WPA2_PSK;
        } else {
            a.ap.authmode = WIFI_AUTH_OPEN;
        }
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_AP, &a);
        s_ap_on = true;
        s_ap_idle_since = esp_timer_get_time();
        ip4_addr_t ip = { .addr = s_ap_ip };
        ESP_LOGI(TAG, "access point \"%s\" on, setup page at http://%s/", g_cfg.hostname, ip4addr_ntoa(&ip));
    } else {
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_ap_on = false;
        s_ap_clients = 0;
        ESP_LOGI(TAG, "access point off");
    }
}

/* ---------- bridge-mode connection management ---------- */

static const char *cur_ssid(void) { return s_ssid_idx ? g_cfg.ssid2 : g_cfg.ssid1; }
static const char *cur_pass(void) { return s_ssid_idx ? g_cfg.pass2 : g_cfg.pass1; }

static void schedule_retry(void);

static void connect_now(void)
{
    s_state = WIFI_ST_CONNECTING;
    if (s_scanning && s_mode == MODE_SETUP) {
        esp_wifi_scan_stop();                  /* the wizard's scan must not block a connection test */
        s_scanning = false;
    }
    esp_err_t e = s_scanning ? ESP_ERR_WIFI_STATE : esp_wifi_connect();
    if (e != ESP_OK && e != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "connect: %s, retrying", esp_err_to_name(e));
        if (s_mode == MODE_BRIDGE) {
            s_state = WIFI_ST_DISCONNECTED;
            schedule_retry();               /* no DISCONNECTED event will come for an attempt that never started */
        }
    }
}

static void retry_cb(void *arg)
{
    if (s_mode == MODE_BRIDGE && s_state != WIFI_ST_CONNECTED) {
        if (s_scanning) {
            esp_timer_start_once(s_retry_timer, 2000000);
            return;
        }
        set_sta_config(cur_ssid(), cur_pass());
        ESP_LOGI(TAG, "connecting to \"%s\"", cur_ssid());
        connect_now();
    }
}

static void schedule_retry(void)
{
    s_fail_count++;
    if (g_cfg.ssid2[0] && s_fail_count % 3 == 0) {
        s_ssid_idx ^= 1;                          /* alternate between SSID1 and SSID2 */
        ESP_LOGI(TAG, "switching to SSID%d \"%s\"", s_ssid_idx + 1, cur_ssid());
    }
    int shift = s_fail_count < 6 ? s_fail_count : 5;
    int64_t delay_ms = 500LL << shift;            /* 1 s .. 16 s */
    if (s_ap_clients > 0) {
        delay_ms = 30000;                         /* connection attempts hop channels and disturb AP users */
    }
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, delay_ms * 1000);
}

/* 1 s housekeeping: fallback AP on/off */
static void tick_cb(void *arg)
{
    if (s_mode != MODE_BRIDGE) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (s_state != WIFI_ST_CONNECTED) {
        if (!s_ap_on && g_cfg.ap_fallback_min > 0 && now - s_down_since > (int64_t)g_cfg.ap_fallback_min * 60000000LL) {
            ESP_LOGW(TAG, "no Wi-Fi for %ld min: starting fallback access point", (long)g_cfg.ap_fallback_min);
            ap_set(true);
        }
    } else if (s_ap_on && !s_ap_forced) {
        if (s_ap_clients > 0) {
            s_ap_idle_since = now;
        } else if (now - s_ap_idle_since > 120000000LL && now - s_connected_at > 60000000LL) {
            ap_set(false);
        }
    }
}

/* ---------- test & free-address suggestion (setup mode) ---------- */

static SemaphoreHandle_t s_ping_done;
static volatile bool s_ping_reply;
static void ping_ok(esp_ping_handle_t h, void *a) { s_ping_reply = true; }
static void ping_end(esp_ping_handle_t h, void *a) { xSemaphoreGive(s_ping_done); }

static bool ip_in_use(uint32_t addr)
{
    esp_ping_config_t c = ESP_PING_DEFAULT_CONFIG();
    c.target_addr.type = IPADDR_TYPE_V4;
    c.target_addr.u_addr.ip4.addr = addr;
    c.count = 2;
    c.interval_ms = 250;
    c.timeout_ms = 500;
    c.task_stack_size = 3072;
    esp_ping_callbacks_t cb = { .on_ping_success = ping_ok, .on_ping_end = ping_end };
    esp_ping_handle_t h;
    s_ping_reply = false;
    xSemaphoreTake(s_ping_done, 0);        /* drop a late "end" from a previous, timed-out session */
    if (esp_ping_new_session(&c, &cb, &h) != ESP_OK) {
        return true;
    }
    esp_ping_start(h);
    xSemaphoreTake(s_ping_done, pdMS_TO_TICKS(4000));
    esp_ping_delete_session(h);
    if (s_ping_reply) {
        return true;
    }
    /* hosts that drop ICMP still answer ARP, which the ping triggered */
    struct eth_addr *ea;
    const ip4_addr_t *ipr;
    ip4_addr_t a = { .addr = addr };
    LOCK_TCPIP_CORE();
    int r = etharp_find_addr(esp_netif_get_netif_impl(s_sta), &a, &ea, &ipr);
    UNLOCK_TCPIP_CORE();
    return r >= 0;
}

static void suggest_task(void *arg)
{
    uint32_t mask = ntohl(s_test_ip.netmask.addr), net = ntohl(s_test_ip.ip.addr) & mask;
    uint32_t bcast = net | ~mask;
    uint32_t hosts = ~mask;
    uint32_t start = hosts >= 254 ? (net | 250) : bcast - 1;   /* .250 in a /24 */
    s_suggest = 0;
    for (uint32_t h = start, tries = 0; h > net + 1 && tries < 12 && s_test == T_CONNECTING; h--, tries++) {
        uint32_t a = htonl(h);
        if (a == s_test_ip.ip.addr || a == s_test_ip.gw.addr || a == s_test_dns) {
            continue;
        }
        if (!ip_in_use(a)) {
            s_suggest = a;
            break;
        }
        ip4_addr_t x = { .addr = a };
        ESP_LOGI(TAG, "%s is in use", ip4addr_ntoa(&x));
    }
    ip4_addr_t x = { .addr = s_suggest };
    ESP_LOGI(TAG, "suggested management address %s", s_suggest ? ip4addr_ntoa(&x) : "(none)");
    if (s_test == T_CONNECTING) {          /* not cancelled or restarted meanwhile */
        s_test = T_CONNECTED;
    }
    s_suggesting = false;
    vTaskDelete(NULL);
}

static void test_timeout(void *arg)
{
    if (s_test == T_CONNECTING) {
        strlcpy(s_test_err, s_state == WIFI_ST_CONNECTED ? "no address from DHCP" : "timed out", sizeof s_test_err);
        ESP_LOGW(TAG, "test failed: %s", s_test_err);
        s_test = T_FAILED;
        esp_wifi_disconnect();
    }
}

esp_err_t wifi_test_start(const char *ssid, const char *pass)
{
    if (s_mode != MODE_SETUP) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_test == T_CONNECTING || s_suggesting) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "testing connection to \"%s\"", ssid);
    s_test = T_CONNECTING;
    s_test_err[0] = 0;
    s_test_attempts = 0;
    s_suggest = 0;
    esp_wifi_disconnect();
    set_sta_config(ssid, pass);
    esp_netif_dhcpc_start(s_sta);
    connect_now();
    esp_timer_stop(s_test_timer);
    esp_timer_start_once(s_test_timer, 30 * 1000000);
    return ESP_OK;
}

cJSON *wifi_test_json(void)
{
    static const char *st[] = { "idle", "connecting", "connected", "failed" };
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", st[s_test]);
    cJSON_AddStringToObject(o, "error", s_test_err);
    if (s_test == T_CONNECTED) {
        ip4_addr_t a;
        a.addr = s_test_ip.ip.addr;      cJSON_AddStringToObject(o, "ip", ip4addr_ntoa(&a));
        a.addr = s_test_ip.netmask.addr; cJSON_AddStringToObject(o, "netmask", ip4addr_ntoa(&a));
        a.addr = s_test_ip.gw.addr;      cJSON_AddStringToObject(o, "gateway", ip4addr_ntoa(&a));
        a.addr = s_test_dns ? s_test_dns : s_test_ip.gw.addr; cJSON_AddStringToObject(o, "dns", ip4addr_ntoa(&a));
        a.addr = s_suggest;              cJSON_AddStringToObject(o, "suggested_ip", s_suggest ? ip4addr_ntoa(&a) : "");
    }
    return o;
}

/* ---------- scan ---------- */

void wifi_scan_start(void)
{
    if (s_scanning || (s_scan_at && esp_timer_get_time() - s_scan_at < 10000000)) {
        return;
    }
    if (s_mode == MODE_BRIDGE && s_state == WIFI_ST_CONNECTING) {
        return;                                    /* the driver refuses to scan while connecting */
    }
    wifi_scan_config_t sc = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                              .scan_time.active = { .min = 80, .max = 200 } };
    s_scanning = true;
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) {
        s_scanning = false;
    }
}

static const char *auth_name(wifi_auth_mode_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    default: return "other";
    }
}

cJSON *wifi_scan_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "scanning", s_scanning);
    cJSON *arr = cJSON_AddArrayToObject(o, "networks");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_scan_n; i++) {             /* records are sorted by RSSI by the driver */
        const wifi_ap_record_t *r = &s_scan[i];
        if (!r->ssid[0]) {
            continue;
        }
        bool dup = false;
        for (int k = 0; k < i && !dup; k++) {
            dup = strcmp((char *)s_scan[k].ssid, (char *)r->ssid) == 0;
        }
        if (dup) {
            continue;
        }
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid", (char *)r->ssid);
        cJSON_AddNumberToObject(n, "rssi", r->rssi);
        cJSON_AddNumberToObject(n, "quality", wifi_quality(r->rssi));
        cJSON_AddNumberToObject(n, "channel", r->primary);
        cJSON_AddStringToObject(n, "auth", auth_name(r->authmode));
        cJSON_AddBoolToObject(n, "secure", r->authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, n);
    }
    xSemaphoreGive(s_lock);
    return o;
}

/* ---------- events ---------- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_CONNECTED: {
            wifi_event_sta_connected_t *c = data;
            ESP_LOGI(TAG, "connected to \"%.*s\" (%02x:%02x:%02x:%02x:%02x:%02x, channel %d)", c->ssid_len, c->ssid,
                     c->bssid[0], c->bssid[1], c->bssid[2], c->bssid[3], c->bssid[4], c->bssid[5], c->channel);
            s_state = WIFI_ST_CONNECTED;
            s_connected_at = esp_timer_get_time();
            s_fail_count = 0;
            if (s_mode == MODE_BRIDGE) {
                fast_remember(c);
                /* the raw rx callback is registered in on_sta_connected(), after the default handler */
                wifi_apply_runtime();
                wb_mark_healthy("Wi-Fi connected");
            }
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = data;
            bool was = s_state == WIFI_ST_CONNECTED;
            s_last_reason = d->reason;
            if (s_mode == MODE_BRIDGE) {
                if (was) {
                    s_disconnects++;
                    s_down_since = esp_timer_get_time();
                    bridge_wifi_connected(false);
                    ESP_LOGW(TAG, "disconnected (%s)", reason_str(d->reason));
                } else {
                    ESP_LOGW(TAG, "connection to \"%s\" failed (%s)", cur_ssid(), reason_str(d->reason));
                }
                s_state = WIFI_ST_DISCONNECTED;
                if (was) {
                    s_fail_count = 0;
                    connect_now();                 /* immediate first retry after losing a working link */
                } else {
                    schedule_retry();
                }
            } else {
                s_state = WIFI_ST_DISCONNECTED;
                if (s_test == T_CONNECTING) {
                    if (++s_test_attempts >= 3 || strcmp(reason_human(d->reason), "wrong password") == 0) {
                        strlcpy(s_test_err, reason_human(d->reason), sizeof s_test_err);
                        ESP_LOGW(TAG, "test failed: %s (%s)", s_test_err, reason_str(d->reason));
                        s_test = T_FAILED;
                        esp_wifi_disconnect();
                    } else {
                        connect_now();
                    }
                }
            }
            break;
        }
        case WIFI_EVENT_SCAN_DONE: {
            uint16_t n = MAX_SCAN;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (esp_wifi_scan_get_ap_records(&n, s_scan) != ESP_OK) {
                n = 0;
            }
            s_scan_n = n;
            xSemaphoreGive(s_lock);
            s_scanning = false;
            s_scan_at = esp_timer_get_time();
            ESP_LOGI(TAG, "scan found %u networks", n);
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            s_ap_clients++;
            ESP_LOGI(TAG, "AP client joined (%d)", s_ap_clients);
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (s_ap_clients > 0) {
                s_ap_clients--;
            }
            ESP_LOGI(TAG, "AP client left (%d)", s_ap_clients);
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && s_mode == MODE_BRIDGE) {
        if (s_state == WIFI_ST_CONNECTED) {
            bridge_wifi_connected(true);       /* belt and braces: after every default handler has run */
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && s_mode == MODE_SETUP && s_test == T_CONNECTING) {
        ip_event_got_ip_t *g = data;
        s_test_ip = g->ip_info;
        esp_netif_dns_info_t dns;
        s_test_dns = esp_netif_get_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK ? dns.ip.u_addr.ip4.addr : 0;
        ESP_LOGI(TAG, "test OK, leased " IPSTR " gw " IPSTR, IP2STR(&g->ip_info.ip), IP2STR(&g->ip_info.gw));
        esp_timer_stop(s_test_timer);
        if (!s_suggesting) {
            s_suggesting = true;
            if (xTaskCreate(suggest_task, "suggest", 4096, NULL, 3, NULL) != pdPASS) {
                s_suggesting = false;
                s_test = T_CONNECTED;
            }
        }
    }
}

/*
 * Registered for WIFI_EVENT_STA_CONNECTED specifically, after esp_netif_create_default_wifi_sta(): the event
 * loop runs ANY_ID handlers before ID-specific ones, and ID-specific ones in registration order. The default
 * handler registers esp_netif's rx callback on connect; ours must replace it afterwards.
 */
static void on_sta_connected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (s_mode == MODE_BRIDGE) {
        bridge_wifi_connected(true);
    }
}

void wifi_apply_runtime(void)
{
    if (s_mode == MODE_BRIDGE && !s_ap_on) {
        static const wifi_ps_type_t ps[] = { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM };
        esp_wifi_set_ps(ps[g_cfg.power_save % 3]);
    } else {
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
    esp_wifi_set_max_tx_power((int8_t)(g_cfg.tx_power * 4));
}

void wifi_init(wb_mode_t mode)
{
    s_mode = mode;
    s_lock = xSemaphoreCreateMutex();
    s_ping_done = xSemaphoreCreateBinary();
    s_scan = calloc(MAX_SCAN, sizeof(wifi_ap_record_t));

    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, g_cfg.hostname);

    /* AP: captive DNS (ourselves) and RFC 8910 captive-portal URI in DHCP */
    esp_netif_dhcps_stop(s_ap);
    esp_netif_ip_info_t api = { 0 };
    api.netmask.addr = ipaddr_addr("255.255.255.0");
    s_ap_ip = ipaddr_addr(WB_AP_IP);
    if (g_cfg.ip && ((g_cfg.ip ^ s_ap_ip) & (g_cfg.netmask & api.netmask.addr)) == 0) {
        s_ap_ip = ipaddr_addr(WB_AP_IP_ALT);    /* the LAN uses 192.168.4.0/24 itself */
    }
    api.ip.addr = api.gw.addr = s_ap_ip;
    esp_netif_set_ip_info(s_ap, &api);
    esp_netif_dns_info_t dns = { .ip.type = ESP_IPADDR_TYPE_V4, .ip.u_addr.ip4.addr = api.ip.addr };
    esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &dns);
    uint8_t offer_dns = 0x02;
    esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof offer_dns);
    static char uri[32];
    snprintf(uri, sizeof uri, "http://%s/", ip4addr_ntoa((ip4_addr_t *)&s_ap_ip));
    esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, strlen(uri));
    esp_netif_dhcps_start(s_ap);

    if (mode == MODE_BRIDGE) {
        /* static management address: the Wi-Fi MAC is shared with the wired device, which owns the DHCP lease */
        esp_netif_dhcpc_stop(s_sta);
        esp_netif_ip_info_t ip = { .ip.addr = g_cfg.ip, .netmask.addr = g_cfg.netmask, .gw.addr = g_cfg.gateway };
        esp_netif_set_ip_info(s_sta, &ip);
        uint32_t d = g_cfg.dns ? g_cfg.dns : g_cfg.gateway;
        if (d) {
            esp_netif_dns_info_t di = { .ip.type = ESP_IPADDR_TYPE_V4, .ip.u_addr.ip4.addr = d };
            esp_netif_set_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &di);
        }
    }

    esp_timer_create_args_t ra = { .callback = retry_cb, .name = "wifi_retry" };
    esp_timer_create(&ra, &s_retry_timer);
    esp_timer_create_args_t ta = { .callback = tick_cb, .name = "wifi_tick" };
    esp_timer_create(&ta, &s_tick_timer);
    esp_timer_create_args_t xa = { .callback = test_timeout, .name = "wifi_test" };
    esp_timer_create(&xa, &s_test_timer);

#if CONFIG_WB_QEMU
    ESP_LOGW(TAG, "QEMU build: Wi-Fi not started");
    return;
#endif
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, on_sta_connected, NULL);

    esp_wifi_set_mode(WIFI_MODE_STA);
    if (mode == MODE_SETUP) {
        ap_set(true);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_apply_runtime();
    s_down_since = esp_timer_get_time();

    if (mode == MODE_SETUP) {
        wifi_scan_start();                           /* so the wizard has a list straight away */
    } else {
        s_ssid_idx = 0;
        set_sta_config(cur_ssid(), cur_pass());
        fast_config(cur_ssid());                     /* retries use set_sta_config(): full scan again */
        ESP_LOGI(TAG, "connecting to \"%s\"", cur_ssid());
        connect_now();
        esp_timer_start_periodic(s_tick_timer, 1000000);
    }
}

/* Ap 1 / Ap 0: start/stop the setup AP on demand (configure from a phone while bridging) */
esp_err_t wifi_ap_force(bool on)
{
    if (s_mode != MODE_BRIDGE) {
        return ESP_ERR_INVALID_STATE;         /* setup mode always has the AP */
    }
    s_ap_forced = on;
    ap_set(on);
    wifi_apply_runtime();
    return ESP_OK;
}

wb_wifi_state_t wifi_state(void) { return s_state; }
bool wifi_connected(void) { return s_state == WIFI_ST_CONNECTED; }
bool wifi_ap_active(void) { return s_ap_on; }
int wifi_ap_clients(void) { return s_ap_clients; }
uint32_t wifi_disconnects(void) { return s_disconnects; }
const char *wifi_last_reason(void) { return reason_str(s_last_reason); }
esp_netif_t *wifi_sta_netif(void) { return s_sta; }
esp_netif_t *wifi_ap_netif(void) { return s_ap; }
uint32_t wifi_connected_s(void)
{
    return s_state == WIFI_ST_CONNECTED ? (uint32_t)((esp_timer_get_time() - s_connected_at) / 1000000) : 0;
}

void wifi_status_json(cJSON *o)
{
    static const char *st[] = { "idle", "connecting", "connected", "disconnected" };
    cJSON_AddStringToObject(o, "state", st[s_state]);
    wifi_ap_record_t ap;
    bool assoc = s_state == WIFI_ST_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    char bssid[18] = "";
    if (assoc) {
        snprintf(bssid, sizeof bssid, "%02x:%02x:%02x:%02x:%02x:%02x", ap.bssid[0], ap.bssid[1], ap.bssid[2],
                 ap.bssid[3], ap.bssid[4], ap.bssid[5]);
    }
    cJSON_AddStringToObject(o, "ssid", assoc ? (char *)ap.ssid : (s_mode == MODE_BRIDGE ? cur_ssid() : ""));
    cJSON_AddStringToObject(o, "bssid", bssid);
    cJSON_AddNumberToObject(o, "channel", assoc ? ap.primary : 0);
    cJSON_AddNumberToObject(o, "rssi", assoc ? ap.rssi : 0);
    cJSON_AddNumberToObject(o, "quality", assoc ? wifi_quality(ap.rssi) : 0);
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_get_ip_info(s_sta, &ip);
    esp_netif_dns_info_t dns = { 0 };
    esp_netif_get_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &dns);
    char b[16];
    ip4_addr_t a;
    a.addr = ip.ip.addr;      cJSON_AddStringToObject(o, "ip", a.addr ? ip4addr_ntoa_r(&a, b, sizeof b) : "");
    a.addr = ip.netmask.addr; cJSON_AddStringToObject(o, "netmask", a.addr ? ip4addr_ntoa_r(&a, b, sizeof b) : "");
    a.addr = ip.gw.addr;      cJSON_AddStringToObject(o, "gateway", a.addr ? ip4addr_ntoa_r(&a, b, sizeof b) : "");
    a.addr = dns.ip.u_addr.ip4.addr; cJSON_AddStringToObject(o, "dns", a.addr ? ip4addr_ntoa_r(&a, b, sizeof b) : "");
    cJSON_AddNumberToObject(o, "connected_s", wifi_connected_s());
    cJSON_AddNumberToObject(o, "disconnects", s_disconnects);
    cJSON_AddStringToObject(o, "last_reason", reason_str(s_last_reason));
    cJSON_AddBoolToObject(o, "ap_active", s_ap_on);
    cJSON_AddStringToObject(o, "ap_ssid", g_cfg.hostname);
    a.addr = s_ap_ip;         cJSON_AddStringToObject(o, "ap_ip", ip4addr_ntoa_r(&a, b, sizeof b));
    cJSON_AddNumberToObject(o, "ap_clients", s_ap_clients);
}
