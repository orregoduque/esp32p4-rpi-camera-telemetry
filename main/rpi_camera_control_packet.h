/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Master/slave control plane (RPCC) on TCP port 9001.
 */

#pragma once

#include <stdint.h>

#define RPI_CTRL_MAGIC           0x52504343U  /* "RPCC" */
#define RPI_CTRL_VERSION         1U

#define RPI_CTRL_MSG_CMD         1U
#define RPI_CTRL_MSG_ACK         2U
#define RPI_CTRL_MSG_STATE       3U

#define RPI_CTRL_CMD_QUERY_STATE 1U
#define RPI_CTRL_CMD_START       2U
#define RPI_CTRL_CMD_STOP        3U
#define RPI_CTRL_CMD_RESTART     4U
#define RPI_CTRL_CMD_OTA         5U

#define RPI_CTRL_STATUS_OK       0U
#define RPI_CTRL_STATUS_BAD_MAGIC    1U
#define RPI_CTRL_STATUS_BAD_VERSION  2U
#define RPI_CTRL_STATUS_BAD_CMD      3U
#define RPI_CTRL_STATUS_INVALID_STATE 4U
#define RPI_CTRL_STATUS_INTERNAL     5U

typedef enum {
    RPI_STATE_BOOT = 0,
    RPI_STATE_NET_INIT,
    RPI_STATE_IDLE,
    RPI_STATE_CAM_INIT,
    RPI_STATE_WARMUP,
    RPI_STATE_RUNNING,
    RPI_STATE_PAUSED,
    RPI_STATE_SPOOL_BACKLOG,
    RPI_STATE_FAULT,
    RPI_STATE_RESTARTING,
    RPI_STATE_OTA,
} rpi_slave_state_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t msg_type;
    uint32_t cmd_id;
    uint16_t command;
    uint16_t reserved;
    uint32_t payload_len;
} rpi_ctrl_cmd_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t msg_type;
    uint32_t cmd_id;
    uint32_t status;
    uint16_t state;
    uint16_t reserved;
} rpi_ctrl_ack_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t msg_type;
    uint32_t cmd_id;
    uint16_t state;
    uint8_t link_up;
    uint8_t tcp_connected;
    uint16_t reserved;
    uint32_t boot_count;
    uint32_t sequence;
    uint32_t spool_pending;
    uint32_t send_failures;
    uint64_t uptime_ms;
} rpi_ctrl_state_msg_t;

#define RPI_CTRL_CMD_SIZE    ((size_t)sizeof(rpi_ctrl_cmd_t))
#define RPI_CTRL_ACK_SIZE    ((size_t)sizeof(rpi_ctrl_ack_t))
#define RPI_CTRL_STATE_SIZE  ((size_t)sizeof(rpi_ctrl_state_msg_t))
