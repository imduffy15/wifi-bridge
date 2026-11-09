/*
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 *
 * Captive-portal DNS: every A query is answered with our own address on the querier's network, so phones
 * and laptops on the setup AP / setup Ethernet port open the setup page by themselves.
 */
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "config.h"
#include "wb.h"

static const char *TAG = "dns";

static uint32_t answer_for(uint32_t src)
{
    uint32_t mask = ipaddr_addr("255.255.255.0");
    uint32_t ap = wifi_ap_ip(), eth = ipaddr_addr(WB_ETH_SETUP_IP);
    if ((src & mask) == (ap & mask)) {
        return ap;
    }
    if ((src & mask) == (eth & mask)) {
        return eth;
    }
    return 0;                                    /* not one of our setup networks: ignore */
}

static void dns_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        ESP_LOGE(TAG, "bind failed");
        if (s >= 0) {
            close(s);
        }
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "captive DNS listening");
    uint8_t buf[512];
    while (true) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(s, buf, sizeof buf - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || (buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) {   /* queries with exactly one question */
            continue;
        }
        /* walk the question name */
        int p = 12;
        while (p < n && buf[p] && p + buf[p] + 1 < n) {
            p += buf[p] + 1;
        }
        if (p + 5 > n || buf[p] != 0) {
            continue;
        }
        p++;
        uint16_t qtype = (buf[p] << 8) | buf[p + 1];
        p += 4;                                  /* qtype + qclass */
        buf[2] = 0x80 | (buf[2] & 0x01);         /* response; copy "recursion desired" */
        buf[3] = 0x80;                           /* recursion available, NOERROR */
        buf[6] = buf[7] = buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int len = p;
        uint32_t ip = answer_for(from.sin_addr.s_addr);
        if (!ip) {
            continue;
        }
        if (qtype == 1 || qtype == 255) {        /* A / ANY */
            uint8_t ans[16] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4 };
            memcpy(ans + 12, &ip, 4);
            memcpy(buf + p, ans, 16);
            buf[7] = 1;
            len += 16;
        }
        sendto(s, buf, len, 0, (struct sockaddr *)&from, fl);
    }
}

void captive_dns_start(void)
{
    static bool started;
    if (!started) {
        started = true;
        xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
    }
}
