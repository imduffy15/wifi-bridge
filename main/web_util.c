/*
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 *
 * HTTP helpers shared by the firmware and the safeboot firmware: JSON replies, access checks
 * (Host, Origin, Basic auth) and the firmware update endpoints.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "lwip/sockets.h"
#include "config.h"
#include "wb.h"
#include "web_util.h"

esp_err_t send_json(httpd_req_t *req, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_sendstr(req, s);
    free(s);
    return r;
}

esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "error", msg);
    return send_json(req, o);
}

esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o);
}

/* local address of this connection: which of our interfaces the client talks to */
void local_ip(httpd_req_t *req, char *buf, size_t n)
{
    struct sockaddr_storage a;
    socklen_t l = sizeof a;
    buf[0] = 0;
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&a, &l) == 0) {
        if (a.ss_family == AF_INET) {
            inet_ntoa_r(((struct sockaddr_in *)&a)->sin_addr, buf, n);
        }
#if LWIP_IPV6
        else {
            /* IPv4-mapped IPv6 */
            struct in_addr v4 = { .s_addr = ((struct sockaddr_in6 *)&a)->sin6_addr.un.u32_addr[3] };
            inet_ntoa_r(v4, buf, n);
        }
#endif
    }
}


static bool local_suffix(const char *s)
{
    static const char *ok[] = { "local", "lan", "home", "home.arpa", "localdomain", "internal", "intranet",
                                "fritz.box", "box", "router", "domain", "localnet", "private", "corp" };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        if (!strcasecmp(s, ok[i])) {
            return true;
        }
    }
    return false;
}

/* Is the Host header one of our own names/addresses? (DNS rebinding protection) */
bool host_is_us(httpd_req_t *req)
{
    char host[72] = "";
    esp_err_t e = httpd_req_get_hdr_value_str(req, "Host", host, sizeof host);
    if (e == ESP_ERR_NOT_FOUND) {
        return true;                       /* HTTP/1.0 clients */
    }
    if (e != ESP_OK) {
        return false;
    }
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
    }
    char me[16], mdns[48];
    local_ip(req, me, sizeof me);
    snprintf(mdns, sizeof mdns, "%s.local", g_cfg.hostname);
#if CONFIG_WB_QEMU
    if (!strcmp(host, "127.0.0.1") || !strcmp(host, "localhost")) {
        return true;
    }
#endif
    size_t hn = strlen(g_cfg.hostname);
    return !strcmp(host, me) || !strcasecmp(host, mdns) || !strcasecmp(host, g_cfg.hostname) ||
           /* names from the router's DNS; a fixed list, so <hostname>.attacker.example can't rebind */
           (!strncasecmp(host, g_cfg.hostname, hn) && host[hn] == '.' && local_suffix(host + hn + 1));
}

/* Basic auth (user admin) when a password is set; Host must be us; Origin must match Host for POSTs (CSRF) */
bool authorized(httpd_req_t *req)
{
    if (!host_is_us(req)) {
        send_err(req, "403 Forbidden", "unknown Host header: use the device's address or hostname");
        return false;
    }
    if (req->method == HTTP_POST) {
        char origin[128] = "", host[72] = "";
        esp_err_t e = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof origin);
        if (e != ESP_ERR_NOT_FOUND) {
            httpd_req_get_hdr_value_str(req, "Host", host, sizeof host);
            const char *o = strstr(origin, "://");
            o = o ? o + 3 : origin;
            if (e != ESP_OK || strcmp(o, host) != 0) {
                send_err(req, "403 Forbidden", "cross-origin request refused");
                return false;
            }
        }
    }
    if (!g_cfg.admin_pass[0]) {
        return true;
    }
    char hdr[128] = "";
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof hdr) == ESP_OK && strncmp(hdr, "Basic ", 6) == 0) {
        unsigned char dec[100];
        size_t olen = 0;
        if (mbedtls_base64_decode(dec, sizeof dec - 1, &olen, (const unsigned char *)hdr + 6, strlen(hdr + 6)) == 0) {
            dec[olen] = 0;
            char expect[80];
            snprintf(expect, sizeof expect, "admin:%s", g_cfg.admin_pass);
            size_t el = strlen(expect);
            unsigned diff = olen ^ el;
            for (size_t i = 0; i < el && i < olen; i++) {
                diff |= dec[i] ^ (unsigned char)expect[i];
            }
            if (diff == 0) {
                return true;
            }
        }
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"WiFi Bridge\", charset=\"UTF-8\"");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"authentication required\"}");
    return false;
}


