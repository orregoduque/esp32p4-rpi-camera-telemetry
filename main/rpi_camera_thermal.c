/*

 * SPDX-License-Identifier: Apache-2.0

 *

 * MLX90640 32x24 thermal array -> upscaled RGB565 heatmap for JPEG uplink.

 */



#include "rpi_camera_thermal.h"



#include <inttypes.h>

#include <math.h>

#include <stdlib.h>

#include <string.h>



#include "MLX90640_API.h"

#include "MLX90640_I2C_Driver.h"

#include "esp_check.h"

#include "esp_log.h"

#include "esp_timer.h"

#include "freertos/FreeRTOS.h"

#include "freertos/task.h"

#include "rpi_camera_config.h"
#include "rpi_camera_health.h"



static const char *TAG = "rpi_thermal";



static paramsMLX90640 s_params;

static uint16_t s_ee_data[MLX90640_EEPROM_DUMP_NUM];

static uint16_t s_frame_data[834];

static float s_pixels[MLX90640_PIXEL_NUM];

static bool s_ready;

static float s_last_spot_c;

static bool s_last_spot_valid;



static bool mlx90640_frame_ok(int err)

{

    return err >= 0;

}



static bool mlx90640_extract_ok(int err)

{

    if (err == MLX90640_NO_ERROR) {

        return true;

    }

    if (err == -MLX90640_BROKEN_PIXELS_NUM_ERROR || err == -MLX90640_OUTLIER_PIXELS_NUM_ERROR ||

        err == -MLX90640_BAD_PIXELS_NUM_ERROR || err == -MLX90640_ADJACENT_BAD_PIXELS_ERROR) {

        ESP_LOGW(TAG, "ExtractParameters calibration warning (%d), continuing", err);

        return true;

    }

    return false;

}



static void mlx90640_kick_streaming(void)

{

    int err = MLX90640_SetRefreshRate(RPI_THERMAL_I2C_ADDR, RPI_THERMAL_REFRESH_HZ);

    if (err != MLX90640_NO_ERROR) {

        ESP_LOGW(TAG, "SetRefreshRate kick failed (%d)", err);

    }

    err = MLX90640_SetChessMode(RPI_THERMAL_I2C_ADDR);

    if (err != MLX90640_NO_ERROR) {

        ESP_LOGW(TAG, "SetChessMode kick failed (%d)", err);

    }

}



static int mlx90640_read_frame_after_ready(uint8_t slave_addr, uint16_t *frame_data, uint16_t status_register)
{
    uint16_t aux[MLX90640_AUX_NUM];
    uint16_t control_register1;
    int error;

    error = MLX90640_I2CWrite(slave_addr, MLX90640_STATUS_REG, MLX90640_INIT_STATUS_VALUE);
    if (error == -MLX90640_I2C_NACK_ERROR) {
        return error;
    }

    error = MLX90640_I2CRead(slave_addr, MLX90640_PIXEL_DATA_START_ADDRESS, MLX90640_PIXEL_NUM, frame_data);
    if (error != MLX90640_NO_ERROR) {
        return error;
    }

    error = MLX90640_I2CRead(slave_addr, MLX90640_AUX_DATA_START_ADDRESS, MLX90640_AUX_NUM, aux);
    if (error != MLX90640_NO_ERROR) {
        return error;
    }

    error = MLX90640_I2CRead(slave_addr, MLX90640_CTRL_REG, 1, &control_register1);
    if (error != MLX90640_NO_ERROR) {
        return error;
    }

    frame_data[832] = control_register1;
    frame_data[833] = MLX90640_GET_FRAME(status_register);

    for (uint8_t cnt = 0; cnt < MLX90640_AUX_NUM; cnt++) {
        frame_data[cnt + MLX90640_PIXEL_NUM] = aux[cnt];
    }

    return frame_data[833];
}

static int mlx90640_get_frame_data_timed(uint8_t slave_addr, uint16_t *frame_data, int timeout_ms)
{
    int64_t deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    uint16_t status_register = 0;
    int error;

    while (esp_timer_get_time() < deadline_us) {
        error = MLX90640_I2CRead(slave_addr, MLX90640_STATUS_REG, 1, &status_register);
        if (error != MLX90640_NO_ERROR) {
            return error;
        }
        if (MLX90640_GET_DATA_READY(status_register)) {
            return mlx90640_read_frame_after_ready(slave_addr, frame_data, status_register);
        }
        rpi_camera_health_feed();
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    ESP_LOGW(TAG, "data-ready timeout (last status=0x%04x)", status_register);
    return -MLX90640_FRAME_DATA_ERROR;
}

static esp_err_t mlx90640_capture_full_frame(float *ambient_c)
{
    const int frame_timeout_ms = (1000 / RPI_THERMAL_REFRESH_HZ) * 2 + 500;
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)frame_timeout_ms * 3000LL;
    uint8_t subpage_mask = 0;
    float tr = 25.0f;

    memset(s_pixels, 0, sizeof(s_pixels));

    while (subpage_mask != 0x03 && esp_timer_get_time() < deadline_us) {
        rpi_camera_health_feed();
        int err = mlx90640_get_frame_data_timed(RPI_THERMAL_I2C_ADDR, s_frame_data, frame_timeout_ms);
        if (!mlx90640_frame_ok(err)) {
            ESP_LOGW(TAG, "subpage read failed (%d), mask=0x%x", err, subpage_mask);
            vTaskDelay(pdMS_TO_TICKS(RPI_THERMAL_BUS_SETTLE_MS));
            continue;
        }

        uint8_t subpage = (uint8_t)(s_frame_data[833] & 0x01u);
        if (subpage > 1) {
            continue;
        }
        if (subpage_mask & (1u << subpage)) {
            continue;
        }

        tr = MLX90640_GetTa(s_frame_data, &s_params);
        MLX90640_CalculateTo(s_frame_data, &s_params, 0.95f, tr, s_pixels);
        subpage_mask |= (1u << subpage);
        ESP_LOGD(TAG, "subpage %u captured (mask=0x%x Ta=%.1fC)", subpage, subpage_mask, tr);
    }

    if (subpage_mask != 0x03) {
        ESP_LOGW(TAG, "incomplete frame (mask=0x%x)", subpage_mask);
        return ESP_FAIL;
    }

    if (ambient_c) {
        *ambient_c = tr;
    }
    return ESP_OK;
}



