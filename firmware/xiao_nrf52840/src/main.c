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

#include <string.h>

#include <sid_api.h>
#include <sid_error.h>
#include <sid_pal_common_ifc.h>
#include <app_ble_config.h>

#include "butterfi_config.h"
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

#define BUTTERFI_SIDEWALK_MSG_QUERY 0x01
#define BUTTERFI_SIDEWALK_MSG_RESEND 0x02
#define BUTTERFI_SIDEWALK_MSG_ACK 0x03
#define BUTTERFI_SIDEWALK_MSG_RESPONSE_CHUNK 0x81
#define BUTTERFI_MAX_TRANSFER_CHUNKS 255
#define BUTTERFI_SIDEWALK_UPLINK_MAX_PAYLOAD 512

static bool usb_ready;
static uint8_t active_request_id;
static bool request_in_flight;
static uint8_t ignored_request_id;
static uint8_t active_total_chunks;
static size_t received_chunk_count;
static uint8_t received_chunk_bitmap[(BUTTERFI_MAX_TRANSFER_CHUNKS + 7) / 8];
static uint8_t current_link_state = BUTTERFI_USB_LINK_UNKNOWN;

#define MAX_TIME_SYNC_INTERVALS 4
static uint16_t default_sync_intervals_h[MAX_TIME_SYNC_INTERVALS] = { 2, 4, 8, 12 };
static struct sid_time_sync_config default_time_sync_config = {
    .adaptive_sync_intervals_h = default_sync_intervals_h,
    .num_intervals = ARRAY_SIZE(default_sync_intervals_h),
};

K_SEM_DEFINE(sidewalk_event_sem, 0, 1);

static void refresh_usb_status(void)
{
    uint8_t device_state = BUTTERFI_USB_DEVICE_STATE_IDLE;

    if (sidewalk_state == SIDEWALK_STATE_ERROR) {
        device_state = BUTTERFI_USB_DEVICE_STATE_ERROR;
    } else if (request_in_flight) {
        device_state = BUTTERFI_USB_DEVICE_STATE_BUSY;
    } else if (sidewalk_state == SIDEWALK_STATE_READY) {
        device_state = BUTTERFI_USB_DEVICE_STATE_SIDEWALK_READY;
    } else if (usb_ready) {
        device_state = BUTTERFI_USB_DEVICE_STATE_SIDEWALK_STARTING;
    }

    butterfi_usb_update_status(device_state,
                               current_link_state,
                               request_in_flight ? active_request_id : 0);
}

static void reset_transfer_state(void)
{
    request_in_flight = false;
    active_request_id = 0;
    active_total_chunks = 0;
    received_chunk_count = 0;
    memset(received_chunk_bitmap, 0, sizeof(received_chunk_bitmap));
    refresh_usb_status();
}

static void start_transfer_state(uint8_t request_id)
{
    active_request_id = request_id;
    request_in_flight = true;
    ignored_request_id = 0;
    active_total_chunks = 0;
    received_chunk_count = 0;
    memset(received_chunk_bitmap, 0, sizeof(received_chunk_bitmap));
    refresh_usb_status();
}

static bool mark_chunk_received(uint8_t chunk_idx)
{
    size_t byte_index = chunk_idx / 8;
    uint8_t bit_mask = BIT(chunk_idx % 8);
    bool already_received = (received_chunk_bitmap[byte_index] & bit_mask) != 0U;

    if (!already_received) {
        received_chunk_bitmap[byte_index] |= bit_mask;
        received_chunk_count++;
    }

    return !already_received;
}

static sid_error_t send_sidewalk_uplink(uint8_t msg_type,
                                        uint8_t request_id,
                                        const uint8_t *payload,
                                        size_t payload_len)
{
    uint8_t buffer[2 + BUTTERFI_SIDEWALK_UPLINK_MAX_PAYLOAD];
    struct sid_msg_desc desc = {
        .link_type = SID_LINK_TYPE_1,
        .type = SID_MSG_TYPE_NOTIFY,
        .link_mode = SID_LINK_MODE_CLOUD,
        .msg_desc_attr = {
            .tx_attr = {
                .request_ack = false,
                .num_retries = 0,
                .ttl_in_seconds = 30,
                .additional_attr = SID_MSG_DESC_TX_ADDITIONAL_ATTRIBUTES_NONE,
            },
        },
    };
    struct sid_msg msg = {
        .data = buffer,
        .size = 2 + payload_len,
    };

    if (payload_len > BUTTERFI_SIDEWALK_UPLINK_MAX_PAYLOAD) {
        return SID_ERROR_PARAM_OUT_OF_RANGE;
    }

    buffer[0] = msg_type;
    buffer[1] = request_id;

    if (payload_len > 0 && payload != NULL) {
        memcpy(&buffer[2], payload, payload_len);
    }

    return sid_put_msg(sid_handle, &msg, &desc);
}