cJSON *read_json(httpd_req_t *req, size_t max)
{
    if (req->content_len == 0 || req->content_len > max) {
        send_err(req, "400 Bad Request", req->content_len ? "request body too large" : "request body required");
        return NULL;
    }
    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        httpd_resp_send_500(req);
        return NULL;
    }
    size_t got = 0;
    int timeouts = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (n <= 0) {
            free(buf);
            return NULL;
        }
        got += n;
    }
    buf[got] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) {
        send_err(req, "400 Bad Request", "invalid JSON");
    }
    return j;
}


esp_err_t h_ota_upload(httpd_req_t *req)
{
    AUTH();
    char err[128];
    if (ota_begin_upload(req->content_len, err, sizeof err) != ESP_OK) {
        return send_err(req, "409 Conflict", err);
    }
    char *buf = malloc(4096);
    if (!buf) {
        ota_abort();
        return httpd_resp_send_500(req);
    }
    size_t left = req->content_len;
    int timeouts = 0;
    while (left > 0) {
        int n = httpd_req_recv(req, buf, left < 4096 ? left : 4096);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) {
            continue;
        }
        if (n <= 0) {
            free(buf);
            ota_abort();
            return ESP_FAIL;                /* closes the connection */
        }
        timeouts = 0;
        left -= n;
        esp_err_t we = ota_write(buf, n, err, sizeof err);
        if (we != ESP_OK) {
            /* read (and discard) the rest, so the client gets the error instead of a reset connection */
            while (left > 0) {
                int d = httpd_req_recv(req, buf, left < 4096 ? left : 4096);
                if (d == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) {
                    continue;
                }
                if (d <= 0) {
                    break;
                }
                left -= d;
            }
            free(buf);
            httpd_resp_set_hdr(req, "Connection", "close");
            /* 409: a valid image that this firmware can't install itself (needs the safeboot firmware) */
            return send_err(req, we == ESP_ERR_NOT_SUPPORTED ? "409 Conflict" : "400 Bad Request", err);
        }
    }
    free(buf);
    if (ota_finish(err, sizeof err) != ESP_OK) {
        return send_err(req, "400 Bad Request", err);
    }
    bool restart = ota_installed_kind() == OTA_KIND_FIRMWARE;
    if (restart) {
        wb_restart("firmware update", 2000);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "installed", restart ? "firmware" : "safeboot");
    cJSON_AddBoolToObject(o, "restart", restart);
    return send_json(req, o);
}

esp_err_t h_ota_url(httpd_req_t *req)
{
    AUTH();
    cJSON *j = read_json(req, 512);
    if (!j) {
        return ESP_OK;
    }
    const cJSON *u = cJSON_GetObjectItem(j, "url");
    char err[128] = "url: required";
    esp_err_t e = cJSON_IsString(u) ? ota_start_url(u->valuestring, err, sizeof err) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(j);
    if (e != ESP_OK) {
        return send_err(req, e == ESP_ERR_NOT_SUPPORTED ? "409 Conflict" : "400 Bad Request", err);
    }
    return send_ok(req);
}

esp_err_t h_ota_get(httpd_req_t *req)
{
    AUTH();
    cJSON *o = cJSON_CreateObject();
    ota_status_json(o);
    return send_json(req, o);
}
