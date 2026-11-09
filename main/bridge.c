/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bridge data path (derived from the ESP-IDF sta2eth example, originally Unlicense OR CC0-1.0).
 *
 *   Wi-Fi -> wired : everything except IPv6, and except frames for the management address (ARP target /
 *                    IPv4 destination), which go to our own lwIP stack. mDNS multicast goes to both.
 *   wired -> Wi-Fi : the wired device's frames, with its MAC rewritten to ours (see eth.c mac_spoof).
 *
 * Airtime: access points send broadcast and multicast at the lowest basic rate, so the wired side is
 * filtered before it reaches the radio. Dropped: frames from any other MAC (the access point would discard
 * them), IEEE 802.1 link-local frames (STP, LLDP, LACP, 802.1X: bridges never forward them), IPv6 (its
 * neighbor discovery carries the real MAC, so it can't work through the bridge), and broadcast/multicast
 * beyond a rate limit (ARP and DHCP are exempt).
 */
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_private/wifi.h"
#include "config.h"
#include "wb.h"
#include "eth.h"

static const char *TAG = "bridge";

static uint8_t s_sta_mac[6];
static uint32_t s_my_ip;
static esp_netif_t *s_sta;
static volatile bool s_up;

#define BCAST_RATE   30          /* broadcast/multicast frames per second, wired -> Wi-Fi */
#define BCAST_BURST  60

static inline bool is_ipv6(const uint8_t *b)
{
    return b[12] == 0x86 && b[13] == 0xdd;
}

/* ARP, and DHCP client -> server (UDP 68 -> 67), keep working whatever the rate */
static inline bool is_arp_or_dhcp(const uint8_t *b, uint16_t len)
{
    if (b[12] == 0x08 && b[13] == 0x06) {
        return true;
    }
    if (b[12] != 0x08 || b[13] != 0x00 || len < 14 + 20 + 8 || b[23] != 17) {
        return false;
    }
    size_t ihl = (b[14] & 0x0F) * 4;
    if (ihl < 20 || len < 14 + ihl + 4) {
        return false;
    }
    const uint8_t *udp = b + 14 + ihl;
    return udp[0] == 0 && udp[1] == 68 && udp[2] == 0 && udp[3] == 67;
}

/* token bucket for wired -> Wi-Fi broadcast/multicast (runs in the Ethernet receive task only) */
static bool bcast_allowed(void)
{
    static int64_t last_us;
    static int32_t tokens = BCAST_BURST;
    int64_t now = esp_timer_get_time();
    int32_t add = (int32_t)((now - last_us) * BCAST_RATE / 1000000);
    if (add > 0) {
        tokens = tokens + add > BCAST_BURST ? BCAST_BURST : tokens + add;
        last_us = now;
    }
    if (tokens <= 0) {
        return false;
    }
    tokens--;
    return true;
}

static inline bool frame_is_for_me(const uint8_t *b, uint16_t len)
{
    uint16_t et = (b[12] << 8) | b[13];
    uint32_t a;
    if (et == 0x0806 && len >= 42) {          /* ARP: target protocol address */
        memcpy(&a, b + 38, 4);
        return a == s_my_ip;
    }
    if (et == 0x0800 && len >= 34) {          /* IPv4 destination */
        memcpy(&a, b + 30, 4);
        return a == s_my_ip;
    }
    return false;
}

/* mDNS (224.0.0.251:5353) is needed by both us (hostname.local) and possibly the wired device */
static inline bool frame_is_mdns(const uint8_t *b, uint16_t len)
{
    return len >= 42 && b[0] == 0x01 && b[1] == 0x00 && b[2] == 0x5e && b[12] == 0x08 && b[13] == 0x00 &&
           b[23] == 17 && b[30] == 224 && b[31] == 0 && b[32] == 0 && b[33] == 251 &&
           (b[14] & 0x0F) == 5 && b[36] == 0x14 && b[37] == 0xe9;
}

static esp_err_t wifi_rx(void *buffer, uint16_t len, void *eb)
{
    uint8_t *b = buffer;
    if (len < 14) {
        esp_wifi_internal_free_rx_buffer(eb);
        return ESP_OK;
    }
    if (frame_is_for_me(b, len)) {
        g_state.frames.to_me++;
        return esp_netif_receive(s_sta, buffer, len, eb);    /* the stack frees eb */
    }
    if (is_ipv6(b)) {
        g_state.frames.wifi_drop++;                         /* IPv4 only: see the top of the file */
        esp_wifi_internal_free_rx_buffer(eb);
        return ESP_OK;
    }
    if (frame_is_mdns(b, len)) {
        eth_transmit(buffer, len);                          /* copied into DMA buffers synchronously */
        g_state.frames.to_me++;
        return esp_netif_receive(s_sta, buffer, len, eb);
    }
    mac_spoof(TO_WIRED, b, len, s_sta_mac);
    if (eth_transmit(buffer, len) == ESP_OK) {
        g_state.frames.wifi_to_wired++;
    } else {
        g_state.frames.wifi_drop++;
    }
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

void bridge_from_wired(uint8_t *b, uint16_t len)
{
    if (!s_up || len < 14) {
        g_state.frames.wired_drop++;
        return;
    }
    /* remember the wired device's IPv4 from its ARP traffic (sender protocol address) */
    uint8_t cm[6];
    if (len >= 42 && b[12] == 0x08 && b[13] == 0x06 && eth_client_mac(cm) && memcmp(b + 6, cm, 6) == 0) {
        uint32_t spa;
        memcpy(&spa, b + 28, 4);
        if (spa) {
            g_state.frames.client_ip = spa;
        }
    }
    /* IEEE 802.1 link-local (01:80:c2:00:00:0x) and IPv6 never go on the air */
    if ((b[0] == 0x01 && b[1] == 0x80 && b[2] == 0xc2 && b[3] == 0 && b[4] == 0 && (b[5] & 0xF0) == 0) ||
        is_ipv6(b)) {
        g_state.frames.wired_drop++;
        return;
    }
    mac_spoof(FROM_WIRED, b, len, s_sta_mac);
    /* only the bridged device's frames (now under our MAC): the access point drops any other source */
    if (memcmp(b + 6, s_sta_mac, 6) != 0) {
        g_state.frames.wired_drop++;
        return;
    }
    if ((b[0] & 1) && !is_arp_or_dhcp(b, len) && !bcast_allowed()) {
        g_state.frames.wired_drop++;
        return;
    }
    if (esp_wifi_internal_tx(WIFI_IF_STA, b, len) != ESP_OK) {
        g_state.frames.wifi_tx_fail++;
    } else {
        g_state.frames.wired_to_wifi++;
    }
}

void bridge_wifi_connected(bool up)
{
    if (!s_sta) {
        return;
    }
    if (up) {
        /* runs after the default handler registered esp_netif's rx callback, so ours replaces it */
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, wifi_rx);
        s_up = true;
    } else {
        s_up = false;
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, NULL);
    }
}

void bridge_start(void)
{
    s_sta = wifi_sta_netif();
    s_my_ip = g_cfg.ip;
    esp_read_mac(s_sta_mac, ESP_MAC_WIFI_STA);
    if (wifi_connected()) {
        bridge_wifi_connected(true);
    }
    ESP_LOGI(TAG, "bridging Wi-Fi <-> Ethernet, management address " IPSTR, IP2STR((esp_ip4_addr_t *)&s_my_ip));
}
