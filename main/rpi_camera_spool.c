/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "rpi_camera_config.h"
#include "rpi_camera_spool.h"

static const char *TAG = "rpi_spool";

static wl_handle_t s_wl_handle;
static bool s_mounted;
static uint32_t s_pending_count;

static void spool_path_for_seq(uint32_t sequence, char *path, size_t path_len)
{
    snprintf(path, path_len, RPI_SPOOL_MOUNT_POINT "/s%08" PRIu32 ".pkt", sequence);
}

static uint32_t seq_from_filename(const char *name)
{
    unsigned long seq = 0;
    if (sscanf(name, "s%08lu.pkt", &seq) == 1) {
        return (uint32_t)seq;
    }
    return UINT32_MAX;
}

static uint32_t count_spool_files(void)
{
    DIR *dir = opendir(RPI_SPOOL_MOUNT_POINT);
    if (dir == NULL) {
        return 0;
    }

    uint32_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (seq_from_filename(entry->d_name) != UINT32_MAX) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

static esp_err_t find_oldest_spool_seq(uint32_t *seq_out)
{
    DIR *dir = opendir(RPI_SPOOL_MOUNT_POINT);
    ESP_RETURN_ON_FALSE(dir, ESP_FAIL, TAG, "opendir failed");

    uint32_t oldest_seq = UINT32_MAX;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        uint32_t seq = seq_from_filename(entry->d_name);
        if (seq != UINT32_MAX && seq < oldest_seq) {
            oldest_seq = seq;
        }
    }
    closedir(dir);

    if (oldest_seq == UINT32_MAX) {
        return ESP_ERR_NOT_FOUND;
    }

    *seq_out = oldest_seq;
    return ESP_OK;
}

static esp_err_t delete_oldest_spool_file(void)
{
    uint32_t oldest_seq = 0;
    ESP_RETURN_ON_ERROR(find_oldest_spool_seq(&oldest_seq), TAG, "no spool file");

    char path[64];
    spool_path_for_seq(oldest_seq, path, sizeof(path));
    if (unlink(path) != 0) {
        ESP_LOGW(TAG, "failed to drop oldest spool file %s", path);
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "spool full — dropped oldest seq=%" PRIu32, oldest_seq);
    if (s_pending_count > 0) {
        s_pending_count--;
    }
    return ESP_OK;
}

esp_err_t rpi_camera_spool_init(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
    };

    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(RPI_SPOOL_MOUNT_POINT, "spool", &mount_config, &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return err;
    }

    s_mounted = true;
    s_pending_count = count_spool_files();
    ESP_LOGI(TAG, "mounted %s — %u packet(s) pending", RPI_SPOOL_MOUNT_POINT, (unsigned)s_pending_count);
    return ESP_OK;
}

esp_err_t rpi_camera_spool_store(const uint8_t *packet, size_t packet_len, uint32_t sequence)
{
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "spool not mounted");
    ESP_RETURN_ON_FALSE(packet && packet_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid packet");

    if (s_pending_count >= RPI_SPOOL_MAX_FILES) {
        ESP_RETURN_ON_ERROR(delete_oldest_spool_file(), TAG, "drop oldest");
    }

    char path[64];
    spool_path_for_seq(sequence, path, sizeof(path));

    FILE *f = fopen(path, "wb");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "open %s failed", path);

    size_t written = fwrite(packet, 1, packet_len, f);
    fclose(f);
    if (written != packet_len) {
        unlink(path);
        ESP_LOGE(TAG, "write failed for seq=%" PRIu32, sequence);
        return ESP_FAIL;
    }

    s_pending_count++;
    ESP_LOGW(TAG, "spooled seq=%" PRIu32 " (%u bytes), pending=%u", sequence, (unsigned)packet_len,
             (unsigned)s_pending_count);
    return ESP_OK;
}

esp_err_t rpi_camera_spool_flush_one(bool (*send_fn)(const uint8_t *packet, size_t packet_len, uint32_t sequence, void *ctx), void *ctx)
{
    ESP_RETURN_ON_FALSE(s_mounted, ESP_ERR_INVALID_STATE, TAG, "spool not mounted");
    ESP_RETURN_ON_FALSE(send_fn, ESP_ERR_INVALID_ARG, TAG, "no send_fn");

    if (s_pending_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    char path[64];
    uint32_t sequence = 0;
    ESP_RETURN_ON_ERROR(find_oldest_spool_seq(&sequence), TAG, "no spool file");
    spool_path_for_seq(sequence, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "open %s failed", path);

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long file_len = ftell(f);
    if (file_len <= 0) {
        fclose(f);
        unlink(path);
        if (s_pending_count > 0) {
            s_pending_count--;
        }
        return ESP_FAIL;
    }
    rewind(f);

    uint8_t *buf = malloc((size_t)file_len);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t nread = fread(buf, 1, (size_t)file_len, f);
    fclose(f);
    if (nread != (size_t)file_len) {
        free(buf);
        return ESP_FAIL;
    }

    bool ok = send_fn(buf, (size_t)file_len, sequence, ctx);
    free(buf);
    if (!ok) {
        return ESP_FAIL;
    }

    if (unlink(path) != 0) {
        ESP_LOGW(TAG, "sent spooled seq=%" PRIu32 " but unlink failed", sequence);
    } else if (s_pending_count > 0) {
        s_pending_count--;
    }

    ESP_LOGI(TAG, "flushed spooled seq=%" PRIu32 ", pending=%u", sequence, (unsigned)s_pending_count);
    return ESP_OK;
}

uint32_t rpi_camera_spool_pending_count(void)
{
    return s_pending_count;
}
