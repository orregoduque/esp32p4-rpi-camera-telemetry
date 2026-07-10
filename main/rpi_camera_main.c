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
#include "esp_check.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_private/esp_cache_private.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/isp.h"
#include "driver/isp_color.h"
#include "rpi_sensor.h"
#include "rpi_camera_config.h"
#include "rpi_camera_control.h"
#include "rpi_camera_ctrl_server.h"
#include "rpi_camera_health.h"
#include "rpi_camera_net.h"
#include "rpi_camera_ota.h"
#include "rpi_camera_tcp.h"
#include "rpi_camera_thermal.h"

static const char *TAG = "rpi_camera";

#define RPI_CAM_FRAME_BUF_CAPS    (MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA)

typedef enum {
    CAPTURE_SLOT_VISIBLE,
    CAPTURE_SLOT_THERMAL,
} capture_slot_t;

typedef struct {
    esp_ldo_channel_handle_t ldo;
    esp_cam_ctlr_handle_t cam;
    isp_proc_handle_t isp;
    rpi_sensor_handle_t sensor;
    void *frame_buffer;
    void *thermal_rgb;
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

static bool IRAM_ATTR camera_get_new_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    (void)handle;
    esp_cam_ctlr_trans_t *configured = (esp_cam_ctlr_trans_t *)user_data;
    trans->buffer = configured->buffer;
    trans->buflen = configured->buflen;
    return false;
}

static bool IRAM_ATTR camera_trans_finished(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    (void)handle;
    (void)trans;
    (void)user_data;
    BaseType_t woken = pdFALSE;
    if (s_cam.frame_done) {
        xSemaphoreGiveFromISR(s_cam.frame_done, &woken);
    }
    return woken == pdTRUE;
}

static esp_err_t camera_restart_streaming(void)
{
    esp_err_t err = esp_cam_ctlr_stop(s_cam.cam);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "csi stop (recover): %s", esp_err_to_name(err));
    }
    err = rpi_sensor_restart_stream();
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(RPI_THERMAL_BUS_SETTLE_MS));
    while (xSemaphoreTake(s_cam.frame_done, 0) == pdTRUE) {
    }
    err = esp_cam_ctlr_start(s_cam.cam);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

static bool capture_interval_elapsed(int64_t last_shot_us)
{
    if (last_shot_us == 0) {
        return true;
    }
    int64_t elapsed_us = esp_timer_get_time() - last_shot_us;
    return elapsed_us >= ((int64_t)RPI_SNAPSHOT_INTERVAL_SEC * 1000000);
}

static esp_err_t camera_pause_for_thermal(void)
{
    esp_err_t err = esp_cam_ctlr_stop(s_cam.cam);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "csi stop: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(RPI_THERMAL_BUS_SETTLE_MS));
    return rpi_sensor_stop_stream();
}

static esp_err_t camera_resume_after_thermal(void)
{
    esp_err_t err = rpi_sensor_restart_stream();
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(RPI_THERMAL_BUS_SETTLE_MS));
    while (xSemaphoreTake(s_cam.frame_done, 0) == pdTRUE) {
    }
    err = esp_cam_ctlr_start(s_cam.cam);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "csi start: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static esp_err_t camera_ensure_thermal_ready(void)
{
    if (s_cam.thermal_rgb == NULL) {
        s_cam.thermal_rgb = heap_caps_malloc(RPI_THERMAL_RGB565_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_cam.thermal_rgb) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!rpi_camera_thermal_is_ready()) {
        ESP_RETURN_ON_ERROR(rpi_camera_thermal_init(s_cam.sensor.i2c_bus_handle), TAG, "thermal init");
    }
    return ESP_OK;
}

