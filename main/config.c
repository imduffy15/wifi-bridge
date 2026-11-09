/*
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/ip4_addr.h"
#include "config.h"

static const char *TAG = "config";
static const char *NS = "wifibridge";

wb_config_t g_cfg;
static SemaphoreHandle_t s_lock;
static char s_default_host[33];

const wb_board_t g_boards[] = {
    /* id               name                              phy          addr mdc mdio clk             power */
    { "wt32-eth01",     "WT32-ETH01",                     PHY_LAN8720, 1,   23, 18,  CLK_EXT_GPIO0,  16 },
    { "olimex-poe",     "Olimex ESP32-POE / POE-ISO",     PHY_LAN8720, 0,   23, 18,  CLK_OUT_GPIO17, 12 },
    { "olimex-gateway", "Olimex ESP32-Gateway",           PHY_LAN8720, 0,   23, 18,  CLK_OUT_GPIO17, 5 },
    { "esp32-eth-kit",  "Espressif ESP32-Ethernet-Kit",   PHY_IP101,   1,   23, 18,  CLK_EXT_GPIO0,  5 },
    { "wesp32",         "wESP32 (rev 7+, RTL8201)",       PHY_RTL8201, 0,   16, 17,  CLK_EXT_GPIO0,  -1 },
    { "wesp32-lan8720", "wESP32 (rev 5 and older)",       PHY_LAN8720, 0,   16, 17,  CLK_EXT_GPIO0,  -1 },
    { "lilygo-poe",     "LilyGO T-Internet-POE",          PHY_LAN8720, 0,   23, 18,  CLK_OUT_GPIO17, -1 },
    { "custom",         "Custom",                         PHY_LAN8720, 1,   23, 18,  CLK_EXT_GPIO0,  -1 },
};
const int g_board_count = sizeof g_boards / sizeof g_boards[0];

/* ---- field table ---- */

typedef enum { F_STR, F_SECRET, F_BOOL, F_INT, F_IP, F_MAC, F_ENUM, F_HOST } ftype_t;

typedef struct {
    const char *section, *key, *cmd;
    ftype_t type;
    size_t off, size;
    int32_t min, max;                 /* F_INT range, F_STR/F_SECRET min length */
    const char *const *enums;         /* F_ENUM names, NULL-terminated */
    bool reboot;                      /* takes effect after restart */
} field_t;

static const char *const E_PS[] = { "none", "min", "max", NULL };
static const char *const E_PHY[] = { "lan8720", "ip101", "rtl8201", "dp83848", "ksz80xx", NULL };
static const char *const E_CLK[] = { "ext_gpio0", "out_gpio0", "out_gpio16", "out_gpio17", NULL };
static const char *const E_LOG[] = { "error", "warn", "info", "debug", NULL };

