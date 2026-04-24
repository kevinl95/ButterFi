/*
 * butterfi_usb.c
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/reboot.h>
#include <string.h>
#include <stdio.h>

#include "butterfi_usb.h"
#include "butterfi_config.h"

LOG_MODULE_REGISTER(butterfi_usb, LOG_LEVEL_DBG);

/* Uses the CDC ACM UART defined in the devicetree / Kconfig */
#define CDC_DEV_NAME "CDC_ACM_0"

static const struct device *cdc_dev;

#define RX_BUF_SIZE  256
#define TX_BUF_SIZE  512

static char rx_buf[RX_BUF_SIZE];
static int  rx_pos = 0;

/* ── Minimal JSON helpers ───────────────────────────────────────────────── */

static const char *json_get_str(const char *json, const char *key,
                                 char *out, size_t out_len)
{
    /* Very small hand-rolled extractor — no heap, no external deps.
     * Finds "key":"value" and copies value into out. */
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);

    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);

    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return out;
}

/* ── TX helpers ─────────────────────────────────────────────────────────── */

static void usb_write_str(const char *s)
{
    if (!cdc_dev || !device_is_ready(cdc_dev)) return;

    size_t len = strlen(s);
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(cdc_dev, s[i]);
    }
    /* Newline as frame delimiter */
    uart_poll_out(cdc_dev, '\n');
}

void butterfi_usb_send_status(const char *sidewalk_state)
{
    char buf[TX_BUF_SIZE];
    snprintf(buf, sizeof(buf),
        "{\"event\":\"status\","
        "\"fw\":\"%s\","
        "\"school_id\":\"%s\","
        "\"device_name\":\"%s\","
        "\"content_pkg\":\"%s\","
        "\"provisioned\":%s,"
        "\"sidewalk\":\"%s\"}",
        CONFIG_BUTTERFI_VERSION,
        butterfi_config_get_school_id(),
        butterfi_config_get_device_name(),
        butterfi_config_get_content_pkg(),
        butterfi_config_is_provisioned() ? "true" : "false",
        sidewalk_state
    );
    usb_write_str(buf);
}

/* ── Command dispatcher ─────────────────────────────────────────────────── */

static void handle_command(const char *line)
{
    char cmd[32] = {0};
    json_get_str(line, "cmd", cmd, sizeof(cmd));

    LOG_DBG("USB CMD: %s", cmd);

    if (strcmp(cmd, "ping") == 0) {
        usb_write_str("{\"ok\":true,\"pong\":true}");

    } else if (strcmp(cmd, "status") == 0) {
        butterfi_usb_send_status("UNKNOWN");

    } else if (strcmp(cmd, "provision") == 0) {
        butterfi_config_t cfg = {0};

        if (!json_get_str(line, "school_id",
                          cfg.school_id, sizeof(cfg.school_id))) {
            usb_write_str("{\"ok\":false,\"error\":\"missing school_id\"}");
            return;
        }
        json_get_str(line, "device_name",
                     cfg.device_name, sizeof(cfg.device_name));
        if (cfg.device_name[0] == '\0') {
            strncpy(cfg.device_name, "ButterFi-Dongle",
                    sizeof(cfg.device_name));
        }
        if (!json_get_str(line, "content_pkg",
                          cfg.content_pkg, sizeof(cfg.content_pkg))) {
            strncpy(cfg.content_pkg, "k12-general",
                    sizeof(cfg.content_pkg));
        }

        int ret = butterfi_config_save(&cfg);
        if (ret == 0) {
            char resp[TX_BUF_SIZE];
            snprintf(resp, sizeof(resp),
                "{\"ok\":true,"
                "\"school_id\":\"%s\","
                "\"device_name\":\"%s\","
                "\"content_pkg\":\"%s\"}",
                cfg.school_id, cfg.device_name, cfg.content_pkg);
            usb_write_str(resp);
        } else {
            char resp[64];
            snprintf(resp, sizeof(resp),
                     "{\"ok\":false,\"error\":\"nvs_write failed: %d\"}", ret);
            usb_write_str(resp);
        }

    } else if (strcmp(cmd, "reset") == 0) {
        usb_write_str("{\"ok\":true,\"msg\":\"resetting\"}");
        k_msleep(100);
        sys_reboot(SYS_REBOOT_COLD);

    } else {
        char resp[64];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":false,\"error\":\"unknown cmd: %s\"}", cmd);
        usb_write_str(resp);
    }
}

/* ── USB poll (called from main loop) ──────────────────────────────────── */

void butterfi_usb_poll(void)
{
    if (!cdc_dev || !device_is_ready(cdc_dev)) return;

    unsigned char c;
    while (uart_poll_in(cdc_dev, &c) == 0) {
        if (c == '\n' || c == '\r') {
            if (rx_pos > 0) {
                rx_buf[rx_pos] = '\0';
                handle_command(rx_buf);
                rx_pos = 0;
            }
        } else if (rx_pos < RX_BUF_SIZE - 1) {
            rx_buf[rx_pos++] = (char)c;
        }
    }
}

/* ── Init ───────────────────────────────────────────────────────────────── */

int butterfi_usb_init(void)
{
    int ret;

    ret = usb_enable(NULL);
    if (ret < 0) {
        LOG_ERR("USB enable failed: %d", ret);
        return ret;
    }

    cdc_dev = device_get_binding(CDC_DEV_NAME);
    if (!cdc_dev) {
        LOG_ERR("CDC ACM device not found");
        return -ENODEV;
    }

    /* Small delay for host enumeration */
    k_msleep(500);

    LOG_INF("USB CDC-ACM ready");
    butterfi_usb_send_status("INIT");
    return 0;
}
