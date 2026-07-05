/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * OV5647 slave node — master controls START/STOP/RESTART on TCP :9001.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_private/esp_cache_private.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/isp.h"
#include "driver/isp_color.h"
#include "example_sensor_init.h"
#include "rpi_camera_config.h"
#include "rpi_camera_control.h"
#include "rpi_camera_ctrl_server.h"
#include "rpi_camera_health.h"
#include "rpi_camera_net.h"
#include "rpi_camera_ota.h"
#include "rpi_camera_tcp.h"

static const char *TAG = "rpi_camera";

#define RPI_CAM_FRAME_BUF_CAPS    (MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA)

typedef struct {
    esp_ldo_channel_handle_t ldo;
    esp_cam_ctlr_handle_t cam;
    isp_proc_handle_t isp;
    void *frame_buffer;
    size_t frame_buffer_size;
    esp_cam_ctlr_trans_t trans;
    SemaphoreHandle_t frame_done;
    bool pipeline_ready;
    int64_t warmup_start_us;
    bool warmup_done;
} camera_ctx_t;

static camera_ctx_t s_cam;

static size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static bool IRAM_ATTR camera_trans_finished(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    BaseType_t woken = pdFALSE;
    if (s_cam.frame_done) {
        xSemaphoreGiveFromISR(s_cam.frame_done, &woken);
    }
    return woken == pdTRUE;
}

static esp_err_t camera_pipeline_init(void)
{
    esp_ldo_channel_config_t ldo_config = {
        .chan_id = RPI_CAM_LDO_CHAN_ID,
        .voltage_mv = RPI_CAM_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, &s_cam.ldo), TAG, "ldo");

    example_sensor_handle_t sensor_handle = {
        .sccb_handle = NULL,
        .i2c_bus_handle = NULL,
    };
    example_sensor_config_t sensor_config = {
        .i2c_port_num = I2C_NUM_0,
        .i2c_sda_io_num = RPI_CAM_SCCB_SDA_IO,
        .i2c_scl_io_num = RPI_CAM_SCCB_SCL_IO,
        .port = ESP_CAM_SENSOR_MIPI_CSI,
        .format_name = RPI_CAM_FORMAT_NAME,
    };
    example_sensor_init(&sensor_config, &sensor_handle);
    ESP_RETURN_ON_FALSE(sensor_handle.i2c_bus_handle, ESP_FAIL, TAG, "sensor init");

    esp_cam_ctlr_csi_config_t csi_config = {
        .ctlr_id = 0,
        .h_res = RPI_CAM_HRES,
        .v_res = RPI_CAM_VRES,
        .lane_bit_rate_mbps = RPI_CAM_MIPI_LANE_BITRATE_MBPS,
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RAW8,
        .data_lane_num = 2,
        .byte_swap_en = false,
        .queue_items = 2,
    };
    ESP_RETURN_ON_ERROR(esp_cam_new_csi_ctlr(&csi_config, &s_cam.cam), TAG, "csi");

    size_t cache_line_size = 0;
    ESP_RETURN_ON_ERROR(esp_cache_get_alignment(RPI_CAM_FRAME_BUF_CAPS, &cache_line_size), TAG, "cache align");
    s_cam.frame_buffer_size = align_up(RPI_CAM_FRAME_BYTES, cache_line_size);
    s_cam.frame_buffer = heap_caps_aligned_alloc(cache_line_size, s_cam.frame_buffer_size, RPI_CAM_FRAME_BUF_CAPS);
    ESP_RETURN_ON_FALSE(s_cam.frame_buffer, ESP_ERR_NO_MEM, TAG, "frame buf");

    s_cam.trans.buffer = s_cam.frame_buffer;
    s_cam.trans.buflen = s_cam.frame_buffer_size;

    s_cam.frame_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_cam.frame_done, ESP_ERR_NO_MEM, TAG, "sem");

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = NULL,
        .on_trans_finished = camera_trans_finished,
    };
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_register_event_callbacks(s_cam.cam, &cbs, NULL), TAG, "csi cbs");
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_enable(s_cam.cam), TAG, "csi enable");

    esp_isp_processor_cfg_t isp_config = {
        .clk_hz = 80 * 1000 * 1000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .has_line_start_packet = false,
        .has_line_end_packet = false,
        .h_res = RPI_CAM_HRES,
        .v_res = RPI_CAM_VRES,
    };
    ESP_RETURN_ON_ERROR(esp_isp_new_processor(&isp_config, &s_cam.isp), TAG, "isp");
    esp_isp_color_config_t color_config = {
        .color_contrast = { .integer = 1, .decimal = 0 },
        .color_saturation = { .integer = 1, .decimal = 0 },
        .color_hue = 0,
        .color_brightness = RPI_ISP_BRIGHTNESS,
    };
    ESP_RETURN_ON_ERROR(esp_isp_color_configure(s_cam.isp, &color_config), TAG, "isp color");
    ESP_RETURN_ON_ERROR(esp_isp_color_enable(s_cam.isp), TAG, "isp color en");
    ESP_RETURN_ON_ERROR(esp_isp_enable(s_cam.isp), TAG, "isp en");
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_start(s_cam.cam), TAG, "csi start");

    while (xSemaphoreTake(s_cam.frame_done, 0) == pdTRUE) {
    }

    s_cam.pipeline_ready = true;
    s_cam.warmup_start_us = esp_timer_get_time();
    s_cam.warmup_done = false;
    rpi_camera_control_set_state(RPI_STATE_WARMUP);
    ESP_LOGI(TAG, "camera pipeline ready — warmup %d s", RPI_CAM_WARMUP_SEC);
    return ESP_OK;
}