#define O(f) offsetof(wb_config_t, f), sizeof(((wb_config_t *)0)->f)
static const field_t FIELDS[] = {
    { "device",  "name",             "DeviceName",   F_STR,    O(name),            1, 0, NULL, false },
    { "device",  "hostname",         "Hostname",     F_HOST,   O(hostname),        1, 0, NULL, true },
    { "wifi",    "ssid1",            "SSID1",        F_STR,    O(ssid1),           0, 0, NULL, true },
    { "wifi",    "password1",        "Password1",    F_SECRET, O(pass1),           0, 0, NULL, true },
    { "wifi",    "ssid2",            "SSID2",        F_STR,    O(ssid2),           0, 0, NULL, true },
    { "wifi",    "password2",        "Password2",    F_SECRET, O(pass2),           0, 0, NULL, true },
    { "wifi",    "ap_fallback_min",  "ApFallback",   F_INT,    O(ap_fallback_min), 0, 1440, NULL, false },
    { "wifi",    "ap_password",      "ApPassword",   F_SECRET, O(ap_pass),         8, 0, NULL, true },
    { "wifi",    "power_save",       "PowerSave",    F_ENUM,   O(power_save),      0, 0, E_PS, false },
    { "wifi",    "tx_power",         "WifiPower",    F_INT,    O(tx_power),        2, 20, NULL, false },
    { "network", "ip",               "IPAddress",    F_IP,     O(ip),              0, 0, NULL, true },
    { "network", "netmask",          "Subnetmask",   F_IP,     O(netmask),         0, 0, NULL, true },
    { "network", "gateway",          "Gateway",      F_IP,     O(gateway),         0, 0, NULL, true },
    { "network", "dns",              "DnsServer",    F_IP,     O(dns),             0, 0, NULL, true },
    { "bridge",  "client_mac",       "ClientMac",    F_MAC,    O(client_mac),      0, 0, NULL, true },
    { "ethernet","board",            "EthBoard",     F_STR,    O(board),           1, 0, NULL, true },
    { "ethernet","phy",              "EthType",      F_ENUM,   O(phy),             0, 0, E_PHY, true },
    { "ethernet","addr",             "EthAddress",   F_INT,    O(phy_addr),        0, 31, NULL, true },
    { "ethernet","mdc",              "EthMdc",       F_INT,    O(mdc),             0, 39, NULL, true },
    { "ethernet","mdio",             "EthMdio",      F_INT,    O(mdio),            0, 39, NULL, true },
    { "ethernet","clk_mode",         "EthClockMode", F_ENUM,   O(clk_mode),        0, 0, E_CLK, true },
    { "ethernet","power_pin",        "EthPower",     F_INT,    O(power_pin),       -1, 39, NULL, true },
    { "admin",   "password",         "WebPassword",  F_SECRET, O(admin_pass),      0, 0, NULL, false },
    { "mqtt",    "enabled",          "MqttEnable",   F_BOOL,   O(mqtt_enabled),    0, 0, NULL, true },
    { "mqtt",    "host",             "MqttHost",     F_STR,    O(mqtt_host),       0, 0, NULL, true },
    { "mqtt",    "port",             "MqttPort",     F_INT,    O(mqtt_port),       1, 65535, NULL, true },
    { "mqtt",    "user",             "MqttUser",     F_STR,    O(mqtt_user),       0, 0, NULL, true },
    { "mqtt",    "password",         "MqttPassword", F_SECRET, O(mqtt_pass),       0, 0, NULL, true },
    { "mqtt",    "topic",            "Topic",        F_STR,    O(mqtt_topic),      1, 0, NULL, true },
    { "mqtt",    "discovery",        "HassDiscovery",F_BOOL,   O(mqtt_discovery),  0, 0, NULL, true },
    { "mqtt",    "discovery_prefix", "HassPrefix",   F_STR,    O(mqtt_disc_prefix),1, 0, NULL, true },
    { "mqtt",    "tele_period",      "TelePeriod",   F_INT,    O(tele_period),     0, 3600, NULL, false },
    { "time",    "ntp",              "NtpServer",    F_STR,    O(ntp),             0, 0, NULL, true },
    { "time",    "tz",               "Timezone",     F_STR,    O(tz),              1, 0, NULL, false },
    { "syslog",  "host",             "SysLogHost",   F_STR,    O(syslog_host),     0, 0, NULL, false },
    { "syslog",  "port",             "SysLogPort",   F_INT,    O(syslog_port),     1, 65535, NULL, false },
    { "system",  "log_level",        "LogLevel",     F_ENUM,   O(log_level),       0, 0, E_LOG, false },
    { "system",  "cpu_mhz",          "CpuFrequency", F_INT,    O(cpu_mhz),         80, 240, NULL, false },
    { "system",  "led_pin",          "LedPin",       F_INT,    O(led_pin),         -1, 39, NULL, true },
    { "system",  "led_inverted",     "LedInverted",  F_BOOL,   O(led_inverted),    0, 0, NULL, false },
    { "system",  "button_pin",       "ButtonPin",    F_INT,    O(button_pin),      -1, 39, NULL, true },
    { "system",  "powercycle_reset", "PowerCycleReset", F_BOOL, O(powercycle_reset), 0, 0, NULL, false },
};
#define NFIELDS (sizeof FIELDS / sizeof FIELDS[0])

