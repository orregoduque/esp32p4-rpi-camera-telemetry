/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardened TCP uplink for camera snapshots.
 */

#include <inttypes.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "psa/crypto.h"
#include "driver/jpeg_encode.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_crc.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rpi_camera_config.h"
#include "rpi_camera_health.h"
#include "rpi_camera_net.h"
#include "rpi_camera_packet.h"
#include "rpi_camera_spool.h"
#include "rpi_camera_tcp.h"

static const char *TAG = "rpi_tcp";

#define RPI_TCP_NVS_NAMESPACE    "rpi_tcp"
#define RPI_TCP_NVS_SEQ_KEY      "last_seq"
#define RPI_TCP_NVS_FAIL_KEY     "send_fail"

static jpeg_encoder_handle_t s_jpeg_enc;
static uint8_t *s_jpeg_buf;
static size_t s_jpeg_buf_size;
static uint8_t *s_pkt_buf;
static size_t s_pkt_cap;
static uint32_t s_sequence;
static uint32_t s_last_saved_seq;
static uint32_t s_send_failures;
static int64_t s_last_connect_try_us;
static int s_sock = -1;
static mbedtls_svc_key_id_t s_hmac_key = 0;

static esp_err_t nvs_init_once(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
        err = nvs_flash_init();
    }
    return err;
}

static void load_counters_from_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(RPI_TCP_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    uint32_t seq = 0;
    if (nvs_get_u32(handle, RPI_TCP_NVS_SEQ_KEY, &seq) == ESP_OK) {
        s_sequence = seq;
        s_last_saved_seq = seq;
        ESP_LOGI(TAG, "restored sequence from NVS: %" PRIu32, s_sequence);
    }

    uint32_t fails = 0;
    if (nvs_get_u32(handle, RPI_TCP_NVS_FAIL_KEY, &fails) == ESP_OK) {
        s_send_failures = fails;
        ESP_LOGI(TAG, "restored TCP send failures from NVS: %" PRIu32, s_send_failures);
    }
    nvs_close(handle);
}

