/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wired side: EMAC/PHY bring-up from the runtime board configuration, the bridge input path,
 * MAC/DHCP rewriting, and the setup-mode DHCP server.
 * Derived from the ESP-IDF sta2eth example (originally Unlicense OR CC0-1.0).
 */
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_mac_openeth.h"
#include "soc/soc.h"
#include "esp_eth_netif_glue.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dhcpserver/dhcpserver_options.h"
#include "lwip/ip4_addr.h"
#include "config.h"
#include "wb.h"
#include "eth.h"

static const char *TAG = "eth";

static esp_eth_handle_t s_eth;
static esp_netif_t *s_setup_netif;
static volatile bool s_link;
static bool s_hw_ok, s_hw_failed, s_bridge;
static uint8_t s_eth_mac[6];

/* wired client (learned from its first DHCP DISCOVER, or fixed in the config) */
static uint8_t s_client_mac[6];
static volatile bool s_client_known;
static bool s_client_fixed;

static void eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED: {
        eth_speed_t sp = ETH_SPEED_100M;
        eth_duplex_t dp = ETH_DUPLEX_FULL;
        esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &sp);
        esp_eth_ioctl(s_eth, ETH_CMD_G_DUPLEX_MODE, &dp);
        ESP_LOGI(TAG, "link up, %s Mbps %s duplex", sp == ETH_SPEED_100M ? "100" : "10", dp == ETH_DUPLEX_FULL ? "full" : "half");
        s_link = true;
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "link down");
        s_link = false;
        if (!s_client_fixed && s_client_known) {
            /* a different device may be plugged in next: learn again from its DHCP */
            s_client_known = false;
            g_state.frames.client_ip = 0;
            ESP_LOGI(TAG, "forgetting wired client MAC");
        }
        break;
    default:
        break;
    }
}

static void rmii_clock(int32_t mode, eth_esp32_emac_config_t *c)
{
    switch (mode) {
    case CLK_OUT_GPIO0:
        c->clock_config.rmii.clock_mode = EMAC_CLK_OUT;
        c->clock_config.rmii.clock_gpio = EMAC_APPL_CLK_OUT_GPIO;
        break;
    case CLK_OUT_GPIO16:
        c->clock_config.rmii.clock_mode = EMAC_CLK_OUT;
        c->clock_config.rmii.clock_gpio = EMAC_CLK_OUT_GPIO;
        break;
    case CLK_OUT_GPIO17:
        c->clock_config.rmii.clock_mode = EMAC_CLK_OUT;
        c->clock_config.rmii.clock_gpio = EMAC_CLK_OUT_180_GPIO;
        break;
    default:
        c->clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
        c->clock_config.rmii.clock_gpio = EMAC_CLK_IN_GPIO;
        break;
    }
}

/* quiesce the MAC before a software restart (stray DMA/IRQ during the next boot's early init) */
static void eth_shutdown(void)
{
    if (s_eth) {
        esp_eth_stop(s_eth);
    }
#if CONFIG_WB_QEMU
    /* QEMU's soft reset leaves the emulated OpenCores MAC interrupt latched: mask and clear it */
    volatile uint32_t *oe = (volatile uint32_t *)DR_REG_EMAC_BASE;
    oe[0] = 0;          /* MODER: RX/TX off */
    oe[2] = 0;          /* INT_MASK */
    oe[1] = 0x7F;       /* INT_SOURCE: write 1 to clear */
#endif
}

