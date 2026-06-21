/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * TCP control server — slave listens for master commands on port 9001.
 */

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rpi_camera_config.h"
#include "rpi_camera_control.h"
#include "rpi_camera_control_packet.h"
#include "rpi_camera_ctrl_server.h"

static const char *TAG = "rpi_ctrl_srv";

static int s_listen_sock = -1;

static ssize_t recv_exact(int sock, uint8_t *data, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(sock, data + got, len - got, 0);
        if (n <= 0) {
            return n;
        }
        got += (size_t)n;
    }
    return (ssize_t)got;
}

static esp_err_t send_all(int sock, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, p + sent, len - sent, 0);
        if (n <= 0) {
            return ESP_FAIL;
        }
        sent += (size_t)n;
    }
    return ESP_OK;
}

static void handle_client(int client_sock)
{
    struct timeval to = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));

    ESP_LOGI(TAG, "master connected");

    int64_t last_hb_us = 0;
    const int64_t hb_us = (int64_t)RPI_CTRL_HEARTBEAT_SEC * 1000000;

    while (true) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(client_sock, &rfds);
        struct timeval sel_to = { .tv_sec = 1, .tv_usec = 0 };
        int rc = select(client_sock + 1, &rfds, NULL, NULL, &sel_to);
        if (rc < 0) {
            break;
        }

        if (rc > 0 && FD_ISSET(client_sock, &rfds)) {
            rpi_ctrl_cmd_t cmd = {0};
            if (recv_exact(client_sock, (uint8_t *)&cmd, sizeof(cmd)) != sizeof(cmd)) {
                break;
            }

            if (ntohl(cmd.magic) != RPI_CTRL_MAGIC) {
                rpi_ctrl_ack_t ack = {
                    .magic = htonl(RPI_CTRL_MAGIC),
                    .version = htons(RPI_CTRL_VERSION),
                    .msg_type = htons(RPI_CTRL_MSG_ACK),
                    .cmd_id = cmd.cmd_id,
                    .status = htonl(RPI_CTRL_STATUS_BAD_MAGIC),
                    .state = htons((uint16_t)rpi_camera_control_get_state()),
                };
                send_all(client_sock, &ack, sizeof(ack));
                continue;
            }
            if (ntohs(cmd.version) != RPI_CTRL_VERSION) {
                rpi_ctrl_ack_t ack = {
                    .magic = htonl(RPI_CTRL_MAGIC),
                    .version = htons(RPI_CTRL_VERSION),
                    .msg_type = htons(RPI_CTRL_MSG_ACK),
                    .cmd_id = cmd.cmd_id,
                    .status = htonl(RPI_CTRL_STATUS_BAD_VERSION),
                    .state = htons((uint16_t)rpi_camera_control_get_state()),
                };
                send_all(client_sock, &ack, sizeof(ack));
                continue;
            }
            if (ntohs(cmd.msg_type) != RPI_CTRL_MSG_CMD) {
                continue;
            }
            if (ntohl(cmd.payload_len) != 0) {
                continue;
            }

            uint32_t cmd_id = ntohl(cmd.cmd_id);
            uint16_t command = ntohs(cmd.command);

            rpi_ctrl_ack_t ack = {0};
            uint32_t status = rpi_camera_control_handle_command(cmd_id, command, &ack);
            send_all(client_sock, &ack, sizeof(ack));

            rpi_ctrl_state_msg_t state_msg = {0};
            rpi_camera_control_fill_state_msg(&state_msg, cmd_id);
            send_all(client_sock, &state_msg, sizeof(state_msg));

            if (command == RPI_CTRL_CMD_RESTART && status == RPI_CTRL_STATUS_OK) {
                ESP_LOGW(TAG, "restart commanded by master");
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        }

        int64_t now_us = esp_timer_get_time();
        if (last_hb_us == 0 || (now_us - last_hb_us) >= hb_us) {
            rpi_ctrl_state_msg_t hb = {0};
            rpi_camera_control_fill_state_msg(&hb, 0);
            if (send_all(client_sock, &hb, sizeof(hb)) != ESP_OK) {
                break;
            }
            last_hb_us = now_us;
        }
    }

    close(client_sock);
    ESP_LOGI(TAG, "master disconnected");
}

static void ctrl_server_task(void *arg)
{
    (void)arg;

    while (true) {
        struct sockaddr_in peer = {0};
        socklen_t peer_len = sizeof(peer);
        int client = accept(s_listen_sock, (struct sockaddr *)&peer, &peer_len);
        if (client < 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        handle_client(client);
    }
}

esp_err_t rpi_camera_ctrl_server_start(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    ESP_RETURN_ON_FALSE(sock >= 0, ESP_FAIL, TAG, "socket failed");

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)RPI_CTRL_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    ESP_RETURN_ON_ERROR(bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0 ? ESP_OK : ESP_FAIL,
                        TAG, "bind :%d failed", RPI_CTRL_TCP_PORT);
    ESP_RETURN_ON_ERROR(listen(sock, 2) == 0 ? ESP_OK : ESP_FAIL, TAG, "listen failed");

    s_listen_sock = sock;
    ESP_LOGI(TAG, "listening on TCP port %d (master control)", RPI_CTRL_TCP_PORT);

    BaseType_t ok = xTaskCreate(ctrl_server_task, "ctrl_srv", 6144, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");
    return ESP_OK;
}
