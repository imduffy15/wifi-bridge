/*
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 *
 * MQTT (cmnd/stat/tele topics) and Home Assistant discovery.
 *   tele/<topic>/LWT      Online / Offline (retained)
 *   tele/<topic>/STATE    telemetry every TelePeriod seconds
 *   cmnd/<topic>/<Cmd>    any console command; the result goes to stat/<topic>/RESULT
 */
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "lwip/ip4_addr.h"
#include "config.h"
#include "wb.h"
#include "eth.h"

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static esp_timer_handle_t s_tele;
static char s_lwt[96], s_cmnd_prefix[96];

bool mqtt_connected(void) { return s_connected; }

static void topic(char *buf, size_t n, const char *prefix, const char *suffix)
{
    snprintf(buf, n, "%s/%s/%s", prefix, g_cfg.mqtt_topic, suffix);
}

void mqtt_publish_state(void)
{
    if (!s_connected) {
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "Uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(o, "Heap", esp_get_free_heap_size());
    cJSON *w = cJSON_AddObjectToObject(o, "Wifi");
    wifi_ap_record_t ap;
    if (wifi_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        char bssid[18];
        snprintf(bssid, sizeof bssid, MACSTR, MAC2STR(ap.bssid));
        cJSON_AddStringToObject(w, "SSId", (char *)ap.ssid);
        cJSON_AddStringToObject(w, "BSSId", bssid);
        cJSON_AddNumberToObject(w, "Channel", ap.primary);
        cJSON_AddNumberToObject(w, "RSSI", ap.rssi);
        cJSON_AddNumberToObject(w, "Signal", wifi_quality(ap.rssi));
    }
    cJSON_AddNumberToObject(w, "Disconnects", wifi_disconnects());
    cJSON *e = cJSON_AddObjectToObject(o, "Ethernet");
    cJSON_AddStringToObject(e, "Link", eth_link_up() ? "ON" : "OFF");
    cJSON *c = cJSON_AddObjectToObject(o, "Client");
    uint8_t m[6];
    char mac[18] = "";
    if (eth_client_mac(m)) {
        snprintf(mac, sizeof mac, MACSTR, MAC2STR(m));
    }
    ip4_addr_t ip = { .addr = g_state.frames.client_ip };
    cJSON_AddStringToObject(c, "MAC", mac);
    cJSON_AddStringToObject(c, "IP", ip.addr ? ip4addr_ntoa(&ip) : "");
    cJSON *f = cJSON_AddObjectToObject(o, "Frames");
    cJSON_AddNumberToObject(f, "WiredToWifi", g_state.frames.wired_to_wifi);
    cJSON_AddNumberToObject(f, "WifiToWired", g_state.frames.wifi_to_wired);
    cJSON_AddNumberToObject(f, "Dropped", g_state.frames.wifi_drop + g_state.frames.wired_drop + g_state.frames.wifi_tx_fail);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    char t[96];
    topic(t, sizeof t, "tele", "STATE");
    if (s) {
        esp_mqtt_client_enqueue(s_client, t, s, 0, 0, 0, true);
        free(s);
    }
}

static void tele_cb(void *arg) { mqtt_publish_state(); }

/* ---- Home Assistant discovery ---- */

static void disc(const char *component, const char *object, const char *name, cJSON *extra)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char uid[48], t[160], st[96];
    snprintf(uid, sizeof uid, "%02x%02x%02x%02x%02x%02x_%s", MAC2STR(mac), object);
    snprintf(t, sizeof t, "%s/%s/%s/%s/config", g_cfg.mqtt_disc_prefix, component, g_cfg.hostname, object);
    cJSON *o = extra ? extra : cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddStringToObject(o, "unique_id", uid);
    cJSON_AddStringToObject(o, "object_id", uid);
    cJSON_AddStringToObject(o, "availability_topic", s_lwt);
    cJSON_AddStringToObject(o, "payload_available", "Online");
    cJSON_AddStringToObject(o, "payload_not_available", "Offline");
    if (strcmp(component, "button") != 0) {
        topic(st, sizeof st, "tele", "STATE");
        cJSON_AddStringToObject(o, "state_topic", st);
    }
    cJSON *d = cJSON_AddObjectToObject(o, "device");
    char id[13];
    snprintf(id, sizeof id, "%02x%02x%02x%02x%02x%02x", MAC2STR(mac));
    cJSON_AddItemToArray(cJSON_AddArrayToObject(d, "identifiers"), cJSON_CreateString(id));
    cJSON *cn = cJSON_AddArrayToObject(d, "connections");
    cJSON *pair = cJSON_CreateArray();
    char macs[18];
    snprintf(macs, sizeof macs, MACSTR, MAC2STR(mac));
    cJSON_AddItemToArray(pair, cJSON_CreateString("mac"));
    cJSON_AddItemToArray(pair, cJSON_CreateString(macs));
    cJSON_AddItemToArray(cn, pair);
    cJSON_AddStringToObject(d, "name", g_cfg.name);
    cJSON_AddStringToObject(d, "manufacturer", "WiFi Bridge");
    cJSON_AddStringToObject(d, "model", g_cfg.board);
    cJSON_AddStringToObject(d, "sw_version", esp_app_get_description()->version);
    ip4_addr_t ip = { .addr = g_cfg.ip };
    char url[40];
    snprintf(url, sizeof url, "http://%s/", ip4addr_ntoa(&ip));
    cJSON_AddStringToObject(d, "configuration_url", url);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) {
        esp_mqtt_client_enqueue(s_client, t, s, 0, 1, 1, true);
        free(s);
    }
}