esp_err_t eth_hw_init(void)
{
    if (s_hw_ok) {
        return ESP_OK;
    }
    static const char *phys[] = { "LAN8720", "IP101", "RTL8201", "DP83848", "KSZ80xx" };
    ESP_LOGI(TAG, "init %s: PHY %s addr %ld, MDC %ld, MDIO %ld, clock mode %ld, power pin %ld", g_cfg.board,
             phys[g_cfg.phy % 5], (long)g_cfg.phy_addr, (long)g_cfg.mdc, (long)g_cfg.mdio, (long)g_cfg.clk_mode,
             (long)g_cfg.power_pin);
    if (g_cfg.power_pin >= 0) {
        /* WT32-ETH01: enables the 50 MHz oscillator that clocks the EMAC on GPIO0; Olimex: PHY power */
        gpio_reset_pin(g_cfg.power_pin);
        gpio_set_direction(g_cfg.power_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(g_cfg.power_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    mac_cfg.rx_task_stack_size = 4096;
    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.smi_gpio.mdc_num = g_cfg.mdc;
    emac.smi_gpio.mdio_num = g_cfg.mdio;
    rmii_clock(g_cfg.clk_mode, &emac);
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = g_cfg.phy_addr;
    phy_cfg.reset_gpio_num = -1;

#if CONFIG_WB_QEMU
    phy_cfg.autonego_timeout_ms = 100;
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_cfg);
    (void)emac;
#else
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac, &mac_cfg);
    esp_eth_phy_t *phy = NULL;
    switch (g_cfg.phy) {
    case PHY_IP101:   phy = esp_eth_phy_new_ip101(&phy_cfg); break;
    case PHY_RTL8201: phy = esp_eth_phy_new_rtl8201(&phy_cfg); break;
    case PHY_DP83848: phy = esp_eth_phy_new_dp83848(&phy_cfg); break;
    case PHY_KSZ80XX: phy = esp_eth_phy_new_ksz80xx(&phy_cfg); break;
    default:          phy = esp_eth_phy_new_lan87xx(&phy_cfg); break;
    }
#endif
    if (!mac || !phy) {
        ESP_LOGE(TAG, "MAC/PHY allocation failed");
        if (mac) mac->del(mac);
        if (phy) phy->del(phy);
        s_hw_failed = true;
        return ESP_FAIL;
    }
    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t e = esp_eth_driver_install(&cfg, &s_eth);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "driver install failed: %s (check the board / PHY settings)", esp_err_to_name(e));
        mac->del(mac);
        phy->del(phy);
        s_eth = NULL;
        s_hw_failed = true;
        return e;
    }
    esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, s_eth_mac);
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event, NULL);
    esp_register_shutdown_handler(eth_shutdown);
    s_hw_ok = true;
    s_hw_failed = false;
    return ESP_OK;
}

/* ---------------- bridge role ---------------- */

static esp_err_t wired_recv(esp_eth_handle_t h, uint8_t *buffer, uint32_t len, void *priv)
{
    bridge_from_wired(buffer, len);
    free(buffer);
    return ESP_OK;
}

esp_err_t eth_start_bridge(void)
{
    if (g_cfg.client_mac[0]) {
        unsigned m[6];
        if (sscanf(g_cfg.client_mac, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
            for (int i = 0; i < 6; i++) {
                s_client_mac[i] = m[i];
            }
            s_client_known = s_client_fixed = true;
            ESP_LOGI(TAG, "wired client MAC fixed to %s", g_cfg.client_mac);
        }
    }
    esp_err_t e = esp_eth_update_input_path(s_eth, wired_recv, NULL);
    bool on = true;
    if (e == ESP_OK) {
        /* promiscuous: accept frames for the wired device from any LAN host, not just the AP */
        e = esp_eth_ioctl(s_eth, ETH_CMD_S_PROMISCUOUS, &on);
    }
    if (e == ESP_OK) {
        e = esp_eth_start(s_eth);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "bridge start failed: %s", esp_err_to_name(e));
        return e;
    }
    s_bridge = true;
    return ESP_OK;
}

esp_err_t eth_transmit(void *buffer, uint16_t len)
{
    if (!s_link) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_eth_transmit(s_eth, buffer, len);
}

bool eth_link_up(void) { return s_link; }

bool eth_client_mac(uint8_t out[6])
{
    if (s_client_known) {
        memcpy(out, s_client_mac, 6);
    }
    return s_client_known;
}

void eth_mac(uint8_t mac[6]) { memcpy(mac, s_eth_mac, 6); }

/*
 * The wired device appears on Wi-Fi under the ESP32's own STA MAC (an 802.11 station can only send
 * frames from the MAC it associated with). Frames from the wired device get their source MAC (and the
 * sender MAC inside ARP, and chaddr / client-id inside DHCP) rewritten to own_mac; frames to own_mac get
 * their destination rewritten to the wired device's real MAC.
 */
#define IP_V4 0x40
#define IP_PROTO_UDP 0x11
#define DHCP_PORT_IN 0x43
#define DHCP_PORT_OUT 0x44
#define DHCP_MAGIC_COOKIE_OFFSET (8 + 236)
#define DHCP_HW_ADDRESS_OFFSET (36)
#define MIN_DHCP_PACKET_SIZE (285)
#define IP_HEADER_SIZE (20)
#define DHCP_DISCOVER 1

static void update_udp_checksum(uint16_t *udp_header, uint16_t *ip_header)
{
    uint32_t sum = 0;
    uint16_t *ptr = udp_header;
    ptr[3] = 0;
    int payload_len = htons(ip_header[1]) - IP_HEADER_SIZE;
    for (int i = 0; i < payload_len / 2; i++) {
        sum += htons(*ptr++);
    }
    if (payload_len & 1) {
        sum += (*((uint8_t *)ptr) << 8);
    }
    ptr = ip_header + 6;
    for (int i = 0; i < 4; i++) {
        sum += htons(*ptr++);
    }
    sum += IP_PROTO_UDP + payload_len;
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    udp_header[3] = htons(~sum);
}

