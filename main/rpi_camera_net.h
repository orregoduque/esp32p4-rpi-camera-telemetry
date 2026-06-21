/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t rpi_camera_net_init(void);
esp_err_t rpi_camera_net_wait_for_ip(int timeout_ms);
const char *rpi_camera_net_get_ip_str(void);
bool rpi_camera_net_is_ready(void);