static bool valid_hostname(const char *s);
static esp_err_t save_config(const wb_config_t *cfg);

static void set_defaults(wb_config_t *c)
{
    memset(c, 0, sizeof *c);
    strlcpy(c->name, "WiFi Bridge", sizeof c->name);
    strlcpy(c->hostname, s_default_host, sizeof c->hostname);
    c->ap_fallback_min = 3;
    c->power_save = PS_NONE;
    c->tx_power = 20;
    c->netmask = ipaddr_addr("255.255.255.0");
    strlcpy(c->board, CONFIG_WB_DEFAULT_BOARD, sizeof c->board);
    for (int i = 0; i < g_board_count; i++) {
        if (strcmp(g_boards[i].id, c->board) == 0) {
            c->phy = g_boards[i].phy; c->phy_addr = g_boards[i].addr; c->mdc = g_boards[i].mdc;
            c->mdio = g_boards[i].mdio; c->clk_mode = g_boards[i].clk_mode; c->power_pin = g_boards[i].power_pin;
        }
    }
    c->mqtt_port = 1883;
    strlcpy(c->mqtt_topic, s_default_host, sizeof c->mqtt_topic);
    c->mqtt_discovery = true;
    strlcpy(c->mqtt_disc_prefix, "homeassistant", sizeof c->mqtt_disc_prefix);
    c->tele_period = 300;
    strlcpy(c->ntp, "pool.ntp.org", sizeof c->ntp);
    strlcpy(c->tz, "UTC0", sizeof c->tz);
    c->syslog_port = 514;
    c->log_level = LOGL_INFO;
    c->cpu_mhz = 240;
    c->led_pin = CONFIG_WB_DEFAULT_LED_PIN;
    c->button_pin = CONFIG_WB_DEFAULT_BUTTON_PIN;
    c->powercycle_reset = true;
    /* optional build-time presets (sdkconfig.secrets) */
    if (CONFIG_WB_PRESET_HOSTNAME[0] && valid_hostname(CONFIG_WB_PRESET_HOSTNAME)) {
        strlcpy(c->hostname, CONFIG_WB_PRESET_HOSTNAME, sizeof c->hostname);
        strlcpy(c->mqtt_topic, CONFIG_WB_PRESET_HOSTNAME, sizeof c->mqtt_topic);
    }
    strlcpy(c->ssid1, CONFIG_WB_PRESET_SSID, sizeof c->ssid1);
    strlcpy(c->pass1, CONFIG_WB_PRESET_PASSWORD, sizeof c->pass1);
    strlcpy(c->admin_pass, CONFIG_WB_PRESET_ADMIN_PASSWORD, sizeof c->admin_pass);
    c->ip = CONFIG_WB_PRESET_IP[0] ? ipaddr_addr(CONFIG_WB_PRESET_IP) : 0;
    if (CONFIG_WB_PRESET_NETMASK[0]) {
        c->netmask = ipaddr_addr(CONFIG_WB_PRESET_NETMASK);
    }
    c->gateway = CONFIG_WB_PRESET_GATEWAY[0] ? ipaddr_addr(CONFIG_WB_PRESET_GATEWAY) : 0;
    c->dns = CONFIG_WB_PRESET_DNS[0] ? ipaddr_addr(CONFIG_WB_PRESET_DNS) : 0;
    if (c->ip == IPADDR_NONE || c->gateway == IPADDR_NONE || c->dns == IPADDR_NONE) {
        c->ip = c->gateway = c->dns = 0;          /* malformed preset: fall back to setup mode */
    }
}

const char *config_default_hostname(void) { return s_default_host; }

static void apply_board(wb_config_t *cfg, const char *id)
{
    for (int i = 0; i < g_board_count; i++) {
        if (strcmp(g_boards[i].id, id) == 0 && strcmp(id, "custom") != 0) {
            cfg->phy = g_boards[i].phy; cfg->phy_addr = g_boards[i].addr; cfg->mdc = g_boards[i].mdc;
            cfg->mdio = g_boards[i].mdio; cfg->clk_mode = g_boards[i].clk_mode; cfg->power_pin = g_boards[i].power_pin;
        }
    }
}

