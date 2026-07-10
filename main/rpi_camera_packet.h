/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#define RPI_PKT_MAGIC            0x5250434DU  /* "RPCM" */
#define RPI_PKT_VERSION          1U
#define RPI_PKT_HMAC_SIZE        32U

#define RPI_PKT_PAYLOAD_VISIBLE  0U
#define RPI_PKT_PAYLOAD_THERMAL  1U

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t node_id;
    uint32_t sequence;
    uint64_t timestamp_ms;
    int16_t temperature_centi_c;
    uint16_t reserved;
    uint32_t jpeg_size;
    uint32_t jpeg_crc32;
    uint8_t hmac_sha256[RPI_PKT_HMAC_SIZE];
} rpi_node_packet_header_t;

typedef struct __attribute__((packed)) {
    rpi_node_packet_header_t header;
    uint8_t jpeg_payload[];
} rpi_node_packet_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t sequence;
    uint32_t status;
} rpi_node_packet_ack_t;
