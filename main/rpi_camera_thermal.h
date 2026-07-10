/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t rpi_camera_thermal_init(i2c_master_bus_handle_t bus);
bool rpi_camera_thermal_is_ready(void);
bool rpi_camera_thermal_get_last_spot_c(int16_t *temp_centi_c);
esp_err_t rpi_camera_thermal_render_jpeg_rgb565(void *rgb565, size_t rgb_len,
                                                float *min_c, float *max_c, float *spot_c);
