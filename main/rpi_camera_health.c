/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "rpi_camera_config.h"
#include "rpi_camera_health.h"

static const char *TAG = "rpi_health";

#define RPI_HEALTH_NVS_NAMESPACE    "rpi_health"
#define RPI_HEALTH_NVS_BOOT_KEY     "boot_count"

static uint32_t s_boot_count;
static uint32_t s_frame_timeouts;
static uint32_t s_cam_errors;

static const char *reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
    }
}

static void request_recovery(const char *reason)
{
    ESP_LOGE(TAG, "recovery restart: %s (boots=%" PRIu32 " frame_to=%" PRIu32 " cam_err=%" PRIu32 ")",
             reason, s_boot_count, s_frame_timeouts, s_cam_errors);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

esp_err_t rpi_camera_health_init(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGW(TAG, "reset reason: %s", reset_reason_str(reason));

    nvs_handle_t handle;
    s_boot_count = 1;
    if (nvs_open(RPI_HEALTH_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        uint32_t stored = 0;
        if (nvs_get_u32(handle, RPI_HEALTH_NVS_BOOT_KEY, &stored) == ESP_OK) {
            s_boot_count = stored + 1;
        }
        nvs_set_u32(handle, RPI_HEALTH_NVS_BOOT_KEY, s_boot_count);
        nvs_commit(handle);
        nvs_close(handle);
    }

    ESP_LOGI(TAG, "boot count: %" PRIu32, s_boot_count);
    ESP_LOGI(TAG, "task WDT timeout: %d s (subscribe each worker task)", RPI_HEALTH_WDT_TIMEOUT_SEC);

    s_frame_timeouts = 0;
    s_cam_errors = 0;
    return ESP_OK;
}

esp_err_t rpi_camera_health_register_current_task(void)
{
    esp_err_t err = esp_task_wdt_add(NULL);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "task WDT subscribed: %s", pcTaskGetName(NULL));
    } else if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "task WDT add failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

void rpi_camera_health_feed(void)
{
    esp_err_t err = esp_task_wdt_reset();
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGD(TAG, "wdt reset: %s", esp_err_to_name(err));
    }
}

void rpi_camera_health_on_frame_ok(void)
{
    s_frame_timeouts = 0;
}

void rpi_camera_health_on_frame_timeout(void)
{
    s_frame_timeouts++;
    if (s_frame_timeouts >= RPI_HEALTH_MAX_FRAME_TIMEOUTS) {
        request_recovery("camera frame timeouts");
    }
}

void rpi_camera_health_on_cam_error(void)
{
    s_cam_errors++;
    if (s_cam_errors >= RPI_HEALTH_MAX_CAM_ERRORS) {
        request_recovery("camera receive errors");
    }
}

uint32_t rpi_camera_health_get_boot_count(void)
{
    return s_boot_count;
}
