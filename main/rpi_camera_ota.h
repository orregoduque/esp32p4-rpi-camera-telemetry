/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * TCP OTA server — receives an app .bin over Ethernet (ETOA protocol).
 * First firmware flash is always via USB; subsequent updates use this path.
 */

#pragma once

#include "esp_err.h"

esp_err_t rpi_camera_ota_start(void);
