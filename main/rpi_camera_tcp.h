/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t rpi_camera_tcp_init(void);
esp_err_t rpi_camera_tcp_maintain_connection(void);
esp_err_t rpi_camera_tcp_process_spool(void);
void rpi_camera_tcp_on_link_down(void);
esp_err_t rpi_camera_tcp_send_snapshot(const void *rgb565, size_t rgb_len, uint32_t node_id, int16_t temperature_centi_c);
uint32_t rpi_camera_tcp_get_send_failures(void);
uint32_t rpi_camera_tcp_get_spool_pending(void);
uint32_t rpi_camera_tcp_get_sequence(void);
bool rpi_camera_tcp_is_connected(void);
