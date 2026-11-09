/*
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 *
 * Command interpreter, shared by the serial console (UART0, 115200 8N1), the web console,
 * GET /cm?cmnd=... and MQTT cmnd/<topic>/<Command>.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "logbuf.h"
#include "wb.h"

static const char *TAG = "cmd";
static char s_ota_url[200];

static void json_out(cJSON *o, char *out, size_t outlen)
{
    if (!cJSON_PrintPreallocated(o, out, outlen, false)) {
        snprintf(out, outlen, "{\"error\":\"result too large\"}");
    }
    cJSON_Delete(o);
}

static void kv(char *out, size_t outlen, const char *k, const char *v)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, k, v);
    json_out(o, out, outlen);
}

static void exec_one(const char *line, char *out, size_t outlen)
{
    char cmd[32];
    while (isspace((unsigned char)*line)) {
        line++;
    }
    size_t n = 0;
    while (line[n] && !isspace((unsigned char)line[n]) && n < sizeof cmd - 1) {
        cmd[n] = line[n];
        n++;
    }
    cmd[n] = 0;
    const char *arg = line + n;
    while (isspace((unsigned char)*arg)) {
        arg++;
    }
    char argbuf[160];
    strlcpy(argbuf, arg, sizeof argbuf);
    for (size_t l = strlen(argbuf); l && isspace((unsigned char)argbuf[l - 1]); l--) {
        argbuf[l - 1] = 0;
    }
    arg = argbuf;
    int iarg = atoi(arg);

    if (!cmd[0]) {
        out[0] = 0;
        return;
    }
    if (!strcasecmp(cmd, "Help")) {
        char list[900];
        config_cmd_list(list, sizeof list);
        snprintf(out, outlen,
                 "{\"Commands\":\"Status, Config, Restart 1|2 (setup)|3 (safeboot), Reset 1 (all)|2 (Wi-Fi), Scan, "
                 "OtaUrl <url>, Upgrade 1, Time, Boards, CoreDump [0 = erase], Sleep <0..250>, Ap 1|0, Backlog <cmd>;<cmd>\",\"Settings\":\"%s\"}", list);
        return;
    }
    if (!strcasecmp(cmd, "Status") || !strcasecmp(cmd, "State")) {
        cJSON *o = cJSON_CreateObject();
        sys_status_json(o);
        json_out(o, out, outlen);
        return;
    }
    if (!strcasecmp(cmd, "Config")) {
        json_out(config_to_json(false), out, outlen);
        return;
    }
    if (!strcasecmp(cmd, "Restart") || !strcasecmp(cmd, "Setup")) {
        if (!strcasecmp(cmd, "Setup")) {
            iarg = 2;
        }
        const char *target = iarg == 1 ? "normal" : iarg == 2 ? "setup" : iarg == 3 ? "safeboot" : NULL;
        if (!target) {
            kv(out, outlen, "Restart", "1 to restart, 2 for setup mode, 3 for safeboot");
            return;
        }
        if (wb_reboot_to(target, 1000) != ESP_OK) {
            kv(out, outlen, "Restart", "no valid safeboot firmware on this device");
            return;
        }
        kv(out, outlen, "Restart", "Restarting");
        return;
    }
    if (!strcasecmp(cmd, "Reset")) {
        if (iarg != 1 && iarg != 2) {
            kv(out, outlen, "Reset", "1 to erase all settings, 2 to erase Wi-Fi settings");
            return;
        }
        if (config_reset(iarg == 2) != ESP_OK) {
            kv(out, outlen, "Reset", "could not reset settings");
            return;
        }
        wb_restart("settings reset", 1000);
        kv(out, outlen, "Reset", iarg == 1 ? "All settings erased, restarting" : "Wi-Fi settings erased, restarting");
        return;
    }
    if (!strcasecmp(cmd, "Scan")) {
        wifi_scan_start();
        json_out(wifi_scan_json(), out, outlen);
        return;
    }
    if (!strcasecmp(cmd, "OtaUrl")) {
        if (*arg) {
            strlcpy(s_ota_url, arg, sizeof s_ota_url);
        }
        kv(out, outlen, "OtaUrl", s_ota_url);
        return;
    }
    if (!strcasecmp(cmd, "Upgrade") || !strcasecmp(cmd, "Upload")) {
        char err[96];
        if (iarg != 1) {
            kv(out, outlen, "Upgrade", "Upgrade 1 flashes the firmware at OtaUrl");
        } else if (!s_ota_url[0]) {
            kv(out, outlen, "Upgrade", "set OtaUrl first");
        } else if (ota_start_url(s_ota_url, err, sizeof err) != ESP_OK) {
            kv(out, outlen, "Upgrade", err);
        } else {
            kv(out, outlen, "Upgrade", "Started");
        }
        return;
    }
    if (!strcasecmp(cmd, "Time")) {
        char t[32];
        sys_time_iso(t, sizeof t);
        kv(out, outlen, "Time", t[0] ? t : "not synchronised");
        return;
    }
    if (!strcasecmp(cmd, "Ap")) {
        if (*arg && wifi_ap_force(iarg != 0) != ESP_OK) {
            kv(out, outlen, "Ap", "setup mode always runs the access point");
            return;
        }
        char ip[16];
        esp_ip4_addr_t a = { .addr = wifi_ap_ip() };
        snprintf(ip, sizeof ip, IPSTR, IP2STR(&a));
        snprintf(out, outlen, "{\"Ap\":%d,\"SSID\":\"%s\",\"IP\":\"%s\",\"Clients\":%d}", wifi_ap_active(),
                 g_cfg.hostname, ip, wifi_ap_clients());
        return;
    }
    if (!strcasecmp(cmd, "Sleep")) {
        /* Sleep 0 = no Wi-Fi power save, 1..50 = modem sleep, > 50 = max modem sleep */
        static const char *ps[] = { "none", "min", "max" };
        if (*arg) {
            bool rb;
            char tmp[64];
            config_cmd("PowerSave", ps[iarg <= 0 ? 0 : iarg <= 50 ? 1 : 2], tmp, sizeof tmp, &rb);
            wifi_apply_runtime();
        }
        snprintf(out, outlen, "{\"Sleep\":%d,\"PowerSave\":\"%s\"}",
                 g_cfg.power_save == 0 ? 0 : g_cfg.power_save == 1 ? 50 : 250, ps[g_cfg.power_save % 3]);
        return;
    }
    if (!strcasecmp(cmd, "CoreDump")) {
        if (*arg && iarg == 0) {
            sys_crash_erase();
        }
        cJSON *o = cJSON_CreateObject();
        sys_crash_json(o);
        json_out(o, out, outlen);
        return;
    }
    if (!strcasecmp(cmd, "Boards")) {
        cJSON *o = cJSON_CreateObject();
        cJSON *a = cJSON_AddArrayToObject(o, "Boards");
        for (int i = 0; i < g_board_count; i++) {
            cJSON_AddItemToArray(a, cJSON_CreateString(g_boards[i].id));
        }
        json_out(o, out, outlen);
        return;
    }
    bool reboot = false;
    int r = config_cmd(cmd, arg, out, outlen, &reboot);
    if (r == 0) {
        char in[41];
        strlcpy(in, cmd, sizeof in);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "Command", "Unknown");
        cJSON_AddStringToObject(o, "Input", in);
        json_out(o, out, outlen);
        return;
    }
    if (r > 0 && *arg) {
        if (reboot) {
            g_state.reboot_required = true;
        } else {
            sys_apply_runtime();
            wifi_apply_runtime();
        }
    }
}

