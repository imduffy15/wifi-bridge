/*
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 *
 * Persistent configuration. One field table drives NVS storage (JSON blob), the HTTP API,
 * backup/restore and the console setters ("SSID1 HomeNet").
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "cJSON.h"

typedef enum { PS_NONE, PS_MIN, PS_MAX } wb_power_save_t;
typedef enum { PHY_LAN8720, PHY_IP101, PHY_RTL8201, PHY_DP83848, PHY_KSZ80XX } wb_phy_t;
typedef enum { CLK_EXT_GPIO0, CLK_OUT_GPIO0, CLK_OUT_GPIO16, CLK_OUT_GPIO17 } wb_clk_mode_t;
typedef enum { LOGL_ERROR, LOGL_WARN, LOGL_INFO, LOGL_DEBUG } wb_log_level_t;

typedef struct {
    /* device */
    char name[33];
    char hostname[33];
    /* wifi */
    char ssid1[33], pass1[65];
    char ssid2[33], pass2[65];
    int32_t ap_fallback_min;
    char ap_pass[65];
    int32_t power_save;              /* wb_power_save_t */
    int32_t tx_power;                /* dBm */
    /* network (management address, network byte order) */
    uint32_t ip, netmask, gateway, dns;
    /* bridge */
    char client_mac[18];             /* "" = learn */
    /* ethernet */
    char board[20];
    int32_t phy, phy_addr, mdc, mdio, clk_mode, power_pin;
    /* admin */
    char admin_pass[65];
    /* mqtt */
    bool mqtt_enabled;
    char mqtt_host[65];
    int32_t mqtt_port;
    char mqtt_user[65], mqtt_pass[65], mqtt_topic[65];
    bool mqtt_discovery;
    char mqtt_disc_prefix[33];
    int32_t tele_period;
    /* time */
    char ntp[65], tz[65];
    /* syslog */
    char syslog_host[65];
    int32_t syslog_port;
    /* system */
    int32_t log_level;               /* wb_log_level_t */
    int32_t cpu_mhz;
    int32_t led_pin;
    bool led_inverted;
    int32_t button_pin;
    bool powercycle_reset;
} wb_config_t;

extern wb_config_t g_cfg;

typedef struct {
    const char *id, *name;
    int32_t phy, addr, mdc, mdio, clk_mode, power_pin;
} wb_board_t;
extern const wb_board_t g_boards[];
extern const int g_board_count;

void config_init(void);                         /* load from NVS (defaults for anything missing) */
bool config_is_provisioned(void);               /* Wi-Fi SSID and management IP present */
/* Validate, persist and apply a partial JSON configuration under one lock. On error the
 * active config is unchanged; err describes the failure. *reboot flags changes needing a restart. */
esp_err_t config_update_json(const cJSON *obj, char *err, size_t errlen, bool *reboot);
/* Serialise. secrets=false: replace secrets with "<key>_set" booleans (API). secrets=true: backup. */
cJSON *config_to_json(bool secrets);
/* Console: set field by command name ("SSID1", "MqttHost", ...). Returns 1 handled, 0 unknown, -1 error. */
int config_cmd(const char *cmd, const char *arg, char *out, size_t outlen, bool *reboot);
void config_cmd_list(char *out, size_t outlen);
esp_err_t config_reset(bool wifi_only);
const char *config_default_hostname(void);
void config_apply_board(const char *id);

/* Small runtime flags in NVS (not part of the config) */
bool nvs_blob_get(const char *key, void *buf, size_t len);   /* false if missing or a different size */
void nvs_blob_set(const char *key, const void *buf, size_t len);
int32_t nvs_flag_get(const char *key, int32_t def);
void nvs_flag_set(const char *key, int32_t val);