static uint16_t temp_to_rgb565(float norm)

{

    if (norm < 0.0f) {

        norm = 0.0f;

    }

    if (norm > 1.0f) {

        norm = 1.0f;

    }



    uint8_t r = (uint8_t)(norm * 255.0f);

    uint8_t g = (uint8_t)((1.0f - fabsf(norm - 0.5f) * 2.0f) * 180.0f);

    uint8_t b = (uint8_t)((1.0f - norm) * 255.0f);

    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));

}



static void upscale_nearest(const uint16_t *src, uint16_t *dst)

{

    for (int y = 0; y < RPI_THERMAL_JPEG_H; y++) {

        int sy = (y * RPI_THERMAL_ROWS) / RPI_THERMAL_JPEG_H;

        for (int x = 0; x < RPI_THERMAL_JPEG_W; x++) {

            int sx = (x * RPI_THERMAL_COLS) / RPI_THERMAL_JPEG_W;

            dst[y * RPI_THERMAL_JPEG_W + x] = src[sy * RPI_THERMAL_COLS + sx];

        }

    }

}



esp_err_t rpi_camera_thermal_init(i2c_master_bus_handle_t bus)

{

    s_ready = false;

    s_last_spot_valid = false;



    MLX90640_I2CFreqSet(100000);
    ESP_RETURN_ON_ERROR(mlx90640_i2c_bind_device(bus, RPI_THERMAL_I2C_ADDR), TAG, "i2c bind");

    vTaskDelay(pdMS_TO_TICKS(300));

    MLX90640_I2CInit();

    int err = MLX90640_DumpEE(RPI_THERMAL_I2C_ADDR, s_ee_data);
    ESP_RETURN_ON_FALSE(err == MLX90640_NO_ERROR, ESP_FAIL, TAG, "DumpEE failed (%d)", err);

    err = MLX90640_ExtractParameters(s_ee_data, &s_params);
    ESP_RETURN_ON_FALSE(mlx90640_extract_ok(err), ESP_FAIL, TAG, "ExtractParameters failed (%d)", err);

    mlx90640_kick_streaming();

    s_ready = true;

    ESP_LOGI(TAG, "MLX90640 ready on I2C 0x%02x (%dx%d, refresh %d Hz)",

             RPI_THERMAL_I2C_ADDR, RPI_THERMAL_COLS, RPI_THERMAL_ROWS, RPI_THERMAL_REFRESH_HZ);

    return ESP_OK;

}



bool rpi_camera_thermal_is_ready(void)

{

    return s_ready;

}



bool rpi_camera_thermal_get_last_spot_c(int16_t *temp_centi_c)

{

    if (!s_last_spot_valid || !temp_centi_c) {

        return false;

    }

    *temp_centi_c = (int16_t)(s_last_spot_c * 100.0f);

    return true;

}



esp_err_t rpi_camera_thermal_render_jpeg_rgb565(void *rgb565, size_t rgb_len,

                                                float *min_c, float *max_c, float *spot_c)

{

    ESP_RETURN_ON_FALSE(s_ready && rgb565, ESP_ERR_INVALID_STATE, TAG, "not ready");

    ESP_RETURN_ON_FALSE(rgb_len == RPI_THERMAL_RGB565_BYTES, ESP_ERR_INVALID_SIZE, TAG, "bad buffer");



    float t_min = 0.0f;
    float t_max = 0.0f;
    float tr = 25.0f;

    ESP_RETURN_ON_ERROR(mlx90640_capture_full_frame(&tr), TAG, "capture failed");

    MLX90640_BadPixelsCorrection(s_params.brokenPixels, s_pixels, 1, &s_params);
    MLX90640_BadPixelsCorrection(s_params.outlierPixels, s_pixels, 2, &s_params);

    t_min = s_pixels[0];
    t_max = s_pixels[0];

    for (int i = 1; i < MLX90640_PIXEL_NUM; i++) {

        if (s_pixels[i] < t_min) {

            t_min = s_pixels[i];

        }

        if (s_pixels[i] > t_max) {

            t_max = s_pixels[i];

        }

    }



    float span = t_max - t_min;

    if (span < 1.0f) {

        span = 1.0f;

    }



    uint16_t small[RPI_THERMAL_COLS * RPI_THERMAL_ROWS];

    for (int i = 0; i < MLX90640_PIXEL_NUM; i++) {

        float norm = (s_pixels[i] - t_min) / span;

        small[i] = temp_to_rgb565(norm);

    }



    upscale_nearest(small, (uint16_t *)rgb565);



    if (min_c) {

        *min_c = t_min;

    }

    if (max_c) {

        *max_c = t_max;

    }

    if (spot_c) {

        *spot_c = s_pixels[MLX90640_PIXEL_NUM / 2];

    }



    s_last_spot_c = s_pixels[MLX90640_PIXEL_NUM / 2];

    s_last_spot_valid = true;



    ESP_LOGI(TAG, "frame %.1f..%.1f C spot=%.1f C", t_min, t_max, s_last_spot_c);

    return ESP_OK;

}