void config_apply_board(const char *id) { apply_board(&g_cfg, id); }

/* ---- value parsing / validation (into a scratch config, so a failed request changes nothing) ---- */

static bool valid_hostname(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 32 || s[0] == '-' || s[n - 1] == '-') {
        return false;
    }
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '-') {
            return false;
        }
    }
    return true;
}

static bool valid_mac(const char *s)
{
    if (!*s) {
        return true;
    }
    unsigned m[6];
    if (strlen(s) != 17) {
        return false;
    }
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2 ? s[i] != ':' : !isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6 &&
           !(m[0] & 1) && (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]);
}

static int enum_index(const char *const *e, const char *s)
{
    for (int i = 0; e[i]; i++) {
        if (strcasecmp(e[i], s) == 0) {
            return i;
        }
    }
    return -1;
}

/* Set field f in c from a JSON value or a console string (exactly one is non-NULL). */
static bool set_field(wb_config_t *c, const field_t *f, const cJSON *jv, const char *sv, char *err, size_t errlen)
{
    uint8_t *p = (uint8_t *)c + f->off;
    char tmp[80];
    const char *str = sv;
    if (jv) {
        if (cJSON_IsString(jv)) {
            str = jv->valuestring;
        } else if (cJSON_IsNumber(jv)) {
            snprintf(tmp, sizeof tmp, "%ld", (long)jv->valuedouble);
            str = tmp;
        } else if (cJSON_IsBool(jv)) {
            str = cJSON_IsTrue(jv) ? "1" : "0";
        } else if (cJSON_IsNull(jv) && (f->type == F_IP || f->type == F_STR || f->type == F_MAC || f->type == F_SECRET)) {
            str = "";
        } else {
            snprintf(err, errlen, "%s.%s: wrong type", f->section, f->key);
            return false;
        }
    }
    switch (f->type) {
    case F_STR: case F_SECRET: case F_HOST: case F_MAC: {
        size_t n = strlen(str);
        if (n >= f->size) {
            snprintf(err, errlen, "%s.%s: too long (max %u)", f->section, f->key, (unsigned)f->size - 1);
            return false;
        }
        if (n < (size_t)f->min && !(f->type == F_SECRET && n == 0)) {
            snprintf(err, errlen, "%s.%s: too short (min %ld)", f->section, f->key, (long)f->min);
            return false;
        }
        if (f->type == F_HOST && !valid_hostname(str)) {
            snprintf(err, errlen, "%s.%s: letters, digits and '-' only", f->section, f->key);
            return false;
        }
        if (f->type == F_MAC && !valid_mac(str)) {
            snprintf(err, errlen, "%s.%s: expected a nonzero unicast MAC, e.g. aa:bb:cc:dd:ee:ff", f->section, f->key);
            return false;
        }
        strlcpy((char *)p, str, f->size);
        if (f->type == F_MAC) {
            for (char *q = (char *)p; *q; q++) {
                *q = tolower((unsigned char)*q);
            }
        }
        return true;
    }
    case F_BOOL: {
        bool v = strcmp(str, "1") == 0 || strcasecmp(str, "on") == 0 || strcasecmp(str, "true") == 0;
        if (!v && !(strcmp(str, "0") == 0 || strcasecmp(str, "off") == 0 || strcasecmp(str, "false") == 0)) {
            snprintf(err, errlen, "%s.%s: expected 0/1", f->section, f->key);
            return false;
        }
        *(bool *)p = v;
        return true;
    }
    case F_INT: {
        char *end;
        long v = strtol(str, &end, 10);
        if (!*str || *end || v < f->min || v > f->max) {
            snprintf(err, errlen, "%s.%s: expected %ld..%ld", f->section, f->key, (long)f->min, (long)f->max);
            return false;
        }
        *(int32_t *)p = (int32_t)v;
        return true;
    }
    case F_IP: {
        ip4_addr_t a = { 0 };
        if (*str && !ip4addr_aton(str, &a)) {
            snprintf(err, errlen, "%s.%s: not an IPv4 address", f->section, f->key);
            return false;
        }
        *(uint32_t *)p = a.addr;
        return true;
    }
    case F_ENUM: {
        int i = enum_index(f->enums, str);
        if (i < 0) {
            char *e = tmp;
            for (int k = 0; f->enums[k]; k++) {
                e += snprintf(e, sizeof tmp - (e - tmp), "%s%s", k ? "|" : "", f->enums[k]);
            }
            snprintf(err, errlen, "%s.%s: expected %s", f->section, f->key, tmp);
            return false;
        }
        *(int32_t *)p = i;
        return true;
    }
    }
    return false;
}