static cJSON *sensor(const char *tmpl, const char *unit, const char *dev_class, const char *state_class, bool diag)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "value_template", tmpl);
    if (unit) cJSON_AddStringToObject(o, "unit_of_measurement", unit);
    if (dev_class) cJSON_AddStringToObject(o, "device_class", dev_class);
    if (state_class) cJSON_AddStringToObject(o, "state_class", state_class);
    if (diag) cJSON_AddStringToObject(o, "entity_category", "diagnostic");
    return o;
}

static void publish_discovery(void)
{
    disc("sensor", "rssi", "Wi-Fi signal", sensor("{{ value_json.Wifi.RSSI }}", "dBm", "signal_strength", "measurement", true));
    disc("sensor", "ssid", "Wi-Fi network", sensor("{{ value_json.Wifi.SSId }}", NULL, NULL, NULL, true));
    disc("sensor", "uptime", "Uptime", sensor("{{ value_json.Uptime }}", "s", "duration", NULL, true));
    disc("sensor", "heap", "Free memory", sensor("{{ value_json.Heap }}", "B", "data_size", "measurement", true));
    disc("sensor", "client_ip", "Wired client IP", sensor("{{ value_json.Client.IP }}", NULL, NULL, NULL, false));
    disc("sensor", "client_mac", "Wired client MAC", sensor("{{ value_json.Client.MAC }}", NULL, NULL, NULL, true));
    disc("sensor", "wired_to_wifi", "Frames wired to Wi-Fi", sensor("{{ value_json.Frames.WiredToWifi }}", "frames", NULL, "total_increasing", true));
    disc("sensor", "wifi_to_wired", "Frames Wi-Fi to wired", sensor("{{ value_json.Frames.WifiToWired }}", "frames", NULL, "total_increasing", true));
    disc("sensor", "dropped", "Dropped frames", sensor("{{ value_json.Frames.Dropped }}", "frames", NULL, "total_increasing", true));
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "value_template", "{{ value_json.Ethernet.Link }}");
    cJSON_AddStringToObject(b, "device_class", "connectivity");
    cJSON_AddStringToObject(b, "payload_on", "ON");
    cJSON_AddStringToObject(b, "payload_off", "OFF");
    disc("binary_sensor", "eth_link", "Ethernet link", b);
    cJSON *r = cJSON_CreateObject();
    char ct[96];
    topic(ct, sizeof ct, "cmnd", "Restart");
    cJSON_AddStringToObject(r, "command_topic", ct);
    cJSON_AddStringToObject(r, "payload_press", "1");
    cJSON_AddStringToObject(r, "device_class", "restart");
    cJSON_AddStringToObject(r, "entity_category", "config");
    disc("button", "restart", "Restart", r);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        s_connected = true;
        ESP_LOGI(TAG, "connected to %s:%ld", g_cfg.mqtt_host, (long)g_cfg.mqtt_port);
        esp_mqtt_client_publish(s_client, s_lwt, "Online", 0, 1, 1);
        char sub[100];
        snprintf(sub, sizeof sub, "%s#", s_cmnd_prefix);
        esp_mqtt_client_subscribe(s_client, sub, 0);
        if (g_cfg.mqtt_discovery) {
            publish_discovery();
        }
        mqtt_publish_state();
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) {
            ESP_LOGW(TAG, "disconnected");
        }
        s_connected = false;
        break;
    case MQTT_EVENT_DATA: {
        size_t pl = strlen(s_cmnd_prefix);
        if (ev->topic_len <= (int)pl || ev->topic_len - pl > 32 || strncmp(ev->topic, s_cmnd_prefix, pl) != 0 ||
            ev->current_data_offset != 0 || ev->data_len > 200) {
            break;
        }
        char line[240];
        int n = snprintf(line, sizeof line, "%.*s", (int)(ev->topic_len - pl), ev->topic + pl);
        if (ev->data_len) {
            snprintf(line + n, sizeof line - n, " %.*s", ev->data_len, ev->data);
        }
        char *out = malloc(2048);
        if (out) {
            console_exec(line, out, 2048, "mqtt");
            char t[96];
            topic(t, sizeof t, "stat", "RESULT");
            esp_mqtt_client_enqueue(s_client, t, out, 0, 0, 0, true);
            free(out);
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        if (ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGW(TAG, "connection refused (code %d): check user/password", ev->error_handle->connect_return_code);
        }
        break;
    default:
        break;
    }
}

