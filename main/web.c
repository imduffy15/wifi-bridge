/*
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 *
 * Web server: single-page UI, JSON API (docs/API.md), captive-portal redirects.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "lwip/sockets.h"
#include "lwip/ip4_addr.h"
#include "config.h"
#include "logbuf.h"
#include "wb.h"
#include "web_util.h"

static const char *TAG = "web";

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");


/* Requests for foreign hosts (captive-portal probes) while the setup AP or setup mode is active */
static bool captive_redirect(httpd_req_t *req)
{
    if (g_state.mode != MODE_SETUP && !wifi_ap_active()) {
        return false;
    }
    char host[64] = "", me[16];
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof host);
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
    }
    local_ip(req, me, sizeof me);
    char mdns[48];
    snprintf(mdns, sizeof mdns, "%s.local", g_cfg.hostname);
#if CONFIG_WB_QEMU
    if (strcmp(host, "127.0.0.1") == 0 || strcmp(host, "localhost") == 0) {
        return false;                  /* QEMU port forwarding: the browser sees the forwarded address */
    }
#endif
    if (!host[0] || strcmp(host, me) == 0 || strcasecmp(host, mdns) == 0 || strcasecmp(host, g_cfg.hostname) == 0) {
        return false;
    }
    char loc[40];
    snprintf(loc, sizeof loc, "http://%s/", me);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, NULL, 0);
    return true;
}

/* ---------- pages ---------- */

static esp_err_t h_index(httpd_req_t *req)
{
    if (captive_redirect(req)) {
        return ESP_OK;
    }
    AUTH();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    /* no framing by other sites (clickjacking) */
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "frame-ancestors 'none'");
    return httpd_resp_send(req, (const char *)index_html_gz_start, index_html_gz_end - index_html_gz_start);
}

static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t err)
{
    if (captive_redirect(req)) {
        return ESP_OK;
    }
    if (g_state.mode == MODE_SETUP || wifi_ap_active()) {
        /* phones probe paths like /generate_204 on our own address too */
        char me[16], loc[40];
        local_ip(req, me, sizeof me);
        snprintf(loc, sizeof loc, "http://%s/", me);
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", loc);
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    send_err(req, "404 Not Found", "not found");
    return ESP_OK;
}

/* ---------- API ---------- */

static esp_err_t h_status(httpd_req_t *req)
{
    AUTH();
    wb_mark_healthy("web UI reachable");
    g_state.last_web_us = esp_timer_get_time();
    cJSON *o = cJSON_CreateObject();
    sys_status_json(o);
    return send_json(req, o);
}

static esp_err_t h_config_get(httpd_req_t *req)
{
    AUTH();
    return send_json(req, config_to_json(false));
}

static esp_err_t h_config_post(httpd_req_t *req)
{
    AUTH();
    cJSON *j = read_json(req, 4096);
    if (!j) {
        return ESP_OK;
    }
    char err[128];
    bool reboot = false;
    esp_err_t result = config_update_json(j, err, sizeof err, &reboot);
    cJSON_Delete(j);
    if (result != ESP_OK) {
        return send_err(req, result == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "500 Internal Server Error", err);
    }
    if (reboot) {
        g_state.reboot_required = true;
    }
    sys_apply_runtime();
    wifi_apply_runtime();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "reboot_required", g_state.reboot_required);
    return send_json(req, o);
}

static esp_err_t h_boards(httpd_req_t *req)
{
    AUTH();
    static const char *phy[] = { "lan8720", "ip101", "rtl8201", "dp83848", "ksz80xx" };
    static const char *clk[] = { "ext_gpio0", "out_gpio0", "out_gpio16", "out_gpio17" };
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < g_board_count; i++) {
        const wb_board_t *b = &g_boards[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", b->id);
        cJSON_AddStringToObject(o, "name", b->name);
        cJSON_AddStringToObject(o, "phy", phy[b->phy]);
        cJSON_AddNumberToObject(o, "addr", b->addr);
        cJSON_AddNumberToObject(o, "mdc", b->mdc);
        cJSON_AddNumberToObject(o, "mdio", b->mdio);
        cJSON_AddStringToObject(o, "clk_mode", clk[b->clk_mode]);
        cJSON_AddNumberToObject(o, "power_pin", b->power_pin);
        cJSON_AddItemToArray(a, o);
    }
    return send_json(req, a);
}

