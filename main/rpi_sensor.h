/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * OV5647 SCCB init (from ESP-IDF sensor_init) with stream restart after shared-bus thermal I2C.
 */

#pragma once

#include "driver/i2c_master.h"
#include "esp_cam_sensor.h"
#include "esp_err.h"

typedef struct {
    esp_sccb_io_handle_t sccb_handle;
    i2c_master_bus_handle_t i2c_bus_handle;
} rpi_sensor_handle_t;

typedef struct {
    int i2c_port_num;
    int i2c_sda_io_num;
    int i2c_scl_io_num;
    esp_cam_sensor_port_t port;
    const char *format_name;
} rpi_sensor_config_t;

void rpi_sensor_init(rpi_sensor_config_t *sensor_config, rpi_sensor_handle_t *out_sensor_handle);
esp_err_t rpi_sensor_stop_stream(void);
esp_err_t rpi_sensor_restart_stream(void);