void mqtt_start(void)
{
    if (!g_cfg.mqtt_enabled || !g_cfg.mqtt_host[0]) {
        return;
    }
    topic(s_lwt, sizeof s_lwt, "tele", "LWT");
    snprintf(s_cmnd_prefix, sizeof s_cmnd_prefix, "cmnd/%s/", g_cfg.mqtt_topic);
    esp_mqtt_client_config_t c = {
        .broker.address.hostname = g_cfg.mqtt_host,
        .broker.address.port = g_cfg.mqtt_port,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.client_id = g_cfg.hostname,
        .credentials.username = g_cfg.mqtt_user[0] ? g_cfg.mqtt_user : NULL,
        .credentials.authentication.password = g_cfg.mqtt_pass[0] ? g_cfg.mqtt_pass : NULL,
        .session.last_will.topic = s_lwt,
        .session.last_will.msg = "Offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 10000,
        .task.stack_size = 8192,
        .buffer.size = 1536,
    };
    s_client = esp_mqtt_client_init(&c);
    if (!s_client) {
        ESP_LOGE(TAG, "init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_mqtt_client_start(s_client);
    esp_timer_create_args_t ta = { .callback = tele_cb, .name = "tele" };
    esp_timer_create(&ta, &s_tele);
    mqtt_apply_runtime();
}

void mqtt_apply_runtime(void)
{
    if (s_tele) {
        esp_timer_stop(s_tele);
        if (g_cfg.tele_period > 0) {         /* 0: telemetry only on request */
            esp_timer_start_periodic(s_tele, (uint64_t)g_cfg.tele_period * 1000000ULL);
        }
    }
}
