/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Flash-backed FIFO spool for telemetry packets when TCP upload fails.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t rpi_camera_spool_init(void);
esp_err_t rpi_camera_spool_store(const uint8_t *packet, size_t packet_len, uint32_t sequence);
esp_err_t rpi_camera_spool_flush_one(bool (*send_fn)(const uint8_t *packet, size_t packet_len, uint32_t sequence, void *ctx), void *ctx);
uint32_t rpi_camera_spool_pending_count(void);