static bool put_field(cJSON *sec, const wb_config_t *c, const field_t *f, bool secrets)
{
    const uint8_t *p = (const uint8_t *)c + f->off;
    switch (f->type) {
    case F_SECRET:
        if (secrets) {
            return cJSON_AddStringToObject(sec, f->key, (const char *)p) != NULL;
        } else {
            char k[40];
            snprintf(k, sizeof k, "%s_set", f->key);
            return cJSON_AddBoolToObject(sec, k, ((const char *)p)[0] != 0) != NULL;
        }
        break;
    case F_STR: case F_HOST: case F_MAC:
        return cJSON_AddStringToObject(sec, f->key, (const char *)p) != NULL;
        break;
    case F_BOOL:
        return cJSON_AddBoolToObject(sec, f->key, *(const bool *)p) != NULL;
        break;
    case F_INT:
        return cJSON_AddNumberToObject(sec, f->key, *(const int32_t *)p) != NULL;
        break;
    case F_IP: {
        ip4_addr_t a = { .addr = *(const uint32_t *)p };
        return cJSON_AddStringToObject(sec, f->key, a.addr ? ip4addr_ntoa(&a) : "") != NULL;
        break;
    }
    case F_ENUM:
        return cJSON_AddStringToObject(sec, f->key, f->enums[*(const int32_t *)p]) != NULL;
        break;
    }
    return false;
}

static cJSON *serialize_config(const wb_config_t *cfg, bool secrets)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    for (size_t i = 0; i < NFIELDS; i++) {
        cJSON *sec = cJSON_GetObjectItem(root, FIELDS[i].section);
        if (!sec) {
            sec = cJSON_AddObjectToObject(root, FIELDS[i].section);
        }
        if (!sec || !put_field(sec, cfg, &FIELDS[i], secrets)) {
            cJSON_Delete(root);
            return NULL;
        }
    }
    return root;
}

cJSON *config_to_json(bool secrets)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *root = serialize_config(&g_cfg, secrets);
    xSemaphoreGive(s_lock);
    return root;
}

/* ESP32 pins usable for our LED/button/PHY-power/SMI: exists, not flash (6-11), not the RMII data/clock pins,
 * not UART0 (console); outputs can't be 34-39 */
static bool pin_ok(int32_t p, bool output)
{
    static const int8_t reserved[] = { 0, 1, 3, 6, 7, 8, 9, 10, 11, 19, 21, 22, 25, 26, 27 };
    if (p < 0 || p > 39 || p == 20 || p == 24 || (p >= 28 && p <= 31) || (output && p >= 34)) {
        return false;
    }
    for (size_t i = 0; i < sizeof reserved; i++) {
        if (p == reserved[i]) {
            return false;
        }
    }
    return true;
}

static bool check_pin(const char *name, int32_t p, bool output, bool allow_none, char *err, size_t errlen)
{
    if ((allow_none && p == -1) || pin_ok(p, output)) {
        return true;
    }
    /* the button needs the internal pull-up, which GPIO 34-39 don't have: checked like an output */
    snprintf(err, errlen, "%s: GPIO %ld can't be used (doesn't exist, is reserved for flash/Ethernet/serial%s)",
             name, (long)p, output ? ", or has no internal pull-up/output" : "");
    return false;
}

