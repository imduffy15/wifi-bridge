/*
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 *
 * System services: power-cycle factory reset, status LED, button, SNTP, mDNS, CPU frequency, status JSON.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_private/esp_clk.h"
#include "esp_netif_sntp.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "esp_core_dump.h"
#include "lwip/ip4_addr.h"
#include "config.h"
#include "logbuf.h"
#include "wb.h"
#include "eth.h"

static const char *TAG = "system";

#define POWERCYCLE_COUNT 6

static esp_timer_handle_t s_pc_timer;
static bool s_time_synced;

static void pc_clear(void *arg)
{
    nvs_flag_set("pcycles", 0);
}

static void crash_clear(void *arg)
{
    nvs_flag_set("crashes", 0);           /* up for a minute: not a boot loop */
}

/* ---- LED: fast blink = setup, slow blink = connecting, double blink = fallback AP, on = bridging ---- */

static void led_task(void *arg)
{
    int pin = g_cfg.led_pin;
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    uint32_t t = 0;
    while (true) {
        bool on;
        if (g_state.mode == MODE_SETUP) {
            on = (t / 2) % 2;                                 /* 200 ms period */
        } else if (wifi_ap_active() && !wifi_connected()) {
            uint32_t ph = t % 20;
            on = ph == 0 || ph == 3;                           /* double blink every 2 s */
        } else if (!wifi_connected()) {
            on = (t / 5) % 2;                                 /* 1 s period */
        } else {
            on = true;
        }
        gpio_set_level(pin, on ^ g_cfg.led_inverted);
        t++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ---- button (active low, internal pull-up): hold 3 s = setup mode, hold 12 s = factory reset ---- */

static void button_task(void *arg)
{
    int pin = g_cfg.button_pin;
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(500));
    if (gpio_get_level(pin) == 0) {
        ESP_LOGW(TAG, "button pin %d is low at boot: ignoring the button", pin);
        vTaskDelete(NULL);
        return;
    }
    uint32_t held = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(pin) == 0) {
            held += 50;
            if (held == 3000) {
                ESP_LOGW(TAG, "button: release now for setup mode, keep holding for factory reset");
            }
            if (held >= 12000) {
                ESP_LOGW(TAG, "button: factory reset");
                if (config_reset(false) == ESP_OK) {
                    wb_restart("button factory reset", 500);
                    vTaskDelay(portMAX_DELAY);
                }
                /* Wait for release before another attempt after a storage failure. */
                while (gpio_get_level(pin) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                held = 0;
            }
        } else {
            if (held >= 3000) {
                wb_reboot_to("setup", 500);
            }
            held = 0;
        }
    }
}

static void crash_load(void);

/* setup/safe mode on a configured bridge shouldn't strand it: restart if nobody uses the web UI */
static void setup_timeout_check(void *arg)
{
    if (esp_timer_get_time() - g_state.last_web_us > 10 * 60 * 1000000LL) {
        wb_restart("setup mode unused for 10 minutes, returning to bridge mode", 100);
    }
}

void sys_setup_timeout_start(void)
{
    static esp_timer_handle_t t;
    esp_timer_create_args_t a = { .callback = setup_timeout_check, .name = "setup_to" };
    if (!t && esp_timer_create(&a, &t) == ESP_OK) {
        esp_timer_start_periodic(t, 60 * 1000000LL);
        ESP_LOGI(TAG, "returning to bridge mode after 10 minutes without web activity");
    }
}

void sys_early_init(void)
{
    crash_load();
    g_state.boot_count = nvs_flag_get("boots", 0) + 1;
    nvs_flag_set("boots", g_state.boot_count);

    esp_reset_reason_t rr = esp_reset_reason();
    bool crashed = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT;
    int32_t crashes = crashed ? nvs_flag_get("crashes", 0) + 1 : 0;
    nvs_flag_set("crashes", crashes);
    if (crashes >= 3) {
        g_state.safe_mode = true;
        ESP_LOGE(TAG, "SAFE MODE: %ld crashes in a row shortly after boot. Starting only the setup portal and web UI "
                 "(no Ethernet, LED, button or MQTT) so the firmware or settings can be fixed", (long)crashes);
    }
    static esp_timer_handle_t crash_timer;
    esp_timer_create_args_t ca = { .callback = crash_clear, .name = "crashes" };
    if (crashes && esp_timer_create(&ca, &crash_timer) == ESP_OK) {
        esp_timer_start_once(crash_timer, 60 * 1000000);
    }
    if (g_cfg.powercycle_reset && (rr == ESP_RST_POWERON || rr == ESP_RST_EXT || rr == ESP_RST_BROWNOUT)) {
        int32_t n = nvs_flag_get("pcycles", 0) + 1;
        if (n >= POWERCYCLE_COUNT) {
            ESP_LOGW(TAG, "%d quick power cycles: factory reset", POWERCYCLE_COUNT);
            if (config_reset(false) == ESP_OK) {
                esp_restart();
            }
            n = 0;
        }
        nvs_flag_set("pcycles", n);
        if (n > 1) {
            ESP_LOGW(TAG, "quick power cycle %ld of %d (factory reset at %d)", (long)n, POWERCYCLE_COUNT, POWERCYCLE_COUNT);
        }
        esp_timer_create_args_t a = { .callback = pc_clear, .name = "pcycle" };
        esp_timer_create(&a, &s_pc_timer);
        esp_timer_start_once(s_pc_timer, 10 * 1000000);     /* stayed up 10 s: not a quick cycle */
    } else if (!crashed) {
        nvs_flag_set("pcycles", 0);           /* a crash in between doesn't break a power-cycle sequence */
    }
    if (g_cfg.led_pin >= 0 && !g_state.safe_mode) {
        xTaskCreate(led_task, "led", 2048, NULL, 1, NULL);
    }
    if (g_cfg.button_pin >= 0 && !g_state.safe_mode) {
        xTaskCreate(button_task, "button", 4096, NULL, 2, NULL);
    }
    sys_apply_runtime();
}

