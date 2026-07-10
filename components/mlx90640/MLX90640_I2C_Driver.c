/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-IDF I2C glue for the Melexis MLX90640 driver.
 */

#include <string.h>

#include "MLX90640_I2C_Driver.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mlx90640_i2c";

static i2c_master_dev_handle_t s_dev;
static int s_i2c_freq_hz = 100000;
static uint8_t s_i2c_raw_buf[2048];

void MLX90640_I2CInit(void)
{
}

void MLX90640_I2CFreqSet(int freq)
{
    s_i2c_freq_hz = freq;
    (void)s_i2c_freq_hz;
}

esp_err_t mlx90640_i2c_bind_device(i2c_master_bus_handle_t bus, uint8_t addr_7bit)
{
    if (s_dev) {
        return ESP_OK;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr_7bit,
        .scl_speed_hz = s_i2c_freq_hz,
    };

    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02x failed: %s", addr_7bit, esp_err_to_name(err));
    }
    return err;
}

int MLX90640_I2CGeneralReset(void)
{
    return 0;
}

int MLX90640_I2CRead(uint8_t slaveAddr, uint16_t startAddress, uint16_t nMemAddressRead, uint16_t *data)
{
    if (!s_dev || !data || nMemAddressRead == 0) {
        return -1;
    }

    uint8_t addr[2] = { (uint8_t)(startAddress >> 8), (uint8_t)(startAddress & 0xFF) };
    size_t raw_len = (size_t)nMemAddressRead * sizeof(uint16_t);
    if (raw_len > sizeof(s_i2c_raw_buf)) {
        return -1;
    }

    esp_err_t err = i2c_master_transmit_receive(s_dev, addr, sizeof(addr), s_i2c_raw_buf, raw_len,
                                                pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C read 0x%02x @0x%04x failed: %s", slaveAddr, startAddress, esp_err_to_name(err));
        return -MLX90640_I2C_NACK_ERROR;
    }

    /* Melexis 16-bit words on this module: MSB first on the wire. */
    for (uint16_t i = 0; i < nMemAddressRead; i++) {
        data[i] = ((uint16_t)s_i2c_raw_buf[i * 2] << 8) | s_i2c_raw_buf[i * 2 + 1];
    }
    return MLX90640_NO_ERROR;
}

int MLX90640_I2CWrite(uint8_t slaveAddr, uint16_t writeAddress, uint16_t data)
{
    if (!s_dev) {
        return -1;
    }

    uint8_t buf[4] = {
        (uint8_t)(writeAddress >> 8),
        (uint8_t)(writeAddress & 0xFF),
        (uint8_t)(data >> 8),
        (uint8_t)(data & 0xFF),
    };

    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C write 0x%02x @0x%04x failed: %s", slaveAddr, writeAddress, esp_err_to_name(err));
        return -MLX90640_I2C_NACK_ERROR;
    }
    return MLX90640_NO_ERROR;
}
