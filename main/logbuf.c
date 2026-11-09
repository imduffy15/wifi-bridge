/*
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 *
 * Captures every ESP_LOG line into a RAM ring (web console, /api/log) while still printing it on UART0,
 * and forwards lines to a remote syslog server.
 */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "config.h"
#include "logbuf.h"

#define SLOTS     112
#define SLOT_LEN  144

typedef struct {
    uint32_t seq, t;
    char lvl;
    char msg[SLOT_LEN];
} slot_t;

static slot_t *s_ring;
static uint32_t s_seq;                 /* seq of the last written line */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig;
/* the Wi-Fi blob logs a line in pieces ("I (123) wifi:" then "message\n"): join them */
static char s_partial[SLOT_LEN];
static size_t s_partial_len;

static void store(char lvl, const char *msg)
{
    uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL_SAFE(&s_mux);
    uint32_t seq = ++s_seq;
    slot_t *s = &s_ring[seq % SLOTS];
    s->seq = seq;
    s->t = t;
    s->lvl = lvl;
    strlcpy(s->msg, msg, SLOT_LEN);
    portEXIT_CRITICAL_SAFE(&s_mux);
}

/* "I (1234) tag: message\n" -> level 'I', message "tag: message" */
static int hook(const char *fmt, va_list ap)
{
    /* format once (stack-light: this runs on whatever task logged), print the same text on UART */
    char buf[SLOT_LEN + 48];
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(buf, sizeof buf, fmt, cp);
    va_end(cp);
    int r;
    if (n < 0 || n >= (int)sizeof buf) {
        r = s_orig(fmt, ap);                 /* too long for the ring line: print it in full */
    } else {
        r = fputs(buf, stdout) < 0 ? -1 : n;
    }
    if (n > 0) {
        char *m = buf;
        char lvl = '-';
        if ((m[0] == 'E' || m[0] == 'W' || m[0] == 'I' || m[0] == 'D' || m[0] == 'V') && m[1] == ' ' && m[2] == '(') {
            lvl = m[0];
            char *close = strchr(m, ')');
            m = close ? close + 1 : m;
            while (*m == ' ') {
                m++;
            }
        }
        size_t len = strlen(m);
        bool complete = len && m[len - 1] == '\n';
        while (len && (m[len - 1] == '\n' || m[len - 1] == '\r')) {
            m[--len] = 0;
        }
        portENTER_CRITICAL_SAFE(&s_mux);
        if (!complete) {
            /* keep the fragment (with its level) until the rest of the line arrives */
            if (!s_partial_len && lvl != '-') {
                s_partial[0] = lvl;
                s_partial_len = 1;
            }
            size_t room = sizeof s_partial - 1 - s_partial_len;
            size_t n2 = len < room ? len : room;
            memcpy(s_partial + s_partial_len, m, n2);
            s_partial_len += n2;
            s_partial[s_partial_len] = 0;
            portEXIT_CRITICAL_SAFE(&s_mux);
            return r;
        }
        char joined[SLOT_LEN];
        char jl = lvl;
        joined[0] = 0;
        if (s_partial_len) {
            jl = s_partial[0];
            size_t pl = strlcpy(joined, s_partial + 1, sizeof joined);
            if (pl + 1 < sizeof joined) {
                joined[pl] = ' ';
                strlcpy(joined + pl + 1, m, sizeof joined - pl - 1);
            }
            s_partial_len = 0;
        }
        portEXIT_CRITICAL_SAFE(&s_mux);
        if (joined[0]) {
            store(jl, joined);
        } else if (len) {
            store(lvl, m);
        }
    }
    return r;
}

void logbuf_add(char lvl, const char *msg)
{
    store(lvl, msg);
    printf("%s\n", msg);
}

uint32_t logbuf_read(uint32_t since, logbuf_cb_t cb, void *ctx)
{
    slot_t tmp;
    uint32_t last = s_seq;
    if (last > SLOTS && since < last - SLOTS + 1) {
        since = last - SLOTS;          /* oldest still in the ring is last-SLOTS+1 */
    }
    for (uint32_t seq = since + 1; seq <= last; seq++) {
        portENTER_CRITICAL(&s_mux);
        tmp = s_ring[seq % SLOTS];
        portEXIT_CRITICAL(&s_mux);
        if (tmp.seq == seq && !cb(&tmp.seq, tmp.t, tmp.lvl, tmp.msg, ctx)) {
            return seq - 1;            /* not consumed: deliver it next time */
        }
    }
    return last;
}

/* ---- syslog ---- */

static void syslog_task(void *arg)
{
    uint32_t sent = s_seq;
    int sock = -1;
    char host[sizeof g_cfg.syslog_host] = "";
    struct sockaddr_in dst = { 0 };
    char pkt[SLOT_LEN + 96];
    uint32_t last_resolve = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!g_cfg.syslog_host[0]) {
            sent = s_seq;
            continue;
        }
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
        if (strcmp(host, g_cfg.syslog_host) != 0 || (!dst.sin_addr.s_addr && now - last_resolve > 30)) {
            strlcpy(host, g_cfg.syslog_host, sizeof host);
            last_resolve = now;
            dst.sin_addr.s_addr = 0;
            struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res = NULL;
            if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
                dst = *(struct sockaddr_in *)res->ai_addr;
                freeaddrinfo(res);
            }
        }
        if (!dst.sin_addr.s_addr) {
            continue;
        }
        dst.sin_family = AF_INET;
        dst.sin_port = htons(g_cfg.syslog_port);
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            if (sock < 0) {
                continue;
            }
        }
        slot_t s;
        uint32_t last = s_seq;
        if (last - sent > SLOTS) {
            sent = last - SLOTS;
        }
        for (uint32_t seq = sent + 1; seq <= last; seq++) {
            portENTER_CRITICAL(&s_mux);
            s = s_ring[seq % SLOTS];
            portEXIT_CRITICAL(&s_mux);
            if (s.seq != seq) {
                continue;
            }
            int sev = s.lvl == 'E' ? 3 : s.lvl == 'W' ? 4 : s.lvl == 'D' || s.lvl == 'V' ? 7 : 6;
            /* RFC 3164-ish: <PRI>HOSTNAME TAG: MSG, facility local0 (16) */
            int n = snprintf(pkt, sizeof pkt, "<%d>%s wifi-bridge: %s", 16 * 8 + sev, g_cfg.hostname, s.msg);
            sendto(sock, pkt, n, 0, (struct sockaddr *)&dst, sizeof dst);
        }
        sent = last;
    }
}

void logbuf_init(void)
{
    s_ring = calloc(SLOTS, sizeof(slot_t));
    if (!s_ring) {
        return;
    }
    s_orig = esp_log_set_vprintf(hook);
}

void logbuf_start_syslog(void)
{
    xTaskCreate(syslog_task, "syslog", 4096, NULL, 2, NULL);
}

void logbuf_set_level(int lvl)
{
    static const esp_log_level_t map[] = { ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG };
    esp_log_level_set("*", map[lvl < 0 || lvl > 3 ? 2 : lvl]);
    /* the Wi-Fi driver is very chatty at debug */
    if (lvl >= 3) {
        esp_log_level_set("wifi", ESP_LOG_INFO);
    }
}