/* ---- crash report (core dump in flash, written by the panic handler) ---- */

static esp_core_dump_summary_t *s_crash;
static size_t s_crash_size;

static void crash_load(void)
{
    size_t addr;
    free(s_crash);
    s_crash = NULL;
    s_crash_size = 0;
    if (esp_core_dump_image_check() != ESP_OK || esp_core_dump_image_get(&addr, &s_crash_size) != ESP_OK) {
        return;
    }
    s_crash = calloc(1, sizeof *s_crash);
    if (s_crash && esp_core_dump_get_summary(s_crash) != ESP_OK) {
        free(s_crash);
        s_crash = NULL;
    }
    if (s_crash) {
        ESP_LOGW(TAG, "crash report stored: task %s, PC 0x%08lx (download from /api/coredump)", s_crash->exc_task,
                 (unsigned long)s_crash->exc_pc);
    }
}

void sys_crash_json(cJSON *o)
{
    if (!s_crash) {
        cJSON_AddNullToObject(o, "last_crash");
        return;
    }
    cJSON *c = cJSON_AddObjectToObject(o, "last_crash");
    cJSON_AddStringToObject(c, "task", s_crash->exc_task);
    char pc[12], bt[16 * 11 + 1] = "";
    snprintf(pc, sizeof pc, "0x%08lx", (unsigned long)s_crash->exc_pc);
    cJSON_AddStringToObject(c, "pc", pc);
    size_t n = 0;
    for (uint32_t i = 0; i < s_crash->exc_bt_info.depth && i < 16; i++) {
        n += snprintf(bt + n, sizeof bt - n, "%s0x%08lx", i ? " " : "", (unsigned long)s_crash->exc_bt_info.bt[i]);
    }
    cJSON_AddStringToObject(c, "backtrace", bt);
    cJSON_AddNumberToObject(c, "size", s_crash_size);
}

esp_err_t sys_crash_erase(void)
{
    esp_err_t e = esp_core_dump_image_erase();
    crash_load();
    return e;
}

static void time_synced(struct timeval *tv)
{
    if (!s_time_synced) {
        s_time_synced = true;
        char t[32];
        sys_time_iso(t, sizeof t);
        ESP_LOGI(TAG, "time synchronised: %s", t);
    }
}

void sys_net_services_start(void)
{
    if (g_cfg.ntp[0]) {
        esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG(g_cfg.ntp);
        c.sync_cb = time_synced;
        esp_netif_sntp_init(&c);
    }
#if CONFIG_WB_QEMU
    /* QEMU's soft reset doesn't clear a pending hardware timer alarm, and mDNS's 100 ms timer nearly
     * always leaves one: the next boot then crashes in esp_timer init. Real chips reset fully. */
    return;
#endif
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(g_cfg.hostname);
        mdns_instance_name_set(g_cfg.name);
        mdns_txt_item_t txt[] = {
            { "version", esp_app_get_description()->version },
            { "board", g_cfg.board },
            { "firmware", "wifi-bridge" },
        };
        mdns_service_add(NULL, "_http", "_tcp", 80, txt, sizeof txt / sizeof txt[0]);
        ESP_LOGI(TAG, "mDNS: http://%s.local/", g_cfg.hostname);
    }
}

void sys_apply_runtime(void)
{
    logbuf_set_level(g_cfg.log_level);
    setenv("TZ", g_cfg.tz, 1);
    tzset();
#if CONFIG_PM_ENABLE && !CONFIG_WB_QEMU
    /* fixed CPU clock (no light sleep: the bridge must answer frames immediately) */
    if (esp_clk_cpu_freq() / 1000000 != g_cfg.cpu_mhz) {
        esp_pm_config_t pm = { .max_freq_mhz = g_cfg.cpu_mhz, .min_freq_mhz = g_cfg.cpu_mhz, .light_sleep_enable = false };
        esp_err_t e = esp_pm_configure(&pm);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "CPU %ld MHz: %s", (long)g_cfg.cpu_mhz, esp_err_to_name(e));
        } else {
            ESP_LOGI(TAG, "CPU clock %ld MHz", (long)g_cfg.cpu_mhz);
        }
    }
