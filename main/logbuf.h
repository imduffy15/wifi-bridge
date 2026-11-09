/*
 * SPDX-FileCopyrightText: 2026 Ian Duffy
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* return false to stop iterating */
typedef bool (*logbuf_cb_t)(const uint32_t *seq, uint32_t t_ms, char lvl, const char *msg, void *ctx);

void logbuf_init(void);
void logbuf_start_syslog(void);
void logbuf_set_level(int lvl);           /* wb_log_level_t */
void logbuf_add(char lvl, const char *msg);   /* add a line (also printed on UART) */
/* calls cb for each stored line with seq > since; returns the seq to pass as "since" next time */
uint32_t logbuf_read(uint32_t since, logbuf_cb_t cb, void *ctx);
