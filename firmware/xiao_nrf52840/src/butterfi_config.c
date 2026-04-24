/*
 * butterfi_config.c
 */

#include <zephyr/logging/log.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <string.h>
#include "butterfi_config.h"

LOG_MODULE_REGISTER(butterfi_config, LOG_LEVEL_INF);

static struct nvs_fs fs;
static butterfi_config_t active_config;

#define NVS_PARTITION        storage_partition
#define NVS_PARTITION_OFFSET FIXED_PARTITION_OFFSET(NVS_PARTITION)
#define NVS_PARTITION_DEVICE FIXED_PARTITION_DEVICE(NVS_PARTITION)

int butterfi_config_load(void)
{
    int ret;

    struct flash_pages_info info;
    fs.flash_device = NVS_PARTITION_DEVICE;
    fs.offset       = NVS_PARTITION_OFFSET;

    ret = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info);
    if (ret < 0) {
        LOG_ERR("Flash page info failed: %d", ret);
        return ret;
    }

    fs.sector_size  = info.size;
    fs.sector_count = 3U;

    ret = nvs_mount(&fs);
    if (ret < 0) {
        LOG_ERR("NVS mount failed: %d", ret);
        return ret;
    }

    /* Read each field — if any are missing, treat as unprovisioned */
    ssize_t len;

    len = nvs_read(&fs, NVS_ID_SCHOOL_ID,
                   active_config.school_id,
                   sizeof(active_config.school_id));
    if (len <= 0) {
        LOG_WRN("school_id not found in NVS");
        return -ENOENT;
    }

    len = nvs_read(&fs, NVS_ID_DEVICE_NAME,
                   active_config.device_name,
                   sizeof(active_config.device_name));
    if (len <= 0) {
        strncpy(active_config.device_name, "ButterFi-Dongle",
                sizeof(active_config.device_name));
    }

    len = nvs_read(&fs, NVS_ID_CONTENT_PKG,
                   active_config.content_pkg,
                   sizeof(active_config.content_pkg));
    if (len <= 0) {
        strncpy(active_config.content_pkg, "k12-general",
                sizeof(active_config.content_pkg));
    }

    uint8_t prov = 0;
    nvs_read(&fs, NVS_ID_PROVISIONED, &prov, sizeof(prov));
    active_config.provisioned = (prov == 1);

    LOG_INF("Config loaded OK");
    return 0;
}

int butterfi_config_save(const butterfi_config_t *cfg)
{
    int ret;

    ret = nvs_write(&fs, NVS_ID_SCHOOL_ID,
                    cfg->school_id,
                    strnlen(cfg->school_id, BUTTERFI_SCHOOL_ID_MAX) + 1);
    if (ret < 0) { LOG_ERR("NVS write school_id failed: %d", ret); return ret; }

    ret = nvs_write(&fs, NVS_ID_DEVICE_NAME,
                    cfg->device_name,
                    strnlen(cfg->device_name, BUTTERFI_DEVICE_NAME_MAX) + 1);
    if (ret < 0) { LOG_ERR("NVS write device_name failed: %d", ret); return ret; }

    ret = nvs_write(&fs, NVS_ID_CONTENT_PKG,
                    cfg->content_pkg,
                    strnlen(cfg->content_pkg, BUTTERFI_CONTENT_PKG_MAX) + 1);
    if (ret < 0) { LOG_ERR("NVS write content_pkg failed: %d", ret); return ret; }

    uint8_t prov = 1;
    ret = nvs_write(&fs, NVS_ID_PROVISIONED, &prov, sizeof(prov));
    if (ret < 0) { LOG_ERR("NVS write provisioned failed: %d", ret); return ret; }

    memcpy(&active_config, cfg, sizeof(butterfi_config_t));
    active_config.provisioned = true;

    LOG_INF("Config saved: school=%s pkg=%s",
            active_config.school_id, active_config.content_pkg);
    return 0;
}

void butterfi_config_clear(void)
{
    nvs_clear(&fs);
    memset(&active_config, 0, sizeof(active_config));
    LOG_WRN("Config cleared");
}

const char *butterfi_config_get_school_id(void)   { return active_config.school_id; }
const char *butterfi_config_get_device_name(void) { return active_config.device_name; }
const char *butterfi_config_get_content_pkg(void) { return active_config.content_pkg; }
bool        butterfi_config_is_provisioned(void)  { return active_config.provisioned; }