static esp_err_t save_u32_to_nvs(const char *key, uint32_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(RPI_TCP_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u32(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void save_sequence_to_nvs(uint32_t seq)
{
    if (seq <= s_last_saved_seq) {
        return;
    }
    s_last_saved_seq = seq;
    if (save_u32_to_nvs(RPI_TCP_NVS_SEQ_KEY, seq) != ESP_OK) {
        ESP_LOGW(TAG, "failed to persist sequence %" PRIu32, seq);
    }
}

static void record_send_failure(void)
{
    s_send_failures++;
    if (save_u32_to_nvs(RPI_TCP_NVS_FAIL_KEY, s_send_failures) != ESP_OK) {
        ESP_LOGW(TAG, "failed to persist send_fail count %" PRIu32, s_send_failures);
    } else {
        ESP_LOGW(TAG, "TCP send failed (lifetime failures in NVS: %" PRIu32 ")", s_send_failures);
    }
}

static uint64_t htonll(uint64_t v)
{
    uint64_t hi = htonl((uint32_t)(v >> 32));
    uint64_t lo = htonl((uint32_t)(v & 0xFFFFFFFFULL));
    return (lo << 32) | hi;
}

static ssize_t send_all(int sock, const uint8_t *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, data + sent, len - sent, 0);
        if (n <= 0) {
            return n;
        }
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

static ssize_t recv_all(int sock, uint8_t *data, size_t len)
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

static void close_socket(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}

void rpi_camera_tcp_on_link_down(void)
{
    close_socket();
    s_last_connect_try_us = 0;
}

static esp_err_t connect_socket(void)
{
    if (!rpi_camera_net_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_sock >= 0) {
        return ESP_OK;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    ESP_RETURN_ON_FALSE(sock >= 0, ESP_FAIL, TAG, "socket create failed");

    struct timeval to = {
        .tv_sec = RPI_TCP_SEND_TIMEOUT_MS / 1000,
        .tv_usec = (RPI_TCP_SEND_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
    to.tv_sec = RPI_TCP_RECV_TIMEOUT_MS / 1000;
    to.tv_usec = (RPI_TCP_RECV_TIMEOUT_MS % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)RPI_TCP_SERVER_PORT),
    };
    dest.sin_addr.s_addr = inet_addr(RPI_TCP_SERVER_IP);

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(sock, (struct sockaddr *)&dest, sizeof(dest));
    if (rc == 0) {
        fcntl(sock, F_SETFL, flags);
        s_sock = sock;
        ESP_LOGI(TAG, "connected to receiver %s:%d", RPI_TCP_SERVER_IP, RPI_TCP_SERVER_PORT);
        return ESP_OK;
    }
    if (rc != 0 && errno != EINPROGRESS) {
        ESP_LOGW(TAG, "connect %s:%d failed: %s (%d)", RPI_TCP_SERVER_IP, RPI_TCP_SERVER_PORT, strerror(errno), errno);
        close(sock);
        return ESP_FAIL;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    to.tv_sec = RPI_TCP_CONNECT_TIMEOUT_MS / 1000;
    to.tv_usec = (RPI_TCP_CONNECT_TIMEOUT_MS % 1000) * 1000;
    rc = select(sock + 1, NULL, &wfds, NULL, &to);
    if (rc <= 0) {
        ESP_LOGW(TAG, "connect %s:%d timeout (%d ms) — is tcp_receiver.py running on the PC?",
                 RPI_TCP_SERVER_IP, RPI_TCP_SERVER_PORT, RPI_TCP_CONNECT_TIMEOUT_MS);
        close(sock);
        return ESP_FAIL;
    }

    int sock_err = 0;
    socklen_t err_len = sizeof(sock_err);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &sock_err, &err_len);
    if (sock_err != 0) {
        ESP_LOGW(TAG, "connect %s:%d failed: %s (%d)", RPI_TCP_SERVER_IP, RPI_TCP_SERVER_PORT,
                 strerror(sock_err), sock_err);
        close(sock);
        return ESP_FAIL;
    }

    fcntl(sock, F_SETFL, flags);
    s_sock = sock;
    ESP_LOGI(TAG, "connected to receiver %s:%d", RPI_TCP_SERVER_IP, RPI_TCP_SERVER_PORT);
    return ESP_OK;
}

esp_err_t rpi_camera_tcp_maintain_connection(void)
{
    if (s_sock >= 0) {
        return ESP_OK;
    }

    int64_t now_us = esp_timer_get_time();
    int64_t retry_us = (int64_t)RPI_TCP_CONNECT_RETRY_SEC * 1000000;
    if (s_last_connect_try_us != 0 && (now_us - s_last_connect_try_us) < retry_us) {
        return ESP_ERR_INVALID_STATE;
    }
    s_last_connect_try_us = now_us;
    return connect_socket();
}

static bool send_packet_raw(const uint8_t *pkt, size_t len, uint32_t seq)
{
    for (int attempt = 1; attempt <= RPI_TCP_MAX_RETRIES; ++attempt) {
        rpi_camera_health_feed();
        if (connect_socket() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (send_all(s_sock, pkt, len) != (ssize_t)len) {
            ESP_LOGW(TAG, "send failed seq=%" PRIu32 " (attempt %d)", seq, attempt);
            close_socket();
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        rpi_node_packet_ack_t ack = {0};
        if (recv_all(s_sock, (uint8_t *)&ack, sizeof(ack)) != sizeof(ack)) {
            ESP_LOGW(TAG, "ack timeout seq=%" PRIu32 " (attempt %d)", seq, attempt);
            close_socket();
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        if (ntohl(ack.magic) != RPI_PKT_MAGIC || ntohl(ack.sequence) != seq || ntohl(ack.status) != 0U) {
            ESP_LOGW(TAG, "invalid ack for seq=%" PRIu32, seq);
            close_socket();
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        save_sequence_to_nvs(seq);
        return true;
    }
    return false;
}

static bool spool_send_cb(const uint8_t *packet, size_t packet_len, uint32_t sequence, void *ctx)
{
    (void)ctx;
    return send_packet_raw(packet, packet_len, sequence);
}

esp_err_t rpi_camera_tcp_process_spool(void)
{
    if (rpi_camera_spool_pending_count() == 0 || !rpi_camera_net_is_ready()) {
        return ESP_OK;
    }

    esp_err_t err = rpi_camera_spool_flush_one(spool_send_cb, NULL);
    if (err == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    return err;
}

static esp_err_t compute_hmac(const rpi_node_packet_header_t *header_no_hmac, const uint8_t *jpeg, size_t jpeg_len, uint8_t out_hmac[RPI_PKT_HMAC_SIZE])
{
    psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
    size_t mac_len = 0;

    if (s_hmac_key == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    psa_status_t st = psa_mac_sign_setup(&op, s_hmac_key, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    if (st != PSA_SUCCESS) {
        psa_mac_abort(&op);
        return ESP_FAIL;
    }
    st = psa_mac_update(&op, (const uint8_t *)header_no_hmac, sizeof(*header_no_hmac));
    if (st != PSA_SUCCESS) {
        psa_mac_abort(&op);
        return ESP_FAIL;
    }
    st = psa_mac_update(&op, jpeg, jpeg_len);
    if (st != PSA_SUCCESS) {
        psa_mac_abort(&op);
        return ESP_FAIL;
    }
    st = psa_mac_sign_finish(&op, out_hmac, RPI_PKT_HMAC_SIZE, &mac_len);
    if (st != PSA_SUCCESS || mac_len != RPI_PKT_HMAC_SIZE) {
        psa_mac_abort(&op);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t rpi_camera_tcp_init(void)
{
    ESP_RETURN_ON_ERROR(nvs_init_once(), TAG, "nvs init failed");

    s_sequence = 0;
    s_send_failures = 0;
    load_counters_from_nvs();

    psa_status_t st = psa_crypto_init();
    ESP_RETURN_ON_FALSE(st == PSA_SUCCESS, ESP_FAIL, TAG, "psa_crypto_init failed");

    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attrs, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_bits(&attrs, strlen(RPI_TCP_PSK) * 8);

    st = psa_import_key(&attrs, (const uint8_t *)RPI_TCP_PSK, strlen(RPI_TCP_PSK), &s_hmac_key);
    psa_reset_key_attributes(&attrs);
    ESP_RETURN_ON_FALSE(st == PSA_SUCCESS, ESP_FAIL, TAG, "psa_import_key failed");

    jpeg_encode_engine_cfg_t enc_cfg = { .timeout_ms = 800 };
    ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&enc_cfg, &s_jpeg_enc), TAG, "jpeg init failed");

    jpeg_encode_memory_alloc_cfg_t rx_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    s_jpeg_buf = jpeg_alloc_encoder_mem(RPI_CAM_FRAME_BYTES / 2, &rx_cfg, &s_jpeg_buf_size);
    ESP_RETURN_ON_FALSE(s_jpeg_buf, ESP_ERR_NO_MEM, TAG, "jpeg buf alloc failed");

    s_pkt_cap = sizeof(rpi_node_packet_header_t) + RPI_CAM_FRAME_BYTES;
    s_pkt_buf = heap_caps_malloc(s_pkt_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_pkt_buf, ESP_ERR_NO_MEM, TAG, "packet buf alloc failed");

    ESP_RETURN_ON_ERROR(rpi_camera_spool_init(), TAG, "spool init");

    if (rpi_camera_spool_pending_count() > 0) {
        ESP_LOGI(TAG, "%u spooled packet(s) waiting for upload", (unsigned)rpi_camera_spool_pending_count());
    }

    return ESP_OK;
}

uint32_t rpi_camera_tcp_get_send_failures(void)
{
    return s_send_failures;
}

uint32_t rpi_camera_tcp_get_spool_pending(void)
{
    return rpi_camera_spool_pending_count();
}

uint32_t rpi_camera_tcp_get_sequence(void)
{
    return s_sequence;
}

bool rpi_camera_tcp_is_connected(void)
{
    return s_sock >= 0;
}

static esp_err_t send_jpeg_packet(const void *rgb565, size_t rgb_len, uint16_t width, uint16_t height,
                                    uint32_t node_id, int16_t temperature_centi_c, uint16_t payload_type)
{
    ESP_RETURN_ON_FALSE(rgb565 && rgb_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid RGB frame");

    rpi_camera_health_feed();
    jpeg_encode_cfg_t cfg = {
        .width = width,
        .height = height,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = 85,
        .pixel_reverse = false,
    };

    uint32_t jpeg_len = 0;
    ESP_RETURN_ON_ERROR(jpeg_encoder_process(s_jpeg_enc, &cfg, rgb565, rgb_len, s_jpeg_buf, s_jpeg_buf_size, &jpeg_len), TAG, "jpeg encode failed");
    ESP_RETURN_ON_FALSE(jpeg_len > 0, ESP_FAIL, TAG, "empty jpeg");

    size_t total_len = sizeof(rpi_node_packet_header_t) + jpeg_len;
    ESP_RETURN_ON_FALSE(total_len <= s_pkt_cap, ESP_ERR_NO_MEM, TAG, "packet too large");

    rpi_node_packet_header_t header = {
        .magic = htonl(RPI_PKT_MAGIC),
        .version = htons(RPI_PKT_VERSION),
        .header_size = htons(sizeof(rpi_node_packet_header_t)),
        .node_id = htonl(node_id),
        .sequence = htonl(++s_sequence),
        .timestamp_ms = htonll((uint64_t)(esp_timer_get_time() / 1000)),
        .temperature_centi_c = htons((uint16_t)temperature_centi_c),
        .reserved = htons(payload_type),
        .jpeg_size = htonl(jpeg_len),
        .jpeg_crc32 = htonl(esp_crc32_le(0, s_jpeg_buf, jpeg_len)),
    };

    ESP_RETURN_ON_ERROR(compute_hmac(&header, s_jpeg_buf, jpeg_len, header.hmac_sha256), TAG, "hmac failed");

    memcpy(s_pkt_buf, &header, sizeof(header));
    memcpy(s_pkt_buf + sizeof(header), s_jpeg_buf, jpeg_len);

    const char *kind = (payload_type == RPI_PKT_PAYLOAD_THERMAL) ? "thermal" : "visible";
    if (send_packet_raw(s_pkt_buf, total_len, s_sequence)) {
        ESP_LOGI(TAG, "sent %s seq=%" PRIu32 " node=%" PRIu32 " temp=%.2fC jpg=%" PRIu32 "B",
                 kind, s_sequence, node_id, temperature_centi_c / 100.0f, jpeg_len);
        return ESP_OK;
    }

    if (rpi_camera_spool_store(s_pkt_buf, total_len, s_sequence) == ESP_OK) {
        record_send_failure();
        ESP_LOGW(TAG, "TCP failed — %s seq=%" PRIu32 " saved to spool (%u pending)",
                 kind, s_sequence, (unsigned)rpi_camera_spool_pending_count());
        return ESP_ERR_NOT_FINISHED;
    }

    s_sequence--;
    record_send_failure();
    ESP_LOGE(TAG, "TCP and spool failed — %s seq=%" PRIu32 " lost", kind, s_sequence + 1);
    return ESP_FAIL;
}

esp_err_t rpi_camera_tcp_send_snapshot(const void *rgb565, size_t rgb_len, uint32_t node_id, int16_t temperature_centi_c)
{
    ESP_RETURN_ON_FALSE(rgb_len == RPI_CAM_FRAME_BYTES, ESP_ERR_INVALID_ARG, TAG, "invalid visible frame");
    return send_jpeg_packet(rgb565, rgb_len, RPI_CAM_HRES, RPI_CAM_VRES, node_id, temperature_centi_c,
                            RPI_PKT_PAYLOAD_VISIBLE);
}

esp_err_t rpi_camera_tcp_send_thermal(const void *rgb565, size_t rgb_len, uint32_t node_id, int16_t temperature_centi_c)
{
    ESP_RETURN_ON_FALSE(rgb_len == RPI_THERMAL_RGB565_BYTES, ESP_ERR_INVALID_ARG, TAG, "invalid thermal frame");
    return send_jpeg_packet(rgb565, rgb_len, RPI_THERMAL_JPEG_W, RPI_THERMAL_JPEG_H, node_id, temperature_centi_c,
                            RPI_PKT_PAYLOAD_THERMAL);
}
