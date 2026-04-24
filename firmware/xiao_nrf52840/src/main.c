/*
 * ButterFi — Amazon Sidewalk Firmware
 * Target: Seeed Studio XIAO nRF52840
 * SDK:    nRF Connect SDK (Zephyr RTOS)
 *
 * Architecture:
 *   - Sidewalk BLE handles cloud connectivity
 *   - USB CDC-ACM exposes a simple serial command interface
 *     so the web provisioning tool can read status / push config
 *   - Config is stored in Zephyr NVS (non-volatile storage)
 *   - LED reflects connection state
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#include <sid_api.h>
#include <sid_error.h>
#include <sid_pal_common_ifc.h>

#include "butterfi_config.h"
#include "butterfi_content.h"
#include "butterfi_usb.h"

LOG_MODULE_REGISTER(butterfi_main, LOG_LEVEL_INF);

/* ── LED (XIAO nRF52840: RGB on P0.26/P0.30/P0.06) ─────────────────────── */
#define LED_RED_NODE   DT_ALIAS(led0)
#define LED_GREEN_NODE DT_ALIAS(led1)
#define LED_BLUE_NODE  DT_ALIAS(led2)

static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(LED_RED_NODE,   gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(LED_GREEN_NODE, gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(LED_BLUE_NODE,  gpios);

typedef enum {
    LED_STATE_BOOT,        /* Blue pulse  — booting              */
    LED_STATE_UNPROVISIONED, /* Red slow    — needs provisioning  */
    LED_STATE_CONNECTING,  /* Yellow blink — connecting Sidewalk  */
    LED_STATE_CONNECTED,   /* Green solid  — ready               */
    LED_STATE_SENDING,     /* White flash  — transmitting        */
    LED_STATE_ERROR,       /* Red fast     — fault               */
} led_state_t;

static led_state_t current_led_state = LED_STATE_BOOT;

static void led_set(bool r, bool g, bool b)
{
    /* XIAO LEDs are active-low */
    gpio_pin_set_dt(&led_r, !r);
    gpio_pin_set_dt(&led_g, !g);
    gpio_pin_set_dt(&led_b, !b);
}

/* ── Sidewalk state ─────────────────────────────────────────────────────── */
typedef enum {
    SIDEWALK_STATE_INIT,
    SIDEWALK_STATE_READY,
    SIDEWALK_STATE_NOT_REGISTERED,
    SIDEWALK_STATE_ERROR,
} sidewalk_state_t;

static sidewalk_state_t sidewalk_state = SIDEWALK_STATE_INIT;
static struct sid_handle *sid_handle   = NULL;

K_SEM_DEFINE(sidewalk_event_sem, 0, 1);

/* ── Sidewalk event callbacks ───────────────────────────────────────────── */
static void on_sidewalk_event(bool in_isr, void *context)
{
    /* Called when a Sidewalk event is pending; process from main thread */
    k_sem_give(&sidewalk_event_sem);
}

static void on_msg_received(const struct sid_msg_desc *msg_desc,
                             const struct sid_msg *msg,
                             void *context)
{
    LOG_INF("MSG RX: type=%d, id=%u, size=%u",
            msg_desc->type, msg_desc->id, msg->size);

    /* Hand off to content module — it decides what to serve */
    butterfi_content_handle_msg(msg_desc, msg);
}

static void on_msg_sent(const struct sid_msg_desc *msg_desc, void *context)
{
    LOG_INF("MSG TX ACK: id=%u", msg_desc->id);
    current_led_state = LED_STATE_CONNECTED;
}

static void on_send_error(sid_error_t error,
                          const struct sid_msg_desc *msg_desc,
                          void *context)
{
    LOG_ERR("MSG TX ERR: %d (id=%u)", error, msg_desc->id);
    current_led_state = LED_STATE_CONNECTED;
}

static void on_status_changed(const struct sid_status *status, void *context)
{
    LOG_INF("Sidewalk status: state=%d, reg=%d",
            status->state, status->detail.registration_status);

    switch (status->state) {
    case SID_STATE_READY:
        sidewalk_state = SIDEWALK_STATE_READY;
        current_led_state = LED_STATE_CONNECTED;
        LOG_INF("Sidewalk READY — ButterFi online");
        butterfi_usb_send_status("SIDEWALK_READY");
        break;

    case SID_STATE_NOT_READY:
        sidewalk_state = SIDEWALK_STATE_INIT;
        current_led_state = LED_STATE_CONNECTING;
        break;

    case SID_STATE_ERROR:
        sidewalk_state = SIDEWALK_STATE_ERROR;
        current_led_state = LED_STATE_ERROR;
        LOG_ERR("Sidewalk ERROR");
        butterfi_usb_send_status("SIDEWALK_ERROR");
        break;
    }

    if (status->detail.registration_status == SID_STATUS_NOT_REGISTERED) {
        sidewalk_state = SIDEWALK_STATE_NOT_REGISTERED;
        current_led_state = LED_STATE_UNPROVISIONED;
        LOG_WRN("Device not registered — run provisioning tool");
        butterfi_usb_send_status("NEEDS_PROVISIONING");
    }
}

static void on_factory_reset(void *context)
{
    LOG_WRN("Factory reset triggered via Sidewalk");
    butterfi_config_clear();
    sys_reboot(SYS_REBOOT_COLD);
}

static const struct sid_event_callbacks sidewalk_callbacks = {
    .context        = NULL,
    .on_event       = on_sidewalk_event,
    .on_msg_received = on_msg_received,
    .on_msg_sent    = on_msg_sent,
    .on_send_error  = on_send_error,
    .on_status_changed = on_status_changed,
    .on_factory_reset  = on_factory_reset,
};

static int sidewalk_init(void)
{
    struct sid_config config = {
        .callbacks    = &sidewalk_callbacks,
        .link_mask    = SID_LINK_TYPE_1,   /* BLE only for XIAO cert */
        .time_sync_periodicity_seconds = 7200,
    };

    sid_error_t err = sid_init(&config, &sid_handle);
    if (err != SID_ERROR_NONE) {
        LOG_ERR("sid_init failed: %d", err);
        return -EFAULT;
    }

    err = sid_start(sid_handle, SID_LINK_TYPE_1);
    if (err != SID_ERROR_NONE) {
        LOG_ERR("sid_start failed: %d", err);
        return -EFAULT;
    }

    LOG_INF("Sidewalk stack started");
    return 0;
}

/* ── LED blink thread ───────────────────────────────────────────────────── */
#define LED_STACK_SIZE 512
#define LED_PRIORITY   7

static void led_thread_fn(void *a, void *b, void *c)
{
    int tick = 0;

    gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led_b, GPIO_OUTPUT_INACTIVE);

    while (1) {
        tick++;
        switch (current_led_state) {
        case LED_STATE_BOOT:
            led_set(0, 0, tick % 4 < 2);         /* blue pulse        */
            break;
        case LED_STATE_UNPROVISIONED:
            led_set(tick % 8 < 1, 0, 0);          /* red slow blink    */
            break;
        case LED_STATE_CONNECTING:
            led_set(tick % 4 < 2, tick % 4 < 2, 0); /* yellow blink    */
            break;
        case LED_STATE_CONNECTED:
            led_set(0, 1, 0);                      /* green solid       */
            break;
        case LED_STATE_SENDING:
            led_set(1, 1, 1);                      /* white flash       */
            break;
        case LED_STATE_ERROR:
            led_set(tick % 2, 0, 0);               /* red fast blink    */
            break;
        }
        k_msleep(250);
    }
}

K_THREAD_DEFINE(led_tid, LED_STACK_SIZE,
                led_thread_fn, NULL, NULL, NULL,
                LED_PRIORITY, 0, 0);

/* ── Main ───────────────────────────────────────────────────────────────── */
int main(void)
{
    int ret;

    LOG_INF("ButterFi v%s starting", CONFIG_BUTTERFI_VERSION);

    /* Load config from NVS — school ID, content package, etc. */
    ret = butterfi_config_load();
    if (ret < 0) {
        LOG_WRN("No config found — device needs provisioning");
        current_led_state = LED_STATE_UNPROVISIONED;
    } else {
        LOG_INF("Config loaded: school=%s pkg=%s",
                butterfi_config_get_school_id(),
                butterfi_config_get_content_pkg());
    }

    /* USB CDC-ACM for provisioning tool comms */
    ret = butterfi_usb_init();
    if (ret < 0) {
        LOG_ERR("USB init failed: %d", ret);
    }

    /* Sidewalk stack */
    ret = sidewalk_init();
    if (ret < 0) {
        current_led_state = LED_STATE_ERROR;
        LOG_ERR("Sidewalk init failed — halting");
        return ret;
    }

    current_led_state = LED_STATE_CONNECTING;

    /* Main event loop — Sidewalk is event-driven */
    while (1) {
        if (k_sem_take(&sidewalk_event_sem, K_MSEC(100)) == 0) {
            sid_error_t err = sid_process(sid_handle);
            if (err != SID_ERROR_NONE) {
                LOG_ERR("sid_process error: %d", err);
            }
        }

        /* Poll USB for incoming provisioning commands */
        butterfi_usb_poll();
    }

    return 0;
}
