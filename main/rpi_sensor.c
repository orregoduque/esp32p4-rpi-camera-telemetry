/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_cam_sensor_detect.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sccb_i2c.h"
#include "rpi_sensor.h"

static const char *TAG = "sensor_init";

#define RPI_CAM_SCCB_FREQ_HZ    (100 * 1000)

static esp_cam_sensor_device_t *s_cam_dev;

void rpi_sensor_init(rpi_sensor_config_t *sensor_config, rpi_sensor_handle_t *out_sensor_handle)
{
    i2c_master_bus_config_t i2c_bus_conf = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = sensor_config->i2c_sda_io_num,
        .scl_io_num = sensor_config->i2c_scl_io_num,
        .i2c_port = sensor_config->i2c_port_num,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus_handle = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_conf, &i2c_bus_handle));

    esp_cam_sensor_config_t cam_config = {
        .reset_pin = -1,
        .pwdn_pin = -1,
        .xclk_pin = -1,
    };

    esp_cam_sensor_device_t *cam = NULL;
    for (esp_cam_sensor_detect_fn_t *p = &__esp_cam_sensor_detect_fn_array_start;
         p < &__esp_cam_sensor_detect_fn_array_end; ++p) {
        sccb_i2c_config_t i2c_config = {
            .scl_speed_hz = RPI_CAM_SCCB_FREQ_HZ,
            .device_address = p->sccb_addr,
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        };
        ESP_ERROR_CHECK(sccb_new_i2c_io(i2c_bus_handle, &i2c_config, &cam_config.sccb_handle));

        cam_config.sensor_port = p->port;
        cam = (*(p->detect))(&cam_config);
        if (cam) {
            if (p->port != sensor_config->port) {
                ESP_LOGE(TAG, "detect a camera sensor with mismatched interface");
                return;
            }
            break;
        }
        ESP_ERROR_CHECK(esp_sccb_del_i2c_io(cam_config.sccb_handle));
    }

    if (!cam) {
        ESP_LOGE(TAG, "failed to detect camera sensor");
        return;
    }

    s_cam_dev = cam;

    esp_cam_sensor_format_array_t cam_fmt_array = {0};
    esp_cam_sensor_query_format(cam, &cam_fmt_array);
    const esp_cam_sensor_format_t *parray = cam_fmt_array.format_array;
    for (int i = 0; i < cam_fmt_array.count; i++) {
        ESP_LOGI(TAG, "fmt[%d].name:%s", i, parray[i].name);
    }

    esp_cam_sensor_format_t *cam_cur_fmt = NULL;
    for (int i = 0; i < cam_fmt_array.count; i++) {
        if (!strcmp(parray[i].name, sensor_config->format_name)) {
            cam_cur_fmt = (esp_cam_sensor_format_t *)&parray[i];
        }
    }
    if (!cam_cur_fmt) {
        ESP_LOGE(TAG, "Unsupported format");
        ESP_ERROR_CHECK(ESP_ERR_INVALID_ARG);
    }

    ESP_ERROR_CHECK(esp_cam_sensor_set_format(cam, cam_cur_fmt));
    ESP_LOGI(TAG, "Format in use:%s", cam_cur_fmt->name);

    int enable_flag = 1;
    ESP_ERROR_CHECK(esp_cam_sensor_ioctl(cam, ESP_CAM_SENSOR_IOC_S_STREAM, &enable_flag));

    out_sensor_handle->i2c_bus_handle = i2c_bus_handle;
    out_sensor_handle->sccb_handle = cam_config.sccb_handle;
}

esp_err_t rpi_sensor_stop_stream(void)
{
    if (!s_cam_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    int enable_flag = 0;
    esp_err_t err = esp_cam_sensor_ioctl(s_cam_dev, ESP_CAM_SENSOR_IOC_S_STREAM, &enable_flag);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OV5647 MIPI stream stopped");
    } else {
        ESP_LOGW(TAG, "stream stop failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t rpi_sensor_restart_stream(void)
{
    if (!s_cam_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    int enable_flag = 1;
    esp_err_t err = esp_cam_sensor_ioctl(s_cam_dev, ESP_CAM_SENSOR_IOC_S_STREAM, &enable_flag);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OV5647 MIPI stream restarted");
    } else {
        ESP_LOGW(TAG, "stream restart failed: %s", esp_err_to_name(err));
    }
    return err;
}