#endif
    mqtt_apply_runtime();
}

void sys_time_iso(char *buf, size_t n)
{
    time_t now = time(NULL);
    if (now < 1700000000) {
        buf[0] = 0;
        return;
    }
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, n, "%Y-%m-%dT%H:%M:%S%z", &tm);
}

const char *sys_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "unknown";
    }
}

void sys_status_json(cJSON *root)
{
    cJSON_AddStringToObject(root, "mode", g_state.mode == MODE_SETUP ? "setup" : "bridge");

    cJSON *d = cJSON_AddObjectToObject(root, "device");
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(d, "name", g_cfg.name);
    cJSON_AddStringToObject(d, "hostname", g_cfg.hostname);
    cJSON_AddStringToObject(d, "version", app->version);
    cJSON_AddStringToObject(d, "idf", app->idf_ver);
    char build[40];
    snprintf(build, sizeof build, "%s %s", app->date, app->time);
    cJSON_AddStringToObject(d, "build", build);
    char elf_sha256[65];
    for (size_t i = 0; i < sizeof app->app_elf_sha256; i++) {
        snprintf(elf_sha256 + i * 2, 3, "%02x", app->app_elf_sha256[i]);
    }
    cJSON_AddStringToObject(d, "elf_sha256", elf_sha256);
    cJSON_AddStringToObject(d, "board", g_cfg.board);
    const esp_partition_t *run = esp_ota_get_running_partition();
    cJSON_AddStringToObject(d, "partition", run ? run->label : "?");
    cJSON_AddStringToObject(d, "ota_mode", wb_ota_mode());
    ota_safeboot_json(d);
    cJSON_AddNumberToObject(d, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(d, "reset_reason", sys_reset_reason());
    cJSON_AddNumberToObject(d, "boot_count", g_state.boot_count);
    cJSON_AddNumberToObject(d, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(d, "min_free_heap", esp_get_minimum_free_heap_size());
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    char chip[40];
    snprintf(chip, sizeof chip, "ESP32 rev %d.%d, %d cores", ci.revision / 100, ci.revision % 100, ci.cores);
    cJSON_AddStringToObject(d, "chip", chip);
    cJSON_AddNumberToObject(d, "cpu_mhz", g_cfg.cpu_mhz);
    uint32_t fs = 0;
    esp_flash_get_size(NULL, &fs);
    cJSON_AddNumberToObject(d, "flash_mb", fs / (1024 * 1024));
    uint8_t m[6];
    char mac[18];
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(mac, sizeof mac, MACSTR, MAC2STR(m));
    cJSON_AddStringToObject(d, "mac_wifi", mac);
    eth_mac(m);
    snprintf(mac, sizeof mac, MACSTR, MAC2STR(m));
    cJSON_AddStringToObject(d, "mac_eth", mac);
    char t[32];
    sys_time_iso(t, sizeof t);
    if (t[0]) {
        cJSON_AddStringToObject(d, "time", t);
    } else {
        cJSON_AddNullToObject(d, "time");
    }
    cJSON_AddBoolToObject(d, "auth", g_cfg.admin_pass[0] != 0);
    cJSON_AddBoolToObject(d, "reboot_required", g_state.reboot_required);
    cJSON_AddBoolToObject(d, "safe_mode", g_state.safe_mode);
    sys_crash_json(d);

    wifi_status_json(cJSON_AddObjectToObject(root, "wifi"));
    eth_status_json(cJSON_AddObjectToObject(root, "ethernet"));

    cJSON *c = cJSON_AddObjectToObject(root, "client");
    if (eth_client_mac(m)) {
        snprintf(mac, sizeof mac, MACSTR, MAC2STR(m));
        cJSON_AddStringToObject(c, "mac", mac);
    } else {
        cJSON_AddNullToObject(c, "mac");
    }
    ip4_addr_t ip = { .addr = g_state.frames.client_ip };
    if (ip.addr) {
        cJSON_AddStringToObject(c, "ip", ip4addr_ntoa(&ip));
    } else {
        cJSON_AddNullToObject(c, "ip");
    }

    cJSON *f = cJSON_AddObjectToObject(root, "frames");
    cJSON_AddNumberToObject(f, "wired_to_wifi", g_state.frames.wired_to_wifi);
    cJSON_AddNumberToObject(f, "wifi_to_wired", g_state.frames.wifi_to_wired);
    cJSON_AddNumberToObject(f, "to_bridge", g_state.frames.to_me);
    cJSON_AddNumberToObject(f, "wifi_tx_fail", g_state.frames.wifi_tx_fail);
    cJSON_AddNumberToObject(f, "wifi_drop", g_state.frames.wifi_drop);
    cJSON_AddNumberToObject(f, "wired_drop", g_state.frames.wired_drop);

    cJSON *mq = cJSON_AddObjectToObject(root, "mqtt");
    cJSON_AddBoolToObject(mq, "enabled", g_cfg.mqtt_enabled);
    cJSON_AddBoolToObject(mq, "connected", mqtt_connected());

    ota_status_json(cJSON_AddObjectToObject(root, "ota"));
}
