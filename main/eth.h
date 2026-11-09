/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wired side of the bridge (derived from the ESP-IDF sta2eth example, originally Unlicense OR CC0-1.0).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum { FROM_WIRED, TO_WIRED } mac_spoof_direction_t;

/* Rewrite MACs (and DHCP chaddr / client-id) so the wired device appears on Wi-Fi under own_mac. */
void mac_spoof(mac_spoof_direction_t direction, uint8_t *buffer, uint16_t len, const uint8_t own_mac[6]);
/* Transmit on the wired port (copies the frame; the caller keeps ownership of buffer). */
esp_err_t eth_transmit(void *buffer, uint16_t len);
bool eth_client_mac(uint8_t out[6]);

/* implemented in bridge.c: a frame arrived on the wired port (buffer freed by the caller after return) */
void bridge_from_wired(uint8_t *buffer, uint16_t len);