/* Frames come from the network: only touch DHCP when the IPv4 and UDP lengths agree with the frame length
 * (update_udp_checksum trusts the IP total length). */
static bool udp_lengths_ok(const uint8_t *buffer, uint16_t len)
{
    const uint8_t *ip = buffer + 14;
    uint16_t tot = (ip[2] << 8) | ip[3];
    uint16_t ulen = (ip[24] << 8) | ip[25];
    return tot >= IP_HEADER_SIZE + 8 && 14 + (uint32_t)tot <= len && ulen == tot - IP_HEADER_SIZE &&
           tot > DHCP_MAGIC_COOKIE_OFFSET + IP_HEADER_SIZE + 4;
}

static void learn_client(const uint8_t *mac, const char *how)
{
    memcpy(s_client_mac, mac, 6);
    s_client_known = true;
    ESP_LOGI(TAG, "wired client %02x:%02x:%02x:%02x:%02x:%02x (from %s)", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5], how);
}

/* DHCP from the wired client: learn its MAC from DISCOVER, rewrite chaddr and option 61 to own_mac */
static void dhcp_from_wired(uint8_t *buffer, uint16_t len, const uint8_t own_mac[6])
{
    uint8_t *ip_header = buffer + 14;
    if (len <= MIN_DHCP_PACKET_SIZE || (ip_header[0] & 0xF0) != IP_V4 || ip_header[9] != IP_PROTO_UDP ||
        (ip_header[0] & 0x0F) != 5 || !udp_lengths_ok(buffer, len)) {
        return;
    }
    uint8_t *udp = ip_header + IP_HEADER_SIZE;
    const uint8_t ports[] = { 0, DHCP_PORT_OUT, 0, DHCP_PORT_IN };
    if (memcmp(udp, ports, sizeof ports) != 0) {
        return;
    }
    uint8_t *magic = udp + DHCP_MAGIC_COOKIE_OFFSET;
    const uint8_t discover[] = { 0x63, 0x82, 0x53, 0x63, 0x35, 1, DHCP_DISCOVER };
    if (!s_client_known && memcmp(magic, discover, sizeof discover) == 0) {
        learn_client(buffer + 6, "DHCP DISCOVER");
    }
    if (!s_client_known) {
        return;
    }
    bool changed = false;
    uint8_t *chaddr = udp + DHCP_HW_ADDRESS_OFFSET;
    if (memcmp(chaddr, s_client_mac, 6) == 0) {
        memcpy(chaddr, own_mac, 6);
        changed = true;
    }
    uint8_t *opt = magic + 4;
    while (opt + 2 < buffer + len && *opt != 0xFF) {
        if (*opt == 0) {               /* pad */
            opt++;
            continue;
        }
        if (opt[0] == 61 && opt[1] == 7 && opt[2] == 1 && opt + 9 <= buffer + len && memcmp(opt + 3, s_client_mac, 6) == 0) {
            memcpy(opt + 3, own_mac, 6);
            changed = true;
            break;
        }
        opt += opt[1] + 2;
    }
    if (changed) {
        update_udp_checksum((uint16_t *)udp, (uint16_t *)ip_header);
    }
}

/* DHCP to the wired client: restore its real MAC in chaddr */
static void dhcp_to_wired(uint8_t *buffer, uint16_t len, const uint8_t own_mac[6])
{
    uint8_t *ip_header = buffer + 14;
    if (len <= MIN_DHCP_PACKET_SIZE || (ip_header[0] & 0xF0) != IP_V4 || ip_header[9] != IP_PROTO_UDP ||
        (ip_header[0] & 0x0F) != 5 || !udp_lengths_ok(buffer, len)) {
        return;
    }
    uint8_t *udp = ip_header + IP_HEADER_SIZE;
    const uint8_t ports[] = { 0, DHCP_PORT_IN, 0, DHCP_PORT_OUT };
    if (memcmp(udp, ports, sizeof ports) != 0) {
        return;
    }
    uint8_t *chaddr = udp + DHCP_HW_ADDRESS_OFFSET;
    if (memcmp(chaddr, own_mac, 6) == 0) {
        memcpy(chaddr, s_client_mac, 6);
        update_udp_checksum((uint16_t *)udp, (uint16_t *)ip_header);
    }
}

