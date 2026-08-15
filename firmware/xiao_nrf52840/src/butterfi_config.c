/*
 * butterfi_config.c
 */

#include <zephyr/logging/log.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <pm_config.h>
#include <string.h>
#include "butterfi_config.h"

LOG_MODULE_REGISTER(butterfi_config, LOG_LEVEL_INF);

static struct nvs_fs fs;
static butterfi_config_t active_config;
static bool nvs_ready;   /* true once nvs_mount() has succeeded */

int butterfi_config_load(void)
{
    int ret;
    struct flash_pages_info info;
    const struct flash_area *fa;

    /* Mount NVS on the dedicated butterfi_storage partition (Partition
     * Manager), NOT the DTS storage_partition — the latter overlaps
     * settings_storage, where Sidewalk keeps its registration/time-sync key
     * store, and this NVS instance would corrupt it on mount. */
    ret = flash_area_open(PM_BUTTERFI_STORAGE_ID, &fa);
    if (ret < 0) {
        LOG_ERR("Open butterfi_storage failed: %d", ret);
        return ret;
    }
    fs.flash_device = flash_area_get_device(fa);
    fs.offset       = fa->fa_off;
    flash_area_close(fa);

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
    nvs_ready = true;

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
    /* nvs_clear() erases the whole butterfi_storage partition, including
     * NVS_ID_MAINT_GEN — so a Sidewalk factory reset (the only caller) re-arms
     * the one-time settings_storage wipe on the next boot. That is intentional:
     * a factory reset should also drop any stale maintenance state. nvs_clear()
     * also leaves fs unmounted, so writes fail until reboot — the caller
     * (on_factory_reset) sys_reboot()s immediately, so that is fine today. */
    nvs_clear(&fs);
    nvs_ready = false;
    memset(&active_config, 0, sizeof(active_config));
    LOG_WRN("Config cleared");
}

int butterfi_config_get_maint_gen(uint32_t *gen)
{
    ssize_t len;

    if (gen == NULL) {
        return -EINVAL;
    }

    /* Fail closed: if NVS never mounted, an unmounted nvs_read returns
     * -EACCES, and reporting gen=0 here would run the (unrecordable)
     * maintenance wipe on every boot forever. Return the error instead so the
     * caller skips the wipe. */
    if (!nvs_ready) {
        return -EIO;
    }

    *gen = 0;
    len = nvs_read(&fs, NVS_ID_MAINT_GEN, gen, sizeof(*gen));
    if (len == -ENOENT) {
        *gen = 0;   /* never written yet — a legitimate 0 */
        return 0;
    }
    if (len != (ssize_t)sizeof(*gen)) {
        return (len < 0) ? (int)len : -EIO;
    }
    return 0;
}

int butterfi_config_set_maint_gen(uint32_t gen)
{
    int ret;

    if (!nvs_ready) {
        return -EIO;
    }

    ret = nvs_write(&fs, NVS_ID_MAINT_GEN, &gen, sizeof(gen));
    return (ret < 0) ? ret : 0;
}

const char *butterfi_config_get_school_id(void)   { return active_config.school_id; }
const char *butterfi_config_get_device_name(void) { return active_config.device_name; }
const char *butterfi_config_get_content_pkg(void) { return active_config.content_pkg; }
bool        butterfi_config_is_provisioned(void)  { return active_config.provisioned; }