static void camera_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(rpi_camera_health_register_current_task());
    esp_err_t ret;
    uint32_t shot_num = 0;
    const int64_t warmup_us = (int64_t)RPI_CAM_WARMUP_SEC * 1000000;
    const int64_t snapshot_interval_us = (int64_t)RPI_SNAPSHOT_INTERVAL_SEC * 1000000;
    int64_t last_shot_us = 0;

    for (;;) {
        if (rpi_camera_control_take_restart()) {
            esp_restart();
        }

        if (rpi_camera_control_get_state() == RPI_STATE_OTA) {
            rpi_camera_health_feed();
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!s_cam.pipeline_ready) {
            while (rpi_camera_control_wait_for_start(1000) != ESP_OK) {
                rpi_camera_health_feed();
                if (rpi_camera_control_take_restart()) {
                    esp_restart();
                }
            }
            if (camera_pipeline_init() != ESP_OK) {
                ESP_LOGE(TAG, "camera init failed");
                rpi_camera_control_set_state(RPI_STATE_FAULT);
                vTaskDelay(pdMS_TO_TICKS(2000));
                esp_restart();
            }
        }

        rpi_camera_health_feed();

        ret = esp_cam_ctlr_receive(s_cam.cam, &s_cam.trans, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            if (ret != ESP_ERR_TIMEOUT) {
                ESP_LOGE(TAG, "receive: %s", esp_err_to_name(ret));
                rpi_camera_health_on_cam_error();
            }
            continue;
        }

        if (xSemaphoreTake(s_cam.frame_done, pdMS_TO_TICKS(3000)) != pdTRUE) {
            ESP_LOGW(TAG, "frame timeout");
            rpi_camera_health_on_frame_timeout();
            continue;
        }

        rpi_camera_health_on_frame_ok();

        esp_cache_msync(s_cam.frame_buffer, s_cam.frame_buffer_size,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);

        rpi_camera_tcp_maintain_connection();
        if (rpi_camera_control_capture_enabled()) {
            rpi_camera_tcp_process_spool();
        }

        if (!s_cam.warmup_done) {
            if ((esp_timer_get_time() - s_cam.warmup_start_us) >= warmup_us) {
                s_cam.warmup_done = true;
                rpi_camera_control_set_state(RPI_STATE_RUNNING);
                ESP_LOGI(TAG, "warmup done — capture every %d s when RUNNING", RPI_SNAPSHOT_INTERVAL_SEC);
            }
            continue;
        }

        if (!rpi_camera_control_capture_enabled()) {
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        if (last_shot_us == 0 || (now_us - last_shot_us) >= snapshot_interval_us) {
            int16_t temp_centi = (int16_t)(1500 + (esp_random() % 2500));
            last_shot_us = now_us;
            esp_err_t send_ret = rpi_camera_tcp_send_snapshot(s_cam.frame_buffer, RPI_CAM_FRAME_BYTES,
                                                              RPI_NODE_ID, temp_centi);
            if (send_ret == ESP_OK) {
                shot_num++;
                ESP_LOGI(TAG, "packet #%" PRIu32 " sent (node=%" PRIu32 ", temp=%.2fC)",
                         shot_num, RPI_NODE_ID, temp_centi / 100.0f);
            } else if (send_ret == ESP_ERR_NOT_FINISHED) {
                ESP_LOGW(TAG, "packet spooled (pending=%" PRIu32 ")", rpi_camera_tcp_get_spool_pending());
            } else {
                ESP_LOGE(TAG, "packet lost (failures=%" PRIu32 ")", rpi_camera_tcp_get_send_failures());
            }
        }
    }
}

void app_main(void)
{
    printf("\n");
    printf("========================================\n");
    printf(" Raspberry Pi Camera — SLAVE node\n");
    printf(" Control: master -> TCP :%d\n", RPI_CTRL_TCP_PORT);
    printf(" OTA:       TCP :%d (after first USB flash)\n", RPI_OTA_TCP_PORT);
    printf(" Telemetry: -> %s:%d every %d s\n", RPI_TCP_SERVER_IP, RPI_TCP_SERVER_PORT, RPI_SNAPSHOT_INTERVAL_SEC);
    printf(" Camera: %dx%d\n", RPI_CAM_HRES, RPI_CAM_VRES);
    printf("========================================\n\n");

    ESP_ERROR_CHECK(rpi_camera_control_init());
    rpi_camera_control_set_state(RPI_STATE_NET_INIT);

    ESP_ERROR_CHECK(rpi_camera_net_init());
    ESP_LOGI(TAG, "waiting for IP...");
    if (rpi_camera_net_wait_for_ip(120000) != ESP_OK) {
        ESP_LOGE(TAG, "no IP");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    ESP_ERROR_CHECK(rpi_camera_tcp_init());
    ESP_ERROR_CHECK(rpi_camera_health_init());
    ESP_ERROR_CHECK(rpi_camera_health_register_current_task());
    ESP_ERROR_CHECK(rpi_camera_ctrl_server_start());
    ESP_ERROR_CHECK(rpi_camera_ota_start());

    rpi_camera_control_set_state(RPI_STATE_IDLE);
    ESP_LOGI(TAG, "slave IDLE on %s — send START from master (tcp_master.py)", rpi_camera_net_get_ip_str());

    memset(&s_cam, 0, sizeof(s_cam));
    xTaskCreate(camera_task, "camera", 12288, NULL, 4, NULL);

    while (true) {
        rpi_camera_health_feed();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