static esp_err_t h_setup(httpd_req_t *req)
{
    AUTH();
    cJSON *j = read_json(req, 2048);
    if (!j) {
        return ESP_OK;
    }
    const cJSON *ssid = cJSON_GetObjectItem(j, "ssid"), *pass = cJSON_GetObjectItem(j, "password");
    const cJSON *host = cJSON_GetObjectItem(j, "hostname"), *apw = cJSON_GetObjectItem(j, "admin_password");
    const cJSON *net = cJSON_GetObjectItem(j, "network");
    if ((pass && !cJSON_IsString(pass)) || (host && !cJSON_IsString(host)) ||
        (apw && !cJSON_IsString(apw))) {
        cJSON_Delete(j);
        return send_err(req, "400 Bad Request", "password, hostname and admin_password must be strings");
    }
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) {
        cJSON_Delete(j);
        return send_err(req, "400 Bad Request", "ssid: required");
    }
    if (!cJSON_IsObject(net) || !cJSON_IsString(cJSON_GetObjectItem(net, "ip")) ||
        !cJSON_GetObjectItem(net, "ip")->valuestring[0]) {
        cJSON_Delete(j);
        return send_err(req, "400 Bad Request", "network.ip: required");
    }
    ip4_addr_t management;
    if (!ip4addr_aton(cJSON_GetObjectItem(net, "ip")->valuestring, &management) || !management.addr) {
        cJSON_Delete(j);
        return send_err(req, "400 Bad Request", "network.ip: a nonzero management address is required");
    }
    cJSON *c = cJSON_CreateObject();
    cJSON *w = cJSON_AddObjectToObject(c, "wifi");
    cJSON_AddStringToObject(w, "ssid1", ssid->valuestring);
    cJSON_AddStringToObject(w, "password1", cJSON_IsString(pass) ? pass->valuestring : "");
    if (cJSON_IsString(host) && host->valuestring[0]) {
        cJSON_AddStringToObject(cJSON_AddObjectToObject(c, "device"), "hostname", host->valuestring);
        if (strcmp(g_cfg.mqtt_topic, config_default_hostname()) == 0) {
            cJSON_AddStringToObject(cJSON_AddObjectToObject(c, "mqtt"), "topic", host->valuestring);
        }
    }
    if (cJSON_IsString(apw) && apw->valuestring[0]) {
        cJSON_AddStringToObject(cJSON_AddObjectToObject(c, "admin"), "password", apw->valuestring);
    }
    cJSON_AddItemToObject(c, "network", cJSON_Duplicate(net, true));
    cJSON_Delete(j);
    char err[128];
    esp_err_t result = config_update_json(c, err, sizeof err, NULL);
    cJSON_Delete(c);
    if (result != ESP_OK) {
        return send_err(req, result == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "500 Internal Server Error", err);
    }
    if (!config_is_provisioned()) {
        return send_err(req, "400 Bad Request", "incomplete settings");
    }
    nvs_flag_set("setup_once", 0);
    ip4_addr_t a = { .addr = g_cfg.ip };
    char url[48], mdns[64];
    snprintf(url, sizeof url, "http://%s/", ip4addr_ntoa(&a));
    snprintf(mdns, sizeof mdns, "http://%s.local/", g_cfg.hostname);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "url", url);
    cJSON_AddStringToObject(o, "mdns", mdns);
    ESP_LOGI(TAG, "setup complete: Wi-Fi \"%s\", management address %s", g_cfg.ssid1, url);
    wb_restart("setup complete", 2500);
    return send_json(req, o);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    AUTH();
    wifi_scan_start();
    return send_json(req, wifi_scan_json());
}

static esp_err_t h_test_get(httpd_req_t *req)
{
    AUTH();
    return send_json(req, wifi_test_json());
}

static esp_err_t h_test_post(httpd_req_t *req)
{
    AUTH();
    cJSON *j = read_json(req, 512);
    if (!j) {
        return ESP_OK;
    }
    const cJSON *ssid = cJSON_GetObjectItem(j, "ssid"), *pass = cJSON_GetObjectItem(j, "password");
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(ssid) && ssid->valuestring[0] && strlen(ssid->valuestring) <= 32 &&
        (!pass || (cJSON_IsString(pass) && strlen(pass->valuestring) <= 64))) {
        e = wifi_test_start(ssid->valuestring, pass ? pass->valuestring : "");
    }
    cJSON_Delete(j);
    if (e == ESP_ERR_INVALID_STATE) {
        return send_err(req, "409 Conflict", g_state.mode == MODE_SETUP ? "a test is already running" : "only available in setup mode");
    }
    if (e != ESP_OK) {
        return send_err(req, "400 Bad Request", "ssid (1-32 chars) and password (0-64 chars) required");
    }
    return send_ok(req);
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
    if (strcmp(target, "normal") && strcmp(target, "safeboot") && strcmp(target, "setup")) {
        cJSON_Delete(j);
        return send_err(req, "400 Bad Request", "target: normal, setup or safeboot");
    }
    char tg[12];
    strlcpy(tg, target, sizeof tg);
    cJSON_Delete(j);
    if (wb_reboot_to(tg, 1500) != ESP_OK) {
        return send_err(req, "409 Conflict", "no valid safeboot firmware on this device");
    }
    return send_ok(req);
}