/* cross-field checks on a candidate config */
static bool validate(const wb_config_t *c, char *err, size_t errlen)
{
    if (!check_pin("system.led_pin", c->led_pin, true, true, err, errlen) ||
        !check_pin("system.button_pin", c->button_pin, true, true, err, errlen) ||
        !check_pin("ethernet.power_pin", c->power_pin, true, true, err, errlen) ||
        !check_pin("ethernet.mdc", c->mdc, true, false, err, errlen) ||
        !check_pin("ethernet.mdio", c->mdio, true, false, err, errlen)) {
        return false;
    }
    if (c->tele_period > 0 && c->tele_period < 10) {
        snprintf(err, errlen, "mqtt.tele_period: 0 (off) or 10 to 3600 seconds");
        return false;
    }
    /* the RMII clock output pin (GPIO16/17 when the ESP32 drives the PHY clock) is taken too */
    int32_t clk = c->clk_mode == CLK_OUT_GPIO16 ? 16 : c->clk_mode == CLK_OUT_GPIO17 ? 17 : -1;
    int32_t pins[] = { c->led_pin, c->button_pin, c->power_pin, c->mdc, c->mdio, clk };
    for (int i = 0; i < 6; i++) {
        for (int k = i + 1; k < 6; k++) {
            if (pins[i] >= 0 && pins[i] == pins[k]) {
                snprintf(err, errlen, "GPIO %ld is used twice (LED, button, PHY power, MDC, MDIO and RMII clock must differ)", (long)pins[i]);
                return false;
            }
        }
    }

    bool known_board = false;
    for (int i = 0; i < g_board_count; i++) {
        known_board |= strcmp(c->board, g_boards[i].id) == 0;
    }
    if (!known_board) {
        snprintf(err, errlen, "ethernet.board: unknown board preset");
        return false;
    }
    uint32_t mask = lwip_ntohl(c->netmask), hostmask = ~mask;
    if (c->netmask && (!mask || (hostmask & (hostmask + 1)) || hostmask < 3)) {
        snprintf(err, errlen, "network.netmask: use a contiguous subnet mask from /1 to /30");
        return false;
    }
    if (c->ip) {
        if (!c->netmask) {
            snprintf(err, errlen, "network.netmask: required with a static address");
            return false;
        }
        uint32_t ip = lwip_ntohl(c->ip), gateway = lwip_ntohl(c->gateway);
        if (!(ip >> 24) || (ip >> 24) == 127 || ip >= 0xe0000000 ||
            !(ip & hostmask) || (ip & hostmask) == hostmask) {
            snprintf(err, errlen, "network.ip: use a host address, not a network, broadcast, loopback or multicast address");
            return false;
        }
        if (gateway && (!(gateway >> 24) || (gateway >> 24) == 127 || gateway >= 0xe0000000 ||
                        !(gateway & hostmask) || (gateway & hostmask) == hostmask)) {
            snprintf(err, errlen, "network.gateway: use a valid host address");
            return false;
        }
        if (c->gateway && (c->gateway & c->netmask) != (c->ip & c->netmask)) {
            snprintf(err, errlen, "network.gateway: not in the same subnet as network.ip");
            return false;
        }
        if (c->ip == c->gateway) {
            snprintf(err, errlen, "network.ip: must differ from the gateway");
            return false;
        }
    }
    if (c->mqtt_enabled && !c->mqtt_host[0]) {
        snprintf(err, errlen, "mqtt.host: required when MQTT is enabled");
        return false;
    }
    if (c->cpu_mhz != 80 && c->cpu_mhz != 160 && c->cpu_mhz != 240) {
        snprintf(err, errlen, "system.cpu_mhz: 80, 160 or 240");
        return false;
    }
    if (c->mdc == c->mdio) {
        snprintf(err, errlen, "ethernet.mdc and ethernet.mdio must differ");
        return false;
    }
    return true;
}