static void handle_host_frame(uint8_t frame_type,
                              uint8_t request_id,
                              const uint8_t *payload,
                              uint16_t payload_len,
                              void *context)
{
    sid_error_t sid_err;

    ARG_UNUSED(context);

    switch (frame_type) {
    case BUTTERFI_USB_FRAME_HOST_STATUS_REQUEST:
        (void)butterfi_usb_send_status();
        break;

    case BUTTERFI_USB_FRAME_HOST_PING:
        (void)butterfi_usb_send_pong(request_id, payload, payload_len);
        break;

    case BUTTERFI_USB_FRAME_HOST_QUERY_SUBMIT:
        if (payload_len == 0) {
            (void)butterfi_usb_send_transfer_error(request_id,
                                                   BUTTERFI_USB_ERROR_PROTOCOL_MISMATCH,
                                                   "query payload required");
            break;
        }

        if (request_in_flight) {
            (void)butterfi_usb_send_transfer_error(request_id,
                                                   BUTTERFI_USB_ERROR_DEVICE_BUSY,
                                                   "request already active");
            break;
        }

        if (sidewalk_state != SIDEWALK_STATE_READY || sid_handle == NULL) {
            (void)butterfi_usb_send_transfer_error(request_id,
                                                   BUTTERFI_USB_ERROR_SIDEWALK_UNAVAILABLE,
                                                   "sidewalk not ready");
            break;
        }

        sid_err = send_sidewalk_uplink(BUTTERFI_SIDEWALK_MSG_QUERY,
                                       request_id,
                                       payload,
                                       payload_len);
        if (sid_err != SID_ERROR_NONE) {
            LOG_ERR("Query uplink failed: %d", sid_err);
            (void)butterfi_usb_send_transfer_error(request_id,
                                                   BUTTERFI_USB_ERROR_CLOUD_FETCH_FAILED,
                                                   "query uplink failed");
            break;
        }

        start_transfer_state(request_id);
        current_led_state = LED_STATE_SENDING;
        (void)butterfi_usb_send_uplink_accepted(request_id);
        break;

    case BUTTERFI_USB_FRAME_HOST_RESEND_REQUEST:
        if (payload_len != 1 || !request_in_flight || request_id != active_request_id) {
            (void)butterfi_usb_send_transfer_error(request_id,
                                                   BUTTERFI_USB_ERROR_PROTOCOL_MISMATCH,
                                                   "invalid resend request");
            break;
        }

        if (sidewalk_state != SIDEWALK_STATE_READY || sid_handle == NULL) {
            (void)butterfi_usb_send_transfer_error(request_id,
                                                   BUTTERFI_USB_ERROR_SIDEWALK_UNAVAILABLE,
                                                   "sidewalk not ready");
            break;
        }

        sid_err = send_sidewalk_uplink(BUTTERFI_SIDEWALK_MSG_RESEND,
                                       request_id,
                                       payload,
                                       payload_len);
        if (sid_err != SID_ERROR_NONE) {
            LOG_ERR("Resend uplink failed: %d", sid_err);
            (void)butterfi_usb_send_transfer_error(request_id,
                                                   BUTTERFI_USB_ERROR_CLOUD_FETCH_FAILED,
                                                   "resend uplink failed");
            break;
        }

        (void)butterfi_usb_send_uplink_accepted(request_id);
        break;

    case BUTTERFI_USB_FRAME_HOST_CANCEL_REQUEST:
        if (request_in_flight && request_id == active_request_id) {
            ignored_request_id = request_id;
            reset_transfer_state();
            current_led_state = (sidewalk_state == SIDEWALK_STATE_READY)
                ? LED_STATE_CONNECTED
                : LED_STATE_CONNECTING;
        } else {
            (void)butterfi_usb_send_transfer_error(request_id,
                                                   BUTTERFI_USB_ERROR_PROTOCOL_MISMATCH,
                                                   "request not active");
        }
        break;

    default:
        (void)butterfi_usb_send_transfer_error(request_id,
                                               BUTTERFI_USB_ERROR_PROTOCOL_MISMATCH,
                                               "unsupported host frame");
        break;
    }
}

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
    const uint8_t *payload = msg->data;
    uint8_t request_id;
    uint8_t chunk_idx;
    uint8_t total_chunks;
    bool is_new_chunk;

    ARG_UNUSED(context);

    LOG_INF("MSG RX: type=%d, id=%u, size=%u",
            msg_desc->type, msg_desc->id, msg->size);

    if (payload == NULL || msg->size < 4 || payload[0] != BUTTERFI_SIDEWALK_MSG_RESPONSE_CHUNK) {
        LOG_WRN("Ignoring unexpected Sidewalk downlink payload");
        return;
    }

    request_id = payload[1];
    chunk_idx = payload[2];
    total_chunks = payload[3];

    if (total_chunks == 0 || chunk_idx >= total_chunks) {
        (void)butterfi_usb_send_transfer_error(request_id,
                                               BUTTERFI_USB_ERROR_PROTOCOL_MISMATCH,
                                               "invalid chunk metadata");
        return;
    }

    if (!request_in_flight) {
        if (request_id == ignored_request_id) {
            LOG_INF("Ignoring chunk for cancelled request %u", request_id);
            return;
        }

        start_transfer_state(request_id);
    }

    if (request_id != active_request_id) {
        LOG_WRN("Ignoring chunk for request %u while %u is active", request_id, active_request_id);
        return;
    }

    if (active_total_chunks == 0) {
        active_total_chunks = total_chunks;
    } else if (active_total_chunks != total_chunks) {
        (void)butterfi_usb_send_transfer_error(request_id,
                                               BUTTERFI_USB_ERROR_PROTOCOL_MISMATCH,
                                               "chunk total changed");
        reset_transfer_state();
        return;
    }

    is_new_chunk = mark_chunk_received(chunk_idx);
    current_led_state = LED_STATE_SENDING;
    (void)butterfi_usb_send_response_chunk(request_id, payload, msg->size);

    if (is_new_chunk && received_chunk_count == active_total_chunks) {
        (void)butterfi_usb_send_transfer_complete(request_id);
        current_led_state = LED_STATE_CONNECTED;
        reset_transfer_state();
    }
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
    ARG_UNUSED(context);

    LOG_ERR("MSG TX ERR: %d (id=%u)", error, msg_desc->id);
    current_led_state = LED_STATE_CONNECTED;

    if (request_in_flight) {
        (void)butterfi_usb_send_transfer_error(active_request_id,
                                               BUTTERFI_USB_ERROR_CLOUD_FETCH_FAILED,
                                               "sidewalk transmission failed");
        reset_transfer_state();
    }
}