static esp_err_t h_reset(httpd_req_t *req)
{
    AUTH();
    cJSON *j = read_json(req, 256);
    if (!j) {
        return ESP_OK;
    }
    const cJSON *s = cJSON_GetObjectItem(j, "scope");
    bool wifi_only = cJSON_IsString(s) && strcmp(s->valuestring, "wifi") == 0;
    bool all = cJSON_IsString(s) && strcmp(s->valuestring, "all") == 0;
    cJSON_Delete(j);
    if (!wifi_only && !all) {
        return send_err(req, "400 Bad Request", "scope: all or wifi");
    }
    if (config_reset(wifi_only) != ESP_OK) {
        return send_err(req, "500 Internal Server Error", "could not reset settings");
    }
    wb_restart("settings reset", 1500);
    return send_ok(req);
}

static esp_err_t h_backup(httpd_req_t *req)
{
    AUTH();
    char disp[96];
    snprintf(disp, sizeof disp, "attachment; filename=\"wifi-bridge-%s.json\"", g_cfg.hostname);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    /* without an admin password anyone on the network (or the open fallback AP) could read the Wi-Fi
     * password from here: only include secrets when the backup itself is password protected */
    bool secrets = g_cfg.admin_pass[0] != 0;
    cJSON *o = config_to_json(secrets);
    cJSON_AddStringToObject(o, "_firmware", "wifi-bridge");
    if (!secrets) {
        cJSON_AddStringToObject(o, "_note", "passwords are only included when an admin password is set");
    }
    char *s = cJSON_Print(o);
    cJSON_Delete(o);
    if (!s) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}

/* Bridge mode without an admin password: someone who knocks the bridge off Wi-Fi (deauthentication) can
 * bring up the open fallback access point. Clients there may fix the Wi-Fi settings, which is what it's
 * for, but not install firmware, run commands or restore a backup. */
static bool fallback_ap_locked(httpd_req_t *req)
{
    if (g_state.mode != MODE_BRIDGE || g_cfg.admin_pass[0] || !wifi_ap_active()) {
        return false;
    }
    char me[16];
    local_ip(req, me, sizeof me);
    ip4_addr_t ap = { .addr = wifi_ap_ip() };
    if (strcmp(me, ip4addr_ntoa(&ap)) != 0) {
        return false;
    }
    send_err(req, "403 Forbidden", "not available on the setup network without an admin password: "
             "fix the Wi-Fi settings here, or set an admin password");
    return true;
}

static esp_err_t h_ota_upload_ap(httpd_req_t *req)
{
    return fallback_ap_locked(req) ? ESP_OK : h_ota_upload(req);
}

static esp_err_t h_ota_url_ap(httpd_req_t *req)
{
    return fallback_ap_locked(req) ? ESP_OK : h_ota_url(req);
}

static esp_err_t h_restore(httpd_req_t *req)
{
    AUTH();
    if (fallback_ap_locked(req)) {
        return ESP_OK;
    }
    cJSON *j = read_json(req, 8192);
    if (!j) {
        return ESP_OK;
    }
    const cJSON *firmware = cJSON_GetObjectItem(j, "_firmware");
    if (!cJSON_IsString(firmware) || strcmp(firmware->valuestring, "wifi-bridge") ||
        !cJSON_IsObject(cJSON_GetObjectItem(j, "device")) ||
        !cJSON_IsObject(cJSON_GetObjectItem(j, "wifi")) ||
        !cJSON_IsObject(cJSON_GetObjectItem(j, "network"))) {
        cJSON_Delete(j);
        return send_err(req, "400 Bad Request", "choose a WiFi Bridge settings backup");
    }
    char err[128];
    esp_err_t result = config_update_json(j, err, sizeof err, NULL);
    cJSON_Delete(j);
    if (result != ESP_OK) {
        return send_err(req, result == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "500 Internal Server Error", err);
    }
    wb_restart("settings restored", 1500);
    return send_ok(req);
}