esp_err_t config_update_json(const cJSON *obj, char *err, size_t errlen, bool *reboot)
{
    if (!cJSON_IsObject(obj)) {
        snprintf(err, errlen, "expected a JSON object");
        return ESP_ERR_INVALID_ARG;
    }
    wb_config_t *tmp = malloc(sizeof *tmp);
    if (!tmp) {
        snprintf(err, errlen, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *tmp = g_cfg;
    bool ok = true, rb = false;
    /* a board preset is applied first, so explicit pin fields in the same request still win */
    const cJSON *board = cJSON_GetObjectItem(cJSON_GetObjectItem(obj, "ethernet"), "board");
    if (cJSON_IsString(board)) {
        apply_board(tmp, board->valuestring);
    }
    for (size_t i = 0; ok && i < NFIELDS; i++) {
        const cJSON *section = cJSON_GetObjectItem(obj, FIELDS[i].section);
        if (section && !cJSON_IsObject(section)) {
            snprintf(err, errlen, "%s: expected an object", FIELDS[i].section);
            ok = false;
            break;
        }
        const cJSON *v = cJSON_GetObjectItem(section, FIELDS[i].key);
        if (v) {
            ok = set_field(tmp, &FIELDS[i], v, NULL, err, errlen);
        }
    }
    if (ok) {
        ok = validate(tmp, err, errlen);
    }
    esp_err_t result = ok ? save_config(tmp) : ESP_ERR_INVALID_ARG;
    if (ok && result != ESP_OK) {
        snprintf(err, errlen, "could not save settings; previous settings kept");
    }
    if (result == ESP_OK) {
        for (size_t i = 0; i < NFIELDS; i++) {
            if (FIELDS[i].reboot && memcmp((uint8_t *)tmp + FIELDS[i].off, (uint8_t *)&g_cfg + FIELDS[i].off, FIELDS[i].size)) {
                rb = true;
            }
        }
        g_cfg = *tmp;
    }
    xSemaphoreGive(s_lock);
    free(tmp);
    if (reboot) {
        *reboot = rb;
    }
    return result;
}

int config_cmd(const char *cmd, const char *arg, char *out, size_t outlen, bool *reboot)
{
    for (size_t i = 0; i < NFIELDS; i++) {
        const field_t *f = &FIELDS[i];
        if (strcasecmp(cmd, f->cmd) != 0) {
            continue;
        }
        if (arg && *arg) {
            /* '"' clears a string */
            const char *v = strcmp(arg, "\"") == 0 ? "" : arg;
            cJSON *obj = cJSON_CreateObject();
            cJSON *sec = cJSON_AddObjectToObject(obj, f->section);
            cJSON_AddStringToObject(sec, f->key, v);
            char err[96];
            esp_err_t result = config_update_json(obj, err, sizeof err, reboot);
            cJSON_Delete(obj);
            if (result != ESP_OK) {
                snprintf(out, outlen, "{\"%s\":\"error: %s\"}", f->cmd, err);
                return -1;
            }
        }
        cJSON *o = cJSON_CreateObject();
        cJSON *tmp = cJSON_CreateObject();
        put_field(tmp, &g_cfg, f, false);
        cJSON *val = tmp->child;
        cJSON_DetachItemViaPointer(tmp, val);
        cJSON_AddItemToObject(o, f->cmd, val);
        cJSON_Delete(tmp);
        cJSON_PrintPreallocated(o, out, outlen, false);
        cJSON_Delete(o);
        return 1;
    }
    return 0;
}

void config_cmd_list(char *out, size_t outlen)
{
    size_t n = 0;
    for (size_t i = 0; i < NFIELDS && n < outlen; i++) {
        n += snprintf(out + n, outlen - n, "%s%s", i ? ", " : "", FIELDS[i].cmd);
    }
}

/* ---- persistence ---- */

static esp_err_t save_config(const wb_config_t *cfg)
{
    cJSON *j = serialize_config(cfg, true);
    if (!j) {
        return ESP_ERR_NO_MEM;
    }
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_blob(h, "config", s, strlen(s));
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    free(s);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(e));
    } else {
        ESP_LOGI(TAG, "saved");
    }
    return e;
}

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored configuration, using defaults");
        return;
    }
    size_t len = 0;
    if (nvs_get_blob(h, "config", NULL, &len) == ESP_OK && len > 0) {
        char *s = malloc(len + 1);
        if (s && nvs_get_blob(h, "config", s, &len) == ESP_OK) {
            s[len] = 0;
            cJSON *j = cJSON_Parse(s);
            if (j) {
                /* field by field, so one bad value (e.g. after a downgrade) doesn't lose the rest */
                for (size_t i = 0; i < NFIELDS; i++) {
                    const cJSON *v = cJSON_GetObjectItem(cJSON_GetObjectItem(j, FIELDS[i].section), FIELDS[i].key);
                    char err[96];
                    if (v && !set_field(&g_cfg, &FIELDS[i], v, NULL, err, sizeof err)) {
                        ESP_LOGW(TAG, "ignoring stored %s", err);
                    }
                }
                cJSON_Delete(j);
                ESP_LOGI(TAG, "loaded (%u bytes)", (unsigned)len);
                char err[128];
                if (!validate(&g_cfg, err, sizeof err)) {
                    ESP_LOGE(TAG, "stored settings invalid (%s): disabling LED/button pins, board defaults", err);
                    g_cfg.led_pin = g_cfg.button_pin = -1;
                    bool known = false;
                    for (int i = 0; i < g_board_count; i++) {
                        known |= !strcmp(g_boards[i].id, g_cfg.board) && strcmp(g_cfg.board, "custom");
                    }
                    if (!known) {
                        strlcpy(g_cfg.board, CONFIG_WB_DEFAULT_BOARD, sizeof g_cfg.board);
                    }
                    config_apply_board(g_cfg.board);
                }
            } else {
                ESP_LOGE(TAG, "stored configuration is corrupt, using defaults");
            }
        }
        free(s);
    }
    nvs_close(h);
}