static void on_status_changed(const struct sid_status *status, void *context)
{
    LOG_INF("Sidewalk status: state=%d, reg=%d",
            status->state, status->detail.registration_status);

    switch (status->state) {
    case SID_STATE_READY:
    case SID_STATE_SECURE_CHANNEL_READY:
        sidewalk_state = SIDEWALK_STATE_READY;
        current_led_state = LED_STATE_CONNECTED;
        LOG_INF("Sidewalk READY — ButterFi online");
        break;

    case SID_STATE_NOT_READY:
        sidewalk_state = SIDEWALK_STATE_INIT;
        current_led_state = LED_STATE_CONNECTING;
        break;

    case SID_STATE_ERROR:
        sidewalk_state = SIDEWALK_STATE_ERROR;
        current_led_state = LED_STATE_ERROR;
        LOG_ERR("Sidewalk ERROR");
        break;
    }

    current_link_state = (status->detail.link_status_mask & SID_LINK_TYPE_1)
        ? BUTTERFI_USB_LINK_BLE
        : BUTTERFI_USB_LINK_UNKNOWN;

    if (status->detail.registration_status == SID_STATUS_NOT_REGISTERED) {
        sidewalk_state = SIDEWALK_STATE_NOT_REGISTERED;
        current_led_state = LED_STATE_UNPROVISIONED;
        LOG_WRN("Device not registered — run provisioning tool");
    }

    refresh_usb_status();
}

static void on_factory_reset(void *context)
{
    LOG_WRN("Factory reset triggered via Sidewalk");
    butterfi_config_clear();
    reset_transfer_state();
    sys_reboot(SYS_REBOOT_COLD);
}

static struct sid_event_callbacks sidewalk_callbacks = {
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
    struct sid_end_device_characteristics dev_ch = {
        .type = SID_END_DEVICE_TYPE_STATIC,
        .power_type = SID_END_DEVICE_POWERED_BY_BATTERY_AND_LINE_POWER,
        .qualification_id = 0x0001,
    };

    struct sid_config config = {
        .link_mask = SID_LINK_TYPE_1,
        .dev_ch = dev_ch,
        .callbacks = &sidewalk_callbacks,
        .link_config = app_get_ble_config(),
        .sub_ghz_link_config = NULL,
        .log_config = NULL,
        .time_sync_config = &default_time_sync_config,
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

    LOG_INF("ButterFi v%s starting", BUTTERFI_VERSION);

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
    ret = butterfi_usb_init(handle_host_frame, NULL);
    if (ret < 0) {
        LOG_ERR("USB init failed: %d", ret);
    } else {
        usb_ready = true;
        refresh_usb_status();
    }

    /* Sidewalk stack */
    ret = sidewalk_init();
    if (ret < 0) {
        current_led_state = LED_STATE_ERROR;
        refresh_usb_status();
        LOG_ERR("Sidewalk init failed — halting");
        return ret;
    }

    current_led_state = LED_STATE_CONNECTING;
    refresh_usb_status();

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