static esp_err_t h_coredump(httpd_req_t *req)
{
    AUTH();
    size_t addr, size;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK) {
        return send_err(req, "404 Not Found", "no crash report stored");
    }
    char disp[96];
    snprintf(disp, sizeof disp, "attachment; filename=\"coredump-%s.elf\"", g_cfg.hostname);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    char *buf = malloc(2048);
    if (!buf) {
        return httpd_resp_send_500(req);
    }
    /* the stored image is the raw dump; espcoredump.py / idf.py coredump-info read it with the firmware .elf */
    for (size_t off = 0; off < size; off += 2048) {
        size_t n = size - off < 2048 ? size - off : 2048;
        if (esp_flash_read(NULL, buf, addr + off, n) != ESP_OK || httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            free(buf);
            return ESP_FAIL;
        }
    }
    free(buf);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_coredump_erase(httpd_req_t *req)
{
    AUTH();
    sys_crash_erase();
    return send_ok(req);
}

typedef struct {
    httpd_req_t *req;
    bool first;
    int count;
} log_ctx_t;

static bool log_line(const uint32_t *seq, uint32_t t, char lvl, const char *msg, void *arg)
{
    log_ctx_t *c = arg;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "seq", *seq);
    cJSON_AddNumberToObject(o, "t", t);
    char l[2] = { lvl, 0 };
    cJSON_AddStringToObject(o, "lvl", l);
    cJSON_AddStringToObject(o, "msg", msg);
    char buf[400];
    buf[0] = c->first ? ' ' : ',';
    if (cJSON_PrintPreallocated(o, buf + 1, sizeof buf - 1, false)) {
        httpd_resp_send_chunk(c->req, buf, strlen(buf));
        c->first = false;
    }
    cJSON_Delete(o);
    return ++c->count < 200;
}

