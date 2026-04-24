/*
 * butterfi_config.h
 *
 * NVS-backed device configuration.
 * Values are written by the web provisioning tool over USB CDC,
 * and read at boot to configure Sidewalk and content selection.
 */

#ifndef BUTTERFI_CONFIG_H
#define BUTTERFI_CONFIG_H

#include <zephyr/kernel.h>

/* NVS key IDs */
#define NVS_ID_SCHOOL_ID     1
#define NVS_ID_DEVICE_NAME   2
#define NVS_ID_CONTENT_PKG   3
#define NVS_ID_PROVISIONED   4   /* u8: 0=no, 1=yes */

#define BUTTERFI_SCHOOL_ID_MAX   32
#define BUTTERFI_DEVICE_NAME_MAX 48
#define BUTTERFI_CONTENT_PKG_MAX 24

typedef struct {
    char school_id[BUTTERFI_SCHOOL_ID_MAX];
    char device_name[BUTTERFI_DEVICE_NAME_MAX];
    char content_pkg[BUTTERFI_CONTENT_PKG_MAX];
    bool provisioned;
} butterfi_config_t;

/**
 * Load config from NVS flash storage.
 * Returns 0 on success, -ENOENT if not yet provisioned.
 */
int butterfi_config_load(void);

/**
 * Write a new config to NVS. Called by the USB provisioning handler.
 */
int butterfi_config_save(const butterfi_config_t *cfg);

/**
 * Erase all stored config (factory reset).
 */
void butterfi_config_clear(void);

const char *butterfi_config_get_school_id(void);
const char *butterfi_config_get_device_name(void);
const char *butterfi_config_get_content_pkg(void);
bool        butterfi_config_is_provisioned(void);

#endif /* BUTTERFI_CONFIG_H */
