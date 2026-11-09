/*
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 *
 * WiFi Bridge: puts a wired-only device on Wi-Fi through an ESP32 Ethernet board.
 *
 * Boot:
 *   not configured (or "Restart 2")  -> setup mode: AP + captive portal, setup page also on the Ethernet port
 *   configured                       -> bridge mode: Wi-Fi STA <-> Ethernet bridge, web UI on the management IP
 */
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "config.h"
#include "logbuf.h"
#include "wb.h"
#include "eth.h"

static const char *TAG = "main";

#define ROLLBACK_TIMEOUT_MIN 10

wb_state_t g_state;
static volatile bool s_healthy;

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS((uint32_t)arg));
    esp_restart();
}

void wb_restart(const char *why, uint32_t delay_ms)
{
    ESP_LOGW(TAG, "restarting: %s", why);
    xTaskCreate(restart_task, "restart", 2048, (void *)delay_ms, 10, NULL);
}

esp_err_t wb_reboot_to(const char *target, uint32_t delay_ms)
{
    if (strcmp(target, "safeboot") == 0) {
        const esp_partition_t *f = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
        if (!ota_safeboot_ready() || esp_ota_set_boot_partition(f) != ESP_OK) {
            ESP_LOGE(TAG, "can't restart into safeboot: no valid safeboot firmware installed");
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGW(TAG, "next boot: %s (safeboot); it answers at the management address (or its own access point if Wi-Fi fails)", f->label);
    } else if (strcmp(target, "setup") == 0) {
        nvs_flag_set("setup_once", 1);
    }
    wb_restart(target, delay_ms);
    return ESP_OK;
}

void wb_mark_healthy(const char *why)
{
    if (s_healthy) {
        return;
    }
    s_healthy = true;
    nvs_flag_set("new_fw", 0);
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed (%s)", why);
    }
}

static void rollback_guard(void *arg)
{
    if (!s_healthy) {
        esp_ota_img_states_t st;
        if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGE(TAG, "new firmware never reached Wi-Fi or the web UI in %d min: rolling back", ROLLBACK_TIMEOUT_MIN);
            nvs_flag_set("new_fw", 0);
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
    }
}

static void banner(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    printf("\n"
           "  __        ___ _____ _   ____       _     _\n"
           "  \\ \\      / (_)  ___(_) | __ ) _ __(_) __| | __ _  ___\n"
           "   \\ \\ /\\ / /| | |_  | | |  _ \\| '__| |/ _` |/ _` |/ _ \\\n"
           "    \\ V  V / | |  _| | | | |_) | |  | | (_| | (_| |  __/\n"
           "     \\_/\\_/  |_|_|   |_| |____/|_|  |_|\\__,_|\\__, |\\___|\n"
           "                                             |___/   v%s\n\n", d->version);
}

static void howto(void)
{
    if (g_state.mode == MODE_SETUP) {
        ip4_addr_t ap = { .addr = wifi_ap_ip() };
        ESP_LOGI(TAG, "SETUP MODE: join Wi-Fi \"%s\"%s and open http://%s/", g_cfg.hostname,
                 g_cfg.ap_pass[0] ? " (password protected)" : "", ip4addr_ntoa(&ap));
        ESP_LOGI(TAG, "            or plug a computer into the Ethernet port and open http://%s/", WB_ETH_SETUP_IP);
        ESP_LOGI(TAG, "            or configure over serial: SSID1 <name>, Password1 <pw>, IPAddress <ip>, Gateway <ip>, Restart 1");
    } else {
        ip4_addr_t a = { .addr = g_cfg.ip };
        ESP_LOGI(TAG, "BRIDGE MODE: Wi-Fi \"%s\", web UI at http://%s/ and http://%s.local/", g_cfg.ssid1,
                 ip4addr_ntoa(&a), g_cfg.hostname);
    }
    ESP_LOGI(TAG, "serial console: type Help");
}

void app_main(void)
{
    logbuf_init();                               /* capture everything for the web console from here on */
    banner();

    /* NVS may also hold another safeboot firmware's settings: only erase if unusable */
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS unusable (%s): erasing", esp_err_to_name(e));
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s (settings cannot be saved)", esp_err_to_name(e));
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    config_init();
    sys_early_init();

    bool setup_once = nvs_flag_get("setup_once", 0) != 0;
    if (setup_once) {
        nvs_flag_set("setup_once", 0);
    }
    g_state.mode = (!config_is_provisioned() || setup_once || g_state.safe_mode) ? MODE_SETUP : MODE_BRIDGE;
    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "%s, board %s, partition %s (%s OTA), reset: %s, boot #%lu", g_cfg.hostname, g_cfg.board,
             run ? run->label : "?", wb_ota_mode(), sys_reset_reason(), (unsigned long)g_state.boot_count);

    if (g_state.mode == MODE_SETUP && config_is_provisioned()) {
        /* a configured bridge put into setup mode (or safe mode) must not stay offline forever */
        sys_setup_timeout_start();
    }
    console_start();
    bool eth_ok = !g_state.safe_mode && eth_hw_init() == ESP_OK;

    if (g_state.mode == MODE_SETUP) {
        wifi_init(MODE_SETUP);
        if (eth_ok) {
            eth_start_setup();
        }
        captive_dns_start();
        web_start();
        wb_mark_healthy("setup portal running");
    } else {
        wifi_init(MODE_BRIDGE);
        bridge_start();
        web_start();
        captive_dns_start();                     /* for the fallback AP */
        sys_net_services_start();
        mqtt_start();
    }
    logbuf_start_syslog();
    howto();

    esp_timer_handle_t guard;
    /* Selecting a slot again (the safeboot firmware does this on every visit) also marks it as new. Only
     * an image that was just installed should be rolled back: confirm anything else straight away. */
    esp_ota_img_states_t ost;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &ost) == ESP_OK &&
        ost == ESP_OTA_IMG_PENDING_VERIFY && !nvs_flag_get("new_fw", 0)) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "firmware confirmed (selected again, not newly installed)");
    }
    esp_timer_create_args_t ga = { .callback = rollback_guard, .name = "rollback" };
    esp_timer_create(&ga, &guard);
    esp_timer_start_once(guard, (uint64_t)ROLLBACK_TIMEOUT_MIN * 60 * 1000000);

    if (g_state.mode == MODE_BRIDGE) {
        /* Ethernet: keep retrying (status page shows the state), never crash */
        int tries = 0;
        while (!eth_ok) {
            vTaskDelay(pdMS_TO_TICKS(10000));
            eth_ok = eth_hw_init() == ESP_OK;
            if (!eth_ok && ++tries % 6 == 0) {
                ESP_LOGE(TAG, "Ethernet still failing: check the board setting (EthBoard) in the web UI");
            }
        }
        eth_start_bridge();
    }

    for (int tick = 1;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (tick % 30) {
            continue;
        }
        ESP_LOGI(TAG, "heap %lu (min %lu), Wi-Fi %s, Ethernet %s", (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)esp_get_minimum_free_heap_size(), wifi_connected() ? "up" : "down",
                 eth_link_up() ? "up" : "down");
    }
}
