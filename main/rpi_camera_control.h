/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "rpi_camera_control_packet.h"

esp_err_t rpi_camera_control_init(void);
rpi_slave_state_t rpi_camera_control_get_state(void);
void rpi_camera_control_set_state(rpi_slave_state_t state);
bool rpi_camera_control_capture_enabled(void);
bool rpi_camera_control_take_restart(void);
esp_err_t rpi_camera_control_wait_for_start(int timeout_ms);
void rpi_camera_control_signal_start(void);
void rpi_camera_control_fill_state_msg(rpi_ctrl_state_msg_t *msg, uint32_t cmd_id);
uint32_t rpi_camera_control_handle_command(uint32_t cmd_id, uint16_t command, rpi_ctrl_ack_t *ack_out);
