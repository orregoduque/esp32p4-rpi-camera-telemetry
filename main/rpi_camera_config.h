/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pin and format settings for the Raspberry Pi camera (OV5647) on the
 * Waveshare ESP32-P4-Module-DEV-KIT MIPI-CSI connector.
 */

#pragma once

#include "sdkconfig.h"

#define RPI_CAM_LDO_CHAN_ID              3
#define RPI_CAM_LDO_VOLTAGE_MV           2500

#define RPI_CAM_SCCB_SCL_IO              8
#define RPI_CAM_SCCB_SDA_IO              7

#define RPI_CAM_HRES                     800
#define RPI_CAM_VRES                     640
#define RPI_CAM_MIPI_LANE_BITRATE_MBPS   200
#define RPI_CAM_FORMAT_NAME              "MIPI_2lane_24Minput_RAW8_800x640_50fps"

#define RPI_CAM_RGB565_BPP               16
#define RPI_CAM_FRAME_BYTES              (RPI_CAM_HRES * RPI_CAM_VRES * RPI_CAM_RGB565_BPP / 8)

/** How often a new photo is captured and sent over TCP */
#define RPI_SNAPSHOT_INTERVAL_SEC        10

/** Let the sensor/ISP settle before the first snapshot (AE needs a few frames) */
#define RPI_CAM_WARMUP_SEC               8

/** ISP brightness boost (-128..127, 0 = neutral) */
#define RPI_ISP_BRIGHTNESS               80

/** Direct-cable Ethernet (disable static IP if you use DHCP on a switch) */
#ifndef CONFIG_RPI_ETH_USE_STATIC_IP
#define RPI_ETH_USE_STATIC_IP            1
#else
#define RPI_ETH_USE_STATIC_IP            CONFIG_RPI_ETH_USE_STATIC_IP
#endif
#define RPI_ETH_STATIC_IP                "192.168.1.20"
#define RPI_ETH_STATIC_GW                "192.168.1.10"
#define RPI_ETH_STATIC_NETMASK           "255.255.255.0"

#ifdef CONFIG_RPI_ETH_STATIC_IP
#undef RPI_ETH_STATIC_IP
#define RPI_ETH_STATIC_IP                CONFIG_RPI_ETH_STATIC_IP
#endif
#ifdef CONFIG_RPI_ETH_STATIC_GW
#undef RPI_ETH_STATIC_GW
#define RPI_ETH_STATIC_GW                CONFIG_RPI_ETH_STATIC_GW
#endif
#ifdef CONFIG_RPI_ETH_STATIC_NETMASK
#undef RPI_ETH_STATIC_NETMASK
#define RPI_ETH_STATIC_NETMASK           CONFIG_RPI_ETH_STATIC_NETMASK
#endif

/** Node metadata and TCP uplink settings */
#define RPI_NODE_ID                      1U
#define RPI_TCP_SERVER_IP                "192.168.1.10"
#define RPI_TCP_SERVER_PORT              9000
#define RPI_TCP_CONNECT_TIMEOUT_MS       3000
#define RPI_TCP_CONNECT_RETRY_SEC        5
#define RPI_TCP_SEND_TIMEOUT_MS          8000
#define RPI_TCP_RECV_TIMEOUT_MS          4000
#define RPI_TCP_MAX_RETRIES              3

/** Master control plane (slave listens, PC master connects) */
#define RPI_CTRL_TCP_PORT                9001
#define RPI_CTRL_HEARTBEAT_SEC           3

#ifdef CONFIG_RPI_CTRL_TCP_PORT
#undef RPI_CTRL_TCP_PORT
#define RPI_CTRL_TCP_PORT                CONFIG_RPI_CTRL_TCP_PORT
#endif

/** Local spool (FAT on flash) when TCP upload fails */
#define RPI_SPOOL_MOUNT_POINT            "/spool"
#define RPI_SPOOL_MAX_FILES              24

/** Watchdog / auto-recovery thresholds */
#define RPI_HEALTH_WDT_TIMEOUT_SEC       60
#define RPI_HEALTH_MAX_FRAME_TIMEOUTS    120
#define RPI_HEALTH_MAX_CAM_ERRORS        30

/**
 * Pre-shared key used for HMAC-SHA256 integrity/authentication.
 * Change this to a long random secret in production.
 */
#define RPI_TCP_PSK                      "CHANGE_ME_TO_RANDOM_32PLUS_BYTES"

#ifdef CONFIG_RPI_NODE_ID
#undef RPI_NODE_ID
#define RPI_NODE_ID CONFIG_RPI_NODE_ID
#endif

#ifdef CONFIG_RPI_TCP_SERVER_IP
#undef RPI_TCP_SERVER_IP
#define RPI_TCP_SERVER_IP CONFIG_RPI_TCP_SERVER_IP
#endif

#ifdef CONFIG_RPI_TCP_SERVER_PORT
#undef RPI_TCP_SERVER_PORT
#define RPI_TCP_SERVER_PORT CONFIG_RPI_TCP_SERVER_PORT
#endif