void config_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_default_host, sizeof s_default_host, "%s-%02x%02x%02x", CONFIG_WB_HOSTNAME_PREFIX, mac[3], mac[4], mac[5]);
    set_defaults(&g_cfg);
    load();
}

bool config_is_provisioned(void)
{
    return g_cfg.ssid1[0] && g_cfg.ip && g_cfg.netmask;
}

esp_err_t config_reset(bool wifi_only)
{
    wb_config_t *next = malloc(sizeof *next);
    if (!next) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t result;
    if (wifi_only) {
        *next = g_cfg;
        next->ssid1[0] = next->pass1[0] = next->ssid2[0] = next->pass2[0] = 0;
        next->ip = next->gateway = next->dns = 0;
        result = save_config(next);
    } else {
        set_defaults(next);
        /* Keep other firmware's namespaces intact for recovery. */
        nvs_handle_t h;
        result = nvs_open(NS, NVS_READWRITE, &h);
        if (result == ESP_OK) {
            result = nvs_erase_all(h);
            if (result == ESP_OK) {
                result = nvs_commit(h);
            }
            nvs_close(h);
        }
    }
    if (result == ESP_OK) {
        g_cfg = *next;
        ESP_LOGW(TAG, "%s reset", wifi_only ? "Wi-Fi settings" : "factory");
    } else {
        ESP_LOGE(TAG, "reset failed: %s", esp_err_to_name(result));
    }
    xSemaphoreGive(s_lock);
    free(next);
    return result;
}

int32_t nvs_flag_get(const char *key, int32_t def)
{
    nvs_handle_t h;
    int32_t v = def;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, key, &v);
        nvs_close(h);
    }
    return v;
}

void nvs_flag_set(const char *key, int32_t val)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool nvs_blob_get(const char *key, void *buf, size_t len)
{
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t l = len;
        ok = nvs_get_blob(h, key, buf, &l) == ESP_OK && l == len;
        nvs_close(h);
    }
    return ok;
}

void nvs_blob_set(const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, key, buf, len);
        nvs_commit(h);
        nvs_close(h);
    }
}
