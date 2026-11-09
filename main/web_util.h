/*
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 *
 * HTTP helpers shared by the firmware and the safeboot firmware.
 */
#pragma once
#include <stdbool.h>
#include "esp_http_server.h"
#include "cJSON.h"

esp_err_t send_json(httpd_req_t *req, cJSON *o);      /* takes ownership of o */
esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg);
esp_err_t send_ok(httpd_req_t *req);
void local_ip(httpd_req_t *req, char *buf, size_t n);  /* our address on this connection */
bool host_is_us(httpd_req_t *req);                     /* DNS rebinding protection */
/* Host, Origin (POST) and Basic auth checks; on failure the reply has been sent */
bool authorized(httpd_req_t *req);
#define AUTH() do { if (!authorized(req)) return ESP_OK; } while (0)
cJSON *read_json(httpd_req_t *req, size_t max);        /* NULL: error reply already sent */

/* /api/ota/upload (POST, raw image), /api/ota/url (POST {"url"}), /api/ota (GET progress) */
esp_err_t h_ota_upload(httpd_req_t *req);
esp_err_t h_ota_url(httpd_req_t *req);
esp_err_t h_ota_get(httpd_req_t *req);