static esp_err_t h_log(httpd_req_t *req)
{
    AUTH();
    char q[32], v[12];
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && httpd_query_key_value(q, "since", v, sizeof v) == ESP_OK) {
        since = strtoul(v, NULL, 10);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send_chunk(req, "{\"lines\":[", 10);
    log_ctx_t c = { .req = req, .first = true };
    uint32_t next = logbuf_read(since, log_line, &c);
    char tail[32];
    snprintf(tail, sizeof tail, "],\"next\":%lu}", (unsigned long)next);
    httpd_resp_send_chunk(req, tail, strlen(tail));
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t run_cmd(httpd_req_t *req, const char *cmd, bool wrap)
{
    char *out = malloc(3072);
    if (!out) {
        return httpd_resp_send_500(req);
    }
    console_exec(cmd, out, 3072, "web");
    cJSON *res = cJSON_Parse(out);
    if (!wrap) {
        httpd_resp_set_type(req, "application/json");
        esp_err_t r = httpd_resp_sendstr(req, out[0] ? out : "{}");
        cJSON_Delete(res);
        free(out);
        return r;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddItemToObject(o, "result", res ? res : cJSON_CreateString(out));
    free(out);
    return send_json(req, o);
}

static esp_err_t h_cmd(httpd_req_t *req)
{
    AUTH();
    if (fallback_ap_locked(req)) {
        return ESP_OK;
    }
    cJSON *j = read_json(req, 512);
    if (!j) {
        return ESP_OK;
    }
    const cJSON *c = cJSON_GetObjectItem(j, "cmd");
    if (!cJSON_IsString(c)) {
        cJSON_Delete(j);
        return send_err(req, "400 Bad Request", "cmd: required");
    }
    char line[256];
    strlcpy(line, c->valuestring, sizeof line);
    cJSON_Delete(j);
    return run_cmd(req, line, true);
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

/* A command that only reads: no argument, and not one that acts without one */
static bool cmd_read_only(const char *c)
{
    while (*c == ' ') {
        c++;
    }
    size_t n = strcspn(c, " ");
    if (c[n + strspn(c + n, " ")]) {
        return false;                            /* has an argument: it sets something */
    }
    static const char *acts[] = { "Backlog", "Setup", "Scan", "Upgrade", "Upload", "Restart", "Reset" };
    for (size_t i = 0; i < sizeof acts / sizeof acts[0]; i++) {
        if (strlen(acts[i]) == n && !strncasecmp(c, acts[i], n)) {
            return false;
        }
    }
    return true;
}

/* GET /cm?cmnd=Status%200[&user=admin&password=...]
 * Any web page can make a browser send a GET (<img src=...>), with cached Basic credentials and no Referer.
 * So commands that change something need the password in the URL, which a cross-site page can't know;
 * without an admin password /cm only reads. Browsers use POST /api/cmd, which has the Origin check. */
static esp_err_t h_cm(httpd_req_t *req)
{
    char q[320], cmd[256] = "", sfs[24] = "", ref[96] = "";
    /* GET can be triggered by any web page (<img src=...>): refuse browser cross-site requests */
    if (httpd_req_get_hdr_value_str(req, "Sec-Fetch-Site", sfs, sizeof sfs) == ESP_OK &&
        (!strcmp(sfs, "cross-site") || !strcmp(sfs, "same-site"))) {
        return send_err(req, "403 Forbidden", "cross-site request refused");
    }
    if (httpd_req_get_hdr_value_str(req, "Referer", ref, sizeof ref) != ESP_ERR_NOT_FOUND) {
        char host[72] = "";
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof host);
        const char *r = strstr(ref, "://");
        r = r ? r + 3 : ref;
        size_t hl = strlen(host);
        if (!hl || strncmp(r, host, hl) != 0 || (r[hl] != '/' && r[hl] != 0)) {
            return send_err(req, "403 Forbidden", "cross-site request refused");
        }
    }
    if (!host_is_us(req)) {
        return send_err(req, "403 Forbidden", "unknown Host header");
    }
    bool pw_ok = false;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        char pw[80];
        if (g_cfg.admin_pass[0] && httpd_query_key_value(q, "password", pw, sizeof pw) == ESP_OK) {
            url_decode(pw);
            if (strcmp(pw, g_cfg.admin_pass) != 0) {
                return send_err(req, "401 Unauthorized", "wrong password");
            }
            pw_ok = true;
        } else if (!authorized(req)) {
            return ESP_OK;
        }
        esp_err_t ce = httpd_query_key_value(q, "cmnd", cmd, sizeof cmd);
        if (ce == ESP_OK) {
            url_decode(cmd);
        } else if (ce != ESP_ERR_NOT_FOUND) {
            return send_err(req, "400 Bad Request", "command too long");
        }
    } else if (!authorized(req)) {
        return ESP_OK;
    }
    if (cmd[0] && !cmd_read_only(cmd) && fallback_ap_locked(req)) {
        return ESP_OK;
    }
    if (!pw_ok && cmd[0] && !cmd_read_only(cmd)) {
        return send_err(req, "403 Forbidden", g_cfg.admin_pass[0]
                        ? "commands that change settings need &user=admin&password=... in the URL"
                        : "set an admin password to change settings through /cm, or use POST /api/cmd");
    }
    return run_cmd(req, cmd[0] ? cmd : "Status", false);
}

void web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 32;
    cfg.max_open_sockets = 10;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start web server");
        return;
    }
    const httpd_uri_t uris[] = {
        { "/",                   HTTP_GET,  h_index,       NULL },
        { "/index.html",         HTTP_GET,  h_index,       NULL },
        { "/api/status",         HTTP_GET,  h_status,      NULL },
        { "/api/config",         HTTP_GET,  h_config_get,  NULL },
        { "/api/config",         HTTP_POST, h_config_post, NULL },
        { "/api/config/backup",  HTTP_GET,  h_backup,      NULL },
        { "/api/config/restore", HTTP_POST, h_restore,     NULL },
        { "/api/boards",         HTTP_GET,  h_boards,      NULL },
        { "/api/setup",          HTTP_POST, h_setup,       NULL },
        { "/api/scan",           HTTP_GET,  h_scan,        NULL },
        { "/api/wifi/test",      HTTP_GET,  h_test_get,    NULL },
        { "/api/wifi/test",      HTTP_POST, h_test_post,   NULL },
        { "/api/reboot",         HTTP_POST, h_reboot,      NULL },
        { "/api/reset",          HTTP_POST, h_reset,       NULL },
        { "/api/ota/upload",     HTTP_POST, h_ota_upload_ap, NULL },
        { "/api/ota/url",        HTTP_POST, h_ota_url_ap,  NULL },
        { "/api/ota",            HTTP_GET,  h_ota_get,     NULL },
        { "/api/log",            HTTP_GET,  h_log,         NULL },
        { "/api/cmd",            HTTP_POST, h_cmd,         NULL },
        { "/cm",                 HTTP_GET,  h_cm,          NULL },
        { "/api/coredump",       HTTP_GET,  h_coredump,    NULL },
        { "/api/coredump/erase", HTTP_POST, h_coredump_erase, NULL },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, h_404);
    ESP_LOGI(TAG, "web server started");
}
