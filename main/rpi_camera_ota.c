/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rpi_camera_ota.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp32p4/rom/sha.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "rpi_camera_config.h"
#include "rpi_camera_health.h"

static const char *TAG = "rpi_ota";

#define RPI_OTA_MAGIC           0x45544F41u  /* "ETOA" */
#define RPI_OTA_PROTOCOL_VER    1u
#define RPI_OTA_TASK_STACK      8192
#define RPI_OTA_CHUNK_SIZE      4096

typedef struct __attribute__((packed)) {
    uint32_t magic_be;
    uint16_t version_be;
    uint16_t header_len_be;
    uint32_t image_size_be;
    uint8_t sha256[32];
} rpi_ota_header_t;

static bool read_exact(int sock, void *buffer, size_t len)
{
    uint8_t *out = (uint8_t *)buffer;
    size_t done = 0;
    while (done < len) {
        ssize_t ret = recv(sock, out + done, len - done, 0);
        if (ret <= 0) {
            return false;
        }
        done += (size_t)ret;
    }
    return true;
}

static void send_status(int sock, const char *status)
{
    (void)send(sock, status, strlen(status), 0);
}

static void format_ipv4_addr(uint32_t addr_be, char *out, size_t out_len)
{
    const uint32_t addr = ntohl(addr_be);
    snprintf(out, out_len, "%lu.%lu.%lu.%lu",
             (unsigned long)((addr >> 24) & 0xff),
             (unsigned long)((addr >> 16) & 0xff),
             (unsigned long)((addr >> 8) & 0xff),
             (unsigned long)(addr & 0xff));
}

static esp_err_t validate_header(const rpi_ota_header_t *header, uint32_t *image_size)
{
    const uint32_t magic = ntohl(header->magic_be);
    const uint16_t version = ntohs(header->version_be);
    const uint16_t header_len = ntohs(header->header_len_be);
    const uint32_t size = ntohl(header->image_size_be);

    ESP_RETURN_ON_FALSE(magic == RPI_OTA_MAGIC, ESP_ERR_INVALID_ARG, TAG, "bad magic");
    ESP_RETURN_ON_FALSE(version == RPI_OTA_PROTOCOL_VER, ESP_ERR_INVALID_VERSION, TAG, "bad protocol version");
    ESP_RETURN_ON_FALSE(header_len == sizeof(rpi_ota_header_t), ESP_ERR_INVALID_SIZE, TAG, "bad header length");
    ESP_RETURN_ON_FALSE(size > 0, ESP_ERR_INVALID_SIZE, TAG, "empty image");

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    ESP_RETURN_ON_FALSE(partition != NULL, ESP_ERR_NOT_FOUND, TAG, "no OTA partition");
    ESP_RETURN_ON_FALSE(size <= partition->size, ESP_ERR_INVALID_SIZE, TAG,
                        "image too large (%" PRIu32 " > %" PRIu32 ")", size, partition->size);

    *image_size = size;
    return ESP_OK;
}

static esp_err_t maybe_validate_app_descriptor(const esp_partition_t *partition)
{
    esp_app_desc_t new_app = {0};
    ESP_RETURN_ON_ERROR(esp_ota_get_partition_description(partition, &new_app), TAG, "read new app descriptor failed");

    const esp_app_desc_t *running_app = esp_app_get_description();
    ESP_LOGI(TAG, "received app: project=%s version=%s", new_app.project_name, new_app.version);
    ESP_LOGI(TAG, "running app:  project=%s version=%s", running_app->project_name, running_app->version);

#if CONFIG_RPI_OTA_REQUIRE_VERSION_INCREMENT
    ESP_RETURN_ON_FALSE(strcmp(new_app.version, running_app->version) != 0, ESP_ERR_INVALID_VERSION, TAG,
                        "received version equals running version");
#endif

    return ESP_OK;
}