void console_exec(const char *line, char *out, size_t outlen, const char *source)
{
    out[0] = 0;
    if (strncasecmp(line, "Backlog", 7) == 0 && (line[7] == ' ' || line[7] == 0)) {
        /* run each command; keep the last result */
        char *copy = strdup(line + 7);
        char *save = NULL;
        for (char *c = copy ? strtok_r(copy, ";", &save) : NULL; c; c = strtok_r(NULL, ";", &save)) {
            exec_one(c, out, outlen);
        }
        free(copy);
    } else {
        exec_one(line, out, outlen);
    }
    /* never log secrets: only the command word */
    char w[24];
    size_t n = 0;
    while (line[n] && !isspace((unsigned char)line[n]) && n < sizeof w - 1) {
        w[n] = line[n];
        n++;
    }
    w[n] = 0;
    ESP_LOGI(TAG, "%s: %s", source, w);
}

static void serial_task(void *arg)
{
    char line[200];
    size_t n = 0;
    char *out = malloc(2048);
    while (out) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int im = improv_feed((uint8_t)c);     /* Improv packets share the port with the console */
        if (im == 2) {
            n = n >= 5 ? n - 5 : 0;           /* drop "IMPRO" from the line being typed */
            continue;
        }
        if (im == 1) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (n) {
                line[n] = 0;
                fputs("\r\n", stdout);
                console_exec(line, out, 2048, "serial");
                if (out[0]) {
                    printf("%s\n", out);
                }
                n = 0;
            }
            continue;
        }
        if ((c == 0x08 || c == 0x7f) && n) {
            n--;
            fputs("\b \b", stdout);
            fflush(stdout);
            continue;
        }
        if (isprint(c) && n < sizeof line - 1) {
            line[n++] = c;
            fputc(c, stdout);               /* echo: most serial monitors don't echo locally */
            fflush(stdout);
        }
    }
    vTaskDelete(NULL);
}

static void console_shutdown(void)
{
    uart_disable_intr_mask(CONFIG_ESP_CONSOLE_UART_NUM, 0x7FFFF);
    uart_clear_intr_status(CONFIG_ESP_CONSOLE_UART_NUM, 0x7FFFF);
}

void console_start(void)
{
    /* blocking stdin on UART0 via the driver (default VFS reads are non-blocking) */
    if (uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 512, 0, 0, NULL, 0) == ESP_OK) {
        uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
        esp_register_shutdown_handler(console_shutdown);
    }
    setvbuf(stdin, NULL, _IONBF, 0);
    xTaskCreate(serial_task, "console", 8192, NULL, 3, NULL);
    ESP_LOGI(TAG, "serial console ready: type Help");
}