void mac_spoof(mac_spoof_direction_t dir, uint8_t *buffer, uint16_t len, const uint8_t own_mac[6])
{
    if (!s_link || len < 14) {
        return;
    }
    uint8_t *dst = buffer, *src = buffer + 6;
    bool ipv4 = buffer[12] == 0x08 && buffer[13] == 0x00, arp = buffer[12] == 0x08 && buffer[13] == 0x06;
    if (ipv4 && len >= 14 + IP_HEADER_SIZE + 8) {
        if (dir == FROM_WIRED) {
            dhcp_from_wired(buffer, len, own_mac);
        } else if (s_client_known) {
            dhcp_to_wired(buffer, len, own_mac);
        }
    }
    if (!s_client_known && dir == FROM_WIRED && (ipv4 || arp) && !(src[0] & 1)) {
        learn_client(src, ipv4 ? "IPv4 traffic" : "ARP");     /* devices with a static address */
    }
    if (!s_client_known) {
        return;
    }
    if (arp && len >= 42 && dir == FROM_WIRED && memcmp(buffer + 22, s_client_mac, 6) == 0) {
        memcpy(buffer + 22, own_mac, 6);     /* ARP sender hardware address */
    }
    if (dir == FROM_WIRED && memcmp(src, s_client_mac, 6) == 0) {
        memcpy(src, own_mac, 6);
    }
    if (dir == TO_WIRED && memcmp(dst, own_mac, 6) == 0) {
        memcpy(dst, s_client_mac, 6);
    }
}

/* ---------------- setup role ---------------- */

esp_err_t eth_start_setup(void)
{
    static esp_netif_ip_info_t ip;
    ip.ip.addr = ipaddr_addr(WB_ETH_SETUP_IP);
    ip.gw.addr = ip.ip.addr;
    ip.netmask.addr = ipaddr_addr("255.255.255.0");
    esp_netif_inherent_config_t base = {
        .flags = ESP_NETIF_DHCP_SERVER,
        .ip_info = &ip,
        .if_key = "ETH_SETUP",
        .if_desc = "eth-setup",
        .route_prio = 10,
    };
    esp_netif_config_t cfg = { .base = &base, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
    s_setup_netif = esp_netif_new(&cfg);
    if (!s_setup_netif) {
        return ESP_FAIL;
    }
    esp_netif_attach(s_setup_netif, esp_eth_new_netif_glue(s_eth));
    esp_netif_set_mac(s_setup_netif, s_eth_mac);

    /* hand out ourselves as DNS server (captive portal), short leases, and the RFC 8910 portal URI */
    esp_netif_dhcps_stop(s_setup_netif);
    esp_netif_dns_info_t dns = { .ip.type = ESP_IPADDR_TYPE_V4, .ip.u_addr.ip4.addr = ip.ip.addr };
    esp_netif_set_dns_info(s_setup_netif, ESP_NETIF_DNS_MAIN, &dns);
    uint8_t offer_dns = 0x02;
    esp_netif_dhcps_option(s_setup_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof offer_dns);
    uint32_t lease_min = 2;
    esp_netif_dhcps_option(s_setup_netif, ESP_NETIF_OP_SET, IP_ADDRESS_LEASE_TIME, &lease_min, sizeof lease_min);
    static char uri[] = "http://" WB_ETH_SETUP_IP "/";
    esp_netif_dhcps_option(s_setup_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, strlen(uri));

    esp_err_t e = esp_eth_start(s_eth);
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "setup page on the wired port: http://%s/ (DHCP server running)", WB_ETH_SETUP_IP);
    }
    return e;
}

void eth_status_json(cJSON *o)
{
    const char *st = s_hw_failed ? "init_failed" : !s_hw_ok ? "pending" : s_link ? "up" : "down";
    cJSON_AddStringToObject(o, "state", st);
    if (s_link) {
        eth_speed_t sp = ETH_SPEED_100M;
        eth_duplex_t dp = ETH_DUPLEX_FULL;
        esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &sp);
        esp_eth_ioctl(s_eth, ETH_CMD_G_DUPLEX_MODE, &dp);
        cJSON_AddNumberToObject(o, "speed", sp == ETH_SPEED_100M ? 100 : 10);
        cJSON_AddStringToObject(o, "duplex", dp == ETH_DUPLEX_FULL ? "full" : "half");
    } else {
        cJSON_AddNullToObject(o, "speed");
        cJSON_AddNullToObject(o, "duplex");
    }
    cJSON_AddStringToObject(o, "role", s_setup_netif ? "setup" : "bridge");
    if (s_setup_netif) {
        cJSON_AddStringToObject(o, "ip", WB_ETH_SETUP_IP);
    } else {
        cJSON_AddNullToObject(o, "ip");
    }
}