static esp_err_t receive_image(int sock, const rpi_ota_header_t *header)
{
    uint32_t image_size = 0;
    ESP_RETURN_ON_ERROR(validate_header(header, &image_size), TAG, "header rejected");

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    ESP_LOGI(TAG, "writing %" PRIu32 " bytes to %s at 0x%" PRIx32, image_size, partition->label, partition->address);

    esp_ota_handle_t update_handle = 0;
    ESP_RETURN_ON_ERROR(esp_ota_begin(partition, image_size, &update_handle), TAG, "ota begin failed");

    uint8_t *buffer = malloc(RPI_OTA_CHUNK_SIZE);
    if (buffer == NULL) {
        esp_ota_abort(update_handle);
        return ESP_ERR_NO_MEM;
    }

    SHA_CTX sha_ctx = {0};
    ets_sha_enable();
    ets_sha_init(&sha_ctx, SHA2_256);
    ets_sha_starts(&sha_ctx, 0);

    uint32_t remaining = image_size;
    esp_err_t err = ESP_OK;
    while (remaining > 0) {
        const size_t want = remaining > RPI_OTA_CHUNK_SIZE ? RPI_OTA_CHUNK_SIZE : remaining;
        const ssize_t got = recv(sock, buffer, want, 0);
        if (got <= 0) {
            err = ESP_ERR_INVALID_RESPONSE;
            ESP_LOGE(TAG, "socket receive failed: errno=%d", errno);
            break;
        }

        rpi_camera_health_feed();
        ets_sha_update(&sha_ctx, buffer, (uint32_t)got, true);
        err = esp_ota_write(update_handle, buffer, (size_t)got);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota write failed: %s", esp_err_to_name(err));
            break;
        }
        remaining -= (uint32_t)got;
    }

    uint8_t digest[32] = {0};
    ets_sha_finish(&sha_ctx, digest);
    ets_sha_disable();
    free(buffer);

    if (err != ESP_OK) {
        esp_ota_abort(update_handle);
        return err;
    }

    if (memcmp(digest, header->sha256, sizeof(digest)) != 0) {
        esp_ota_abort(update_handle);
        ESP_LOGE(TAG, "sha256 mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    ESP_RETURN_ON_ERROR(esp_ota_end(update_handle), TAG, "ota end failed");
    ESP_RETURN_ON_ERROR(maybe_validate_app_descriptor(partition), TAG, "app descriptor rejected");
    ESP_RETURN_ON_ERROR(esp_ota_set_boot_partition(partition), TAG, "set boot partition failed");

    ESP_LOGI(TAG, "OTA complete; rebooting in %d ms", CONFIG_RPI_OTA_REBOOT_DELAY_MS);
    return ESP_OK;
}

static void ota_server_task(void *arg)
{
    (void)arg;

    while (true) {
        int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listen_sock < 0) {
            ESP_LOGE(TAG, "socket create failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        const int yes = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons((uint16_t)RPI_OTA_TCP_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };

        if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
            listen(listen_sock, 1) != 0) {
            ESP_LOGE(TAG, "bind/listen failed: errno=%d", errno);
            close(listen_sock);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "listening on TCP port %d (Ethernet OTA)", RPI_OTA_TCP_PORT);
        while (true) {
            struct sockaddr_in source_addr = {0};
            socklen_t addr_len = sizeof(source_addr);
            int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
            if (sock < 0) {
                ESP_LOGE(TAG, "accept failed: errno=%d", errno);
                break;
            }

            char ip_addr[16] = {0};
            format_ipv4_addr(source_addr.sin_addr.s_addr, ip_addr, sizeof(ip_addr));
            ESP_LOGI(TAG, "OTA client connected: %s", ip_addr);

            rpi_ota_header_t header = {0};
            esp_err_t err = ESP_FAIL;
            if (read_exact(sock, &header, sizeof(header))) {
                err = receive_image(sock, &header);
            } else {
                err = ESP_ERR_INVALID_RESPONSE;
            }

            if (err == ESP_OK) {
                send_status(sock, "OK rebooting\n");
                close(sock);
                vTaskDelay(pdMS_TO_TICKS(CONFIG_RPI_OTA_REBOOT_DELAY_MS));
                esp_restart();
            } else {
                ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
                send_status(sock, "ERR ota failed\n");
                close(sock);
            }
        }

        close(listen_sock);
    }
}

esp_err_t rpi_camera_ota_start(void)
{
    BaseType_t ok = xTaskCreate(ota_server_task, "ota_tcp", RPI_OTA_TASK_STACK, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
