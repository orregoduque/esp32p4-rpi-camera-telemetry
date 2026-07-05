/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <string.h>

#include <arpa/inet.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "rpi_camera_config.h"
#include "rpi_camera_control.h"
#include "rpi_camera_health.h"
#include "rpi_camera_net.h"
#include "rpi_camera_tcp.h"

static const char *TAG = "rpi_ctrl";

#define CTRL_START_BIT    BIT0

static SemaphoreHandle_t s_state_lock;
static EventGroupHandle_t s_start_events;
static rpi_slave_state_t s_state;
static bool s_restart_pending;

static const char *state_name(rpi_slave_state_t state)
{
    switch (state) {
    case RPI_STATE_BOOT:          return "BOOT";
    case RPI_STATE_NET_INIT:      return "NET_INIT";
    case RPI_STATE_IDLE:          return "IDLE";
    case RPI_STATE_CAM_INIT:      return "CAM_INIT";
    case RPI_STATE_WARMUP:        return "WARMUP";
    case RPI_STATE_RUNNING:       return "RUNNING";
    case RPI_STATE_PAUSED:        return "PAUSED";
    case RPI_STATE_SPOOL_BACKLOG: return "SPOOL_BACKLOG";
    case RPI_STATE_FAULT:         return "FAULT";
    case RPI_STATE_RESTARTING:    return "RESTARTING";
    case RPI_STATE_OTA:           return "OTA";
    default:                      return "UNKNOWN";
    }
}

esp_err_t rpi_camera_control_init(void)
{
    s_state_lock = xSemaphoreCreateMutex();
    s_start_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_state_lock && s_start_events, ESP_ERR_NO_MEM, TAG, "sync alloc failed");

    s_state = RPI_STATE_BOOT;
    s_restart_pending = false;
    return ESP_OK;
}

rpi_slave_state_t rpi_camera_control_get_state(void)
{
    rpi_slave_state_t state = RPI_STATE_BOOT;
    if (xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
        state = s_state;
        xSemaphoreGive(s_state_lock);
    }
    return state;
}

void rpi_camera_control_set_state(rpi_slave_state_t state)
{
    if (xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
        if (s_state != state) {
            ESP_LOGI(TAG, "state %s -> %s", state_name(s_state), state_name(state));
            s_state = state;
        }
        xSemaphoreGive(s_state_lock);
    }
}

bool rpi_camera_control_capture_enabled(void)
{
    rpi_slave_state_t state = rpi_camera_control_get_state();
    return state == RPI_STATE_WARMUP || state == RPI_STATE_RUNNING || state == RPI_STATE_SPOOL_BACKLOG;
}

bool rpi_camera_control_take_restart(void)
{
    bool pending = false;
    if (xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
        pending = s_restart_pending;
        s_restart_pending = false;
        xSemaphoreGive(s_state_lock);
    }
    return pending;
}

esp_err_t rpi_camera_control_wait_for_start(int timeout_ms)
{
    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_start_events, CTRL_START_BIT, pdTRUE, pdFALSE, ticks);
    return (bits & CTRL_START_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void rpi_camera_control_signal_start(void)
{
    xEventGroupSetBits(s_start_events, CTRL_START_BIT);
}

static uint64_t htonll(uint64_t v)
{
    uint32_t hi = htonl((uint32_t)(v >> 32));
    uint32_t lo = htonl((uint32_t)(v & 0xFFFFFFFFULL));
    return ((uint64_t)lo << 32) | hi;
}

void rpi_camera_control_fill_state_msg(rpi_ctrl_state_msg_t *msg, uint32_t cmd_id)
{
    memset(msg, 0, sizeof(*msg));
    msg->magic = htonl(RPI_CTRL_MAGIC);
    msg->version = htons(RPI_CTRL_VERSION);
    msg->msg_type = htons(RPI_CTRL_MSG_STATE);
    msg->cmd_id = htonl(cmd_id);
    msg->state = htons((uint16_t)rpi_camera_control_get_state());
    msg->link_up = rpi_camera_net_is_ready() ? 1 : 0;
    msg->tcp_connected = rpi_camera_tcp_is_connected() ? 1 : 0;
    msg->boot_count = htonl(rpi_camera_health_get_boot_count());
    msg->sequence = htonl(rpi_camera_tcp_get_sequence());
    msg->spool_pending = htonl(rpi_camera_tcp_get_spool_pending());
    msg->send_failures = htonl(rpi_camera_tcp_get_send_failures());
    msg->uptime_ms = htonll((uint64_t)(esp_timer_get_time() / 1000));
}

uint32_t rpi_camera_control_handle_command(uint32_t cmd_id, uint16_t command, rpi_ctrl_ack_t *ack_out)
{
    uint32_t status = RPI_CTRL_STATUS_OK;
    rpi_slave_state_t state = rpi_camera_control_get_state();

    memset(ack_out, 0, sizeof(*ack_out));
    ack_out->magic = htonl(RPI_CTRL_MAGIC);
    ack_out->version = htons(RPI_CTRL_VERSION);
    ack_out->msg_type = htons(RPI_CTRL_MSG_ACK);
    ack_out->cmd_id = htonl(cmd_id);

    switch (command) {
    case RPI_CTRL_CMD_QUERY_STATE:
        break;

    case RPI_CTRL_CMD_START:
        if (state == RPI_STATE_IDLE) {
            rpi_camera_control_signal_start();
            rpi_camera_control_set_state(RPI_STATE_CAM_INIT);
        } else if (state == RPI_STATE_PAUSED) {
            rpi_camera_control_set_state(RPI_STATE_RUNNING);
        } else if (state == RPI_STATE_WARMUP || state == RPI_STATE_RUNNING || state == RPI_STATE_SPOOL_BACKLOG) {
            /* already active */
        } else if (state == RPI_STATE_CAM_INIT) {
            /* camera task already starting */
        } else {
            status = RPI_CTRL_STATUS_INVALID_STATE;
        }
        break;

    case RPI_CTRL_CMD_STOP:
        if (state == RPI_STATE_OTA) {
            rpi_camera_control_set_state(RPI_STATE_PAUSED);
        } else if (state == RPI_STATE_RUNNING || state == RPI_STATE_WARMUP || state == RPI_STATE_SPOOL_BACKLOG
                || state == RPI_STATE_CAM_INIT) {
            rpi_camera_control_set_state(RPI_STATE_PAUSED);
        } else if (state != RPI_STATE_PAUSED && state != RPI_STATE_IDLE) {
            status = RPI_CTRL_STATUS_INVALID_STATE;
        }
        break;

    case RPI_CTRL_CMD_RESTART:
        rpi_camera_control_set_state(RPI_STATE_RESTARTING);
        if (xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
            s_restart_pending = true;
            xSemaphoreGive(s_state_lock);
        }
        break;

    case RPI_CTRL_CMD_OTA:
        if (state == RPI_STATE_NET_INIT || state == RPI_STATE_FAULT || state == RPI_STATE_RESTARTING) {
            status = RPI_CTRL_STATUS_INVALID_STATE;
        } else {
            rpi_camera_control_set_state(RPI_STATE_OTA);
        }
        break;

    default:
        status = RPI_CTRL_STATUS_BAD_CMD;
        break;
    }

    ack_out->status = htonl(status);
    ack_out->state = htons((uint16_t)rpi_camera_control_get_state());
    return status;
}