static esp_err_t camera_run_thermal_slot(uint32_t *shot_num, int64_t *last_shot_us)
{
    float t_min = 0.0f;
    float t_max = 0.0f;
    float t_spot = 0.0f;
    int16_t temp_centi = 0;
    bool thermal_frame_ok = false;

    ESP_LOGI(TAG, "thermal slot — pausing visible camera");
    rpi_camera_health_feed();
    if (camera_pause_for_thermal() == ESP_OK && camera_ensure_thermal_ready() == ESP_OK) {
        thermal_frame_ok = rpi_camera_thermal_render_jpeg_rgb565(s_cam.thermal_rgb,
                                                                   RPI_THERMAL_RGB565_BYTES,
                                                                   &t_min, &t_max, &t_spot) == ESP_OK;
    } else {
        ESP_LOGW(TAG, "thermal slot skipped (pause/init failed)");
    }

    if (thermal_frame_ok) {
        temp_centi = (int16_t)(t_spot * 100.0f);
        rpi_camera_health_feed();
        esp_err_t th_ret = rpi_camera_tcp_send_thermal(s_cam.thermal_rgb, RPI_THERMAL_RGB565_BYTES,
                                                       RPI_NODE_ID, temp_centi);
        if (th_ret == ESP_OK) {
            (*shot_num)++;
            ESP_LOGI(TAG, "thermal #%" PRIu32 " sent (spot=%.1fC range=%.1f..%.1fC)",
                     *shot_num, t_spot, t_min, t_max);
        } else if (th_ret == ESP_ERR_NOT_FINISHED) {
            ESP_LOGW(TAG, "thermal spooled (pending=%" PRIu32 ")", rpi_camera_tcp_get_spool_pending());
            (*shot_num)++;
        } else {
            ESP_LOGE(TAG, "thermal lost (failures=%" PRIu32 ")", rpi_camera_tcp_get_send_failures());
        }
    } else {
        ESP_LOGW(TAG, "thermal capture failed on slot #%" PRIu32, *shot_num + 1);
        (*shot_num)++;
    }

    rpi_camera_health_feed();
    if (camera_resume_after_thermal() != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(500));
        rpi_camera_health_feed();
        if (camera_restart_streaming() != ESP_OK) {
            ESP_LOGE(TAG, "camera resume after thermal failed — rebooting");
            esp_restart();
        }
    }

    *last_shot_us = esp_timer_get_time();
    return ESP_OK;
}

static esp_err_t camera_pipeline_init(void)
{
    esp_ldo_channel_config_t ldo_config = {
        .chan_id = RPI_CAM_LDO_CHAN_ID,
        .voltage_mv = RPI_CAM_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, &s_cam.ldo), TAG, "ldo");

    rpi_sensor_config_t sensor_config = {
        .i2c_port_num = I2C_NUM_0,
        .i2c_sda_io_num = RPI_CAM_SCCB_SDA_IO,
        .i2c_scl_io_num = RPI_CAM_SCCB_SCL_IO,
        .port = ESP_CAM_SENSOR_MIPI_CSI,
        .format_name = RPI_CAM_FORMAT_NAME,
    };
    rpi_sensor_handle_t rpi_sensor = {0};
    rpi_sensor_init(&sensor_config, &rpi_sensor);
    s_cam.sensor.i2c_bus_handle = rpi_sensor.i2c_bus_handle;
    s_cam.sensor.sccb_handle = rpi_sensor.sccb_handle;
    ESP_RETURN_ON_FALSE(s_cam.sensor.i2c_bus_handle, ESP_FAIL, TAG, "sensor init");

    esp_cam_ctlr_csi_config_t csi_config = {
        .ctlr_id = 0,
        .h_res = RPI_CAM_HRES,
        .v_res = RPI_CAM_VRES,
        .lane_bit_rate_mbps = RPI_CAM_MIPI_LANE_BITRATE_MBPS,
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RAW8,
        .data_lane_num = 2,
        .byte_swap_en = false,
        .queue_items = 1,
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
        .on_get_new_trans = camera_get_new_trans,
        .on_trans_finished = camera_trans_finished,
    };
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_register_event_callbacks(s_cam.cam, &cbs, &s_cam.trans), TAG, "csi cbs");
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
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_receive(s_cam.cam, &s_cam.trans, ESP_CAM_CTLR_MAX_DELAY), TAG, "csi receive");

    s_cam.pipeline_ready = true;
    s_cam.warmup_start_us = esp_timer_get_time();
    s_cam.warmup_done = false;
    rpi_camera_control_set_state(RPI_STATE_WARMUP);
    ESP_LOGI(TAG, "camera pipeline ready (visible only) — warmup %d s", RPI_CAM_WARMUP_SEC);
    return ESP_OK;
}

