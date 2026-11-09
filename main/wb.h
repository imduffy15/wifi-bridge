/*
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared runtime state and module entry points.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "cJSON.h"

#define WB_AP_IP       "192.168.4.1"
#define WB_AP_IP_ALT   "172.31.254.1"  /* used when the LAN itself is 192.168.4.0/24 */
#if CONFIG_WB_QEMU
#define WB_ETH_SETUP_IP "10.0.2.15"     /* QEMU user networking forwards host ports to this address */
#else
#define WB_ETH_SETUP_IP "192.168.5.1"
#endif

typedef enum { MODE_SETUP, MODE_BRIDGE } wb_mode_t;

typedef struct {
    volatile uint32_t wired_to_wifi, wifi_to_wired, to_me;
    volatile uint32_t wifi_tx_fail, wifi_drop, wired_drop;
    volatile uint32_t client_ip;            /* wired device IPv4 (network order), learned from ARP */
} wb_frames_t;

typedef struct {
    wb_mode_t mode;
    bool reboot_required;
    wb_frames_t frames;
    uint32_t boot_count;
    bool safe_mode;
    int64_t last_web_us;                    /* last /api/status request (esp_timer time) */                         /* boot loop detected: setup portal only, no Ethernet/LED/button/MQTT */
} wb_state_t;

extern wb_state_t g_state;

/* main.c */
void wb_restart(const char *why, uint32_t delay_ms);       /* deferred restart (lets HTTP replies go out) */
esp_err_t wb_reboot_to(const char *target, uint32_t delay_ms);  /* "normal" | "safeboot" | "setup" */
const char *wb_ota_mode(void);                             /* "dual" | "safeboot" | "none" */
void wb_mark_healthy(const char *why);                     /* cancel rollback of a freshly flashed image */

/* wifi.c */
typedef enum { WIFI_ST_IDLE, WIFI_ST_CONNECTING, WIFI_ST_CONNECTED, WIFI_ST_DISCONNECTED } wb_wifi_state_t;
void wifi_init(wb_mode_t mode);
wb_wifi_state_t wifi_state(void);
bool wifi_connected(void);
bool wifi_ap_active(void);
esp_err_t wifi_ap_force(bool on);           /* bridge mode: setup AP on/off now (until restart) */
int wifi_ap_clients(void);
uint32_t wifi_connected_s(void);
uint32_t wifi_disconnects(void);
const char *wifi_last_reason(void);
esp_netif_t *wifi_sta_netif(void);
esp_netif_t *wifi_ap_netif(void);
uint32_t wifi_ap_ip(void);                 /* network order */
void wifi_scan_start(void);
cJSON *wifi_scan_json(void);              /* {"scanning":..,"networks":[..]} */
esp_err_t wifi_test_start(const char *ssid, const char *pass);
cJSON *wifi_test_json(void);
void wifi_apply_runtime(void);            /* power save / tx power after a config change */
int wifi_quality(int rssi);
void wifi_status_json(cJSON *o);

/* bridge.c */
void bridge_start(void);                  /* bridge mode: raw Wi-Fi <-> Ethernet forwarding */
void bridge_wifi_connected(bool up);      /* called by wifi.c on STA (dis)connect */

/* eth.c */
esp_err_t eth_hw_init(void);              /* EMAC/PHY from the board config */
esp_err_t eth_start_bridge(void);
esp_err_t eth_start_setup(void);          /* DHCP server + management page on the wired port */
void eth_status_json(cJSON *o);
bool eth_link_up(void);
void eth_mac(uint8_t mac[6]);

/* dns.c */
void captive_dns_start(void);

/* web.c */
void web_start(void);

/* ota.c */
#define WB_FIRMWARE_PROJECT "wifi-bridge"
#define WB_SAFEBOOT_PROJECT "wifi-bridge-safeboot"
typedef enum { OTA_KIND_NONE, OTA_KIND_FIRMWARE, OTA_KIND_SAFEBOOT } ota_kind_t;
esp_err_t ota_begin_upload(size_t total, char *err, size_t errlen);
esp_err_t ota_write(const void *data, size_t len, char *err, size_t errlen);
esp_err_t ota_finish(char *err, size_t errlen);
void ota_abort(void);
esp_err_t ota_start_url(const char *url, char *err, size_t errlen);
void ota_status_json(cJSON *o);
ota_kind_t ota_installed_kind(void);      /* after a successful ota_finish(): what was installed */
bool ota_safeboot_ready(void);           /* safeboot partition present (not running) with a valid image */
void ota_safeboot_json(cJSON *o);         /* adds "safeboot": null or {ours, version, size} */

/* improv.c */
/* Feed one serial byte. 0: not Improv, 1: part of a packet, 2: completes the "IMPROV" header (the
 * previous 5 bytes were part of it too). */
int improv_feed(uint8_t c);

/* console.c */
void console_start(void);
/* Execute a command line ("Status 0", "Backlog a; b"). Result is JSON (or text). */
void console_exec(const char *line, char *out, size_t outlen, const char *source);

/* mqtt.c */
void mqtt_start(void);
bool mqtt_connected(void);
void mqtt_publish_state(void);
void mqtt_apply_runtime(void);           /* TelePeriod change */

/* system.c */
void sys_early_init(void);                /* power-cycle reset detection, LED, button */
void sys_net_services_start(void);        /* SNTP, mDNS */
void sys_apply_runtime(void);             /* log level, CPU frequency, timezone */
void sys_time_iso(char *buf, size_t n);   /* "" if not synced */
const char *sys_reset_reason(void);
void sys_status_json(cJSON *o);           /* builds the full /api/status object */
void sys_setup_timeout_start(void);
void sys_crash_json(cJSON *o);            /* adds "last_crash" (null or summary) */
esp_err_t sys_crash_erase(void);
