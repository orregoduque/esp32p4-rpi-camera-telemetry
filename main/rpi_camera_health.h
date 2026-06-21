/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Watchdog feeding and automatic recovery for long-running operation.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t rpi_camera_health_init(void);
esp_err_t rpi_camera_health_register_current_task(void);
void rpi_camera_health_feed(void);
void rpi_camera_health_on_frame_ok(void);
void rpi_camera_health_on_frame_timeout(void);
void rpi_camera_health_on_cam_error(void);
uint32_t rpi_camera_health_get_boot_count(void);