static void camera_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(rpi_camera_health_register_current_task());
    uint32_t shot_num = 0;
    const int64_t warmup_us = (int64_t)RPI_CAM_WARMUP_SEC * 1000000;
    int64_t last_shot_us = 0;
    int timeout_streak = 0;
    capture_slot_t next_slot = CAPTURE_SLOT_VISIBLE;

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

        if (s_cam.warmup_done && rpi_camera_control_capture_enabled() && capture_interval_elapsed(last_shot_us)
            && next_slot == CAPTURE_SLOT_THERMAL) {
            camera_run_thermal_slot(&shot_num, &last_shot_us);
            next_slot = CAPTURE_SLOT_VISIBLE;
            ESP_LOGI(TAG, "next slot: visible in %d s", RPI_SNAPSHOT_INTERVAL_SEC);
            continue;
        }

        bool frame_ready = false;
        int64_t frame_deadline_us = esp_timer_get_time() + 5000000;
        while (esp_timer_get_time() < frame_deadline_us) {
            if (xSemaphoreTake(s_cam.frame_done, pdMS_TO_TICKS(500)) == pdTRUE) {
                frame_ready = true;
                break;
            }
            rpi_camera_health_feed();
        }
        if (!frame_ready) {
            ESP_LOGW(TAG, "frame timeout");
            rpi_camera_health_on_frame_timeout();
            timeout_streak++;
            if (timeout_streak >= 3) {
                ESP_LOGW(TAG, "recovering CSI after %d timeouts", timeout_streak);
                if (camera_restart_streaming() != ESP_OK) {
                    ESP_LOGE(TAG, "CSI recovery failed — rebooting");
                    esp_restart();
                }
                timeout_streak = 0;
            }
            continue;
        }
        timeout_streak = 0;

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
                ESP_LOGI(TAG, "warmup done — visible/thermal alternate every %d s", RPI_SNAPSHOT_INTERVAL_SEC);
            }
            continue;
        }

        if (!rpi_camera_control_capture_enabled()) {
            continue;
        }

        if (capture_interval_elapsed(last_shot_us) && next_slot == CAPTURE_SLOT_VISIBLE) {
            last_shot_us = esp_timer_get_time();
            int16_t temp_centi = 0;
            rpi_camera_thermal_get_last_spot_c(&temp_centi);

            rpi_camera_health_feed();
            esp_err_t send_ret = rpi_camera_tcp_send_snapshot(s_cam.frame_buffer, RPI_CAM_FRAME_BYTES,
                                                              RPI_NODE_ID, temp_centi);
            if (send_ret == ESP_OK) {
                shot_num++;
                ESP_LOGI(TAG, "visible #%" PRIu32 " sent (node=%" PRIu32 ", temp=%.2fC)",
                         shot_num, RPI_NODE_ID, temp_centi / 100.0f);
            } else if (send_ret == ESP_ERR_NOT_FINISHED) {
                ESP_LOGW(TAG, "visible spooled (pending=%" PRIu32 ")", rpi_camera_tcp_get_spool_pending());
                shot_num++;
            } else {
                ESP_LOGE(TAG, "visible lost (failures=%" PRIu32 ")", rpi_camera_tcp_get_send_failures());
            }
            next_slot = CAPTURE_SLOT_THERMAL;
            ESP_LOGI(TAG, "next slot: thermal in %d s", RPI_SNAPSHOT_INTERVAL_SEC);
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
    printf(" Telemetry: visible + thermal alternate every %d s\n", RPI_SNAPSHOT_INTERVAL_SEC);
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
