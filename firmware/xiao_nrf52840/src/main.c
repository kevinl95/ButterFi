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
#include <zephyr/data/json.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#include <string.h>

#if BUTTERFI_INCLUDE_SIDEWALK
#include <pm_config.h>

#include <sid_api.h>
#include <sid_error.h>
#include <sid_pal_common_ifc.h>
#include <app_ble_config.h>
#endif

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

#define BUTTERFI_MAX_TRANSFER_CHUNKS 255

static bool usb_ready;
static uint8_t active_request_id;
static bool request_in_flight;
static uint8_t ignored_request_id;
static uint8_t active_total_chunks;
static size_t received_chunk_count;
static uint8_t received_chunk_bitmap[(BUTTERFI_MAX_TRANSFER_CHUNKS + 7) / 8];
static uint8_t current_link_state = BUTTERFI_USB_LINK_UNKNOWN;
static volatile uint8_t host_frame_led_ticks;
static volatile uint8_t usb_tx_ok_led_ticks;
static volatile uint8_t usb_tx_err_led_ticks;
static volatile uint8_t usb_rx_byte_led_ticks;
static volatile uint8_t usb_rx_error_led_ticks;
static struct butterfi_usb_diag_counters last_usb_diag_snapshot;
static volatile uint8_t boot_fingerprint_ticks = 40;

static void set_usb_diag_led(uint8_t *slot)
{
    host_frame_led_ticks = 0;
    usb_tx_ok_led_ticks = 0;
    usb_tx_err_led_ticks = 0;
    usb_rx_byte_led_ticks = 0;
    usb_rx_error_led_ticks = 0;
    *slot = 1;
}

#if BUTTERFI_INCLUDE_SIDEWALK
static struct sid_handle *sid_handle   = NULL;

#define BUTTERFI_SIDEWALK_MSG_QUERY 0x01
#define BUTTERFI_SIDEWALK_MSG_RESEND 0x02
#define BUTTERFI_SIDEWALK_MSG_ACK 0x03
#define BUTTERFI_SIDEWALK_MSG_RESPONSE_CHUNK 0x81
#define BUTTERFI_SIDEWALK_UPLINK_MAX_PAYLOAD 512

#define MAX_TIME_SYNC_INTERVALS 4
static uint16_t default_sync_intervals_h[MAX_TIME_SYNC_INTERVALS] = { 2, 4, 8, 12 };
static struct sid_time_sync_config default_time_sync_config = {
    .adaptive_sync_intervals_h = default_sync_intervals_h,
    .num_intervals = ARRAY_SIZE(default_sync_intervals_h),
};

K_SEM_DEFINE(sidewalk_event_sem, 0, 1);
#endif

static const char *sidewalk_unavailable_reason(void)
{
    if (sidewalk_state == SIDEWALK_STATE_NOT_REGISTERED) {
        return "sidewalk not registered; missing credential or mobile onboarding";
    }

    return "sidewalk not ready";
}

static void refresh_usb_status(void)
{
    uint8_t device_state = BUTTERFI_USB_DEVICE_STATE_IDLE;

    if (sidewalk_state == SIDEWALK_STATE_ERROR) {
        device_state = BUTTERFI_USB_DEVICE_STATE_ERROR;
    } else if (sidewalk_state == SIDEWALK_STATE_NOT_REGISTERED) {
        device_state = BUTTERFI_USB_DEVICE_STATE_SIDEWALK_NOT_REGISTERED;
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

#if BUTTERFI_INCLUDE_SIDEWALK
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
#endif

static void note_host_frame_seen(uint8_t frame_type, uint8_t request_id)
{
    set_usb_diag_led((uint8_t *)&host_frame_led_ticks);
    LOG_INF("USB host frame: type=0x%02x id=%u", frame_type, request_id);
}

struct butterfi_host_config_json {
    char school_id[BUTTERFI_SCHOOL_ID_MAX];
    char device_name[BUTTERFI_DEVICE_NAME_MAX];
    char content_pkg[BUTTERFI_CONTENT_PKG_MAX];
};

static const struct json_obj_descr butterfi_host_config_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct butterfi_host_config_json, school_id, JSON_TOK_STRING_BUF),
    JSON_OBJ_DESCR_PRIM(struct butterfi_host_config_json, device_name, JSON_TOK_STRING_BUF),
    JSON_OBJ_DESCR_PRIM(struct butterfi_host_config_json, content_pkg, JSON_TOK_STRING_BUF),
};

static int save_host_config_payload(const uint8_t *payload, uint16_t payload_len)
{
    char json_buffer[513];
    struct butterfi_host_config_json host_cfg = { 0 };
    butterfi_config_t cfg = { 0 };
    int64_t decoded;

    if (payload == NULL || payload_len == 0 || payload_len >= sizeof(json_buffer)) {
        return -EMSGSIZE;
    }

    memcpy(json_buffer, payload, payload_len);
    json_buffer[payload_len] = '\0';

    decoded = json_obj_parse(json_buffer,
                             payload_len,
                             butterfi_host_config_descr,
                             ARRAY_SIZE(butterfi_host_config_descr),
                             &host_cfg);
    if (decoded < 0) {
        return (int)decoded;
    }

    if ((decoded & (BIT(0) | BIT(2))) != (BIT(0) | BIT(2))) {
        return -EINVAL;
    }

    if (host_cfg.school_id[0] == '\0' || host_cfg.content_pkg[0] == '\0') {
        return -EINVAL;
    }

    strncpy(cfg.school_id, host_cfg.school_id, sizeof(cfg.school_id) - 1);
    strncpy(cfg.content_pkg, host_cfg.content_pkg, sizeof(cfg.content_pkg) - 1);

    if (host_cfg.device_name[0] != '\0') {
        strncpy(cfg.device_name, host_cfg.device_name, sizeof(cfg.device_name) - 1);
    } else {
        strncpy(cfg.device_name, "ButterFi-Dongle", sizeof(cfg.device_name) - 1);
    }

    return butterfi_config_save(&cfg);
}

static void note_usb_tx_result(int ret, const char *label)
{
    if (ret == 0) {
        set_usb_diag_led((uint8_t *)&usb_tx_ok_led_ticks);
        LOG_INF("USB TX ok: %s", label);
    } else {
        set_usb_diag_led((uint8_t *)&usb_tx_err_led_ticks);
        LOG_WRN("USB TX failed (%d): %s", ret, label);
    }
}

static void poll_usb_diag_counters(void)
{
    struct butterfi_usb_diag_counters diag;

    butterfi_usb_get_diag_counters(&diag);

    if (diag.rx_bytes != last_usb_diag_snapshot.rx_bytes) {
        set_usb_diag_led((uint8_t *)&usb_rx_byte_led_ticks);
    }

    if (diag.rx_errors != last_usb_diag_snapshot.rx_errors) {
        set_usb_diag_led((uint8_t *)&usb_rx_error_led_ticks);
    }

    last_usb_diag_snapshot = diag;
}

static void handle_host_frame(uint8_t frame_type,
                              uint8_t request_id,
                              const uint8_t *payload,
                              uint16_t payload_len,
                              void *context)
{
#if BUTTERFI_INCLUDE_SIDEWALK
    sid_error_t sid_err;
#endif

    ARG_UNUSED(context);

    note_host_frame_seen(frame_type, request_id);

    switch (frame_type) {
    case BUTTERFI_USB_FRAME_HOST_STATUS_REQUEST:
        note_usb_tx_result(butterfi_usb_send_status(), "status");
        break;

    case BUTTERFI_USB_FRAME_HOST_CONFIG_SAVE: {
        int ret = save_host_config_payload(payload, payload_len);

        if (ret < 0) {
            uint8_t error_code = (ret == -EINVAL || ret == -EMSGSIZE)
                ? BUTTERFI_USB_ERROR_INVALID_HOST_FRAME
                : BUTTERFI_USB_ERROR_CONFIG_SAVE_FAILED;
            const char *message = (ret == -EINVAL || ret == -EMSGSIZE)
                ? "invalid config payload"
                : "config save failed";

            LOG_ERR("Config save failed: %d", ret);
            (void)butterfi_usb_send_transfer_error(request_id, error_code, message);
            break;
        }

        note_usb_tx_result(butterfi_usb_send_config_saved(request_id, "config saved"),
                           "config-saved");
        break;
    }

    case BUTTERFI_USB_FRAME_HOST_PING:
        note_usb_tx_result(butterfi_usb_send_pong(request_id, payload, payload_len), "pong");
        break;

    case BUTTERFI_USB_FRAME_HOST_QUERY_SUBMIT:
#if BUTTERFI_USB_CONTROL_DEBUG || !BUTTERFI_INCLUDE_SIDEWALK
        (void)butterfi_usb_send_transfer_error(request_id,
                                               BUTTERFI_USB_ERROR_SIDEWALK_UNAVAILABLE,
                                               "usb control debug build");
        break;
#else
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
                                                   sidewalk_unavailable_reason());
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
#endif

    case BUTTERFI_USB_FRAME_HOST_RESEND_REQUEST:
#if BUTTERFI_USB_CONTROL_DEBUG || !BUTTERFI_INCLUDE_SIDEWALK
        (void)butterfi_usb_send_transfer_error(request_id,
                                               BUTTERFI_USB_ERROR_SIDEWALK_UNAVAILABLE,
                                               "usb control debug build");
        break;
#else
        if (payload_len != 1 || !request_in_flight || request_id != active_request_id) {
            (void)butterfi_usb_send_transfer_error(request_id,
                                                   BUTTERFI_USB_ERROR_PROTOCOL_MISMATCH,
                                                   "invalid resend request");
            break;
        }

        if (sidewalk_state != SIDEWALK_STATE_READY || sid_handle == NULL) {
            (void)butterfi_usb_send_transfer_error(request_id,
                                                   BUTTERFI_USB_ERROR_SIDEWALK_UNAVAILABLE,
                                                   sidewalk_unavailable_reason());
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
#endif

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

#if BUTTERFI_INCLUDE_SIDEWALK
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
    LOG_INF("Sidewalk status: state=%d, reg=%d, time=%d, link_mask=0x%08x",
            status->state,
            status->detail.registration_status,
            status->detail.time_sync_status,
            status->detail.link_status_mask);

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
        LOG_WRN("Device not registered — missing Sidewalk credential or onboarding");
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

static int sidewalk_platform_init_once(void)
{
    static bool platform_ready;
    platform_parameters_t platform_parameters = {
        .mfg_store_region = {
            .addr_start = PM_MFG_STORAGE_ADDRESS,
            .addr_end = PM_MFG_STORAGE_END_ADDRESS,
        },
    };
    sid_error_t err;

    if (platform_ready) {
        return 0;
    }

    err = sid_platform_init(&platform_parameters);
    if (err != SID_ERROR_NONE) {
        LOG_ERR("sid_platform_init failed: %d", err);
        return -EFAULT;
    }

    platform_ready = true;
    return 0;
}

static int sidewalk_init(void)
{
    int ret;
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

    ret = sidewalk_platform_init_once();
    if (ret < 0) {
        return ret;
    }

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
#endif

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

        if (boot_fingerprint_ticks > 0U) {
            boot_fingerprint_ticks--;
            led_set(1, 1, 1);
            k_msleep(250);
            continue;
        }

        if (usb_rx_error_led_ticks > 0U) {
            led_set(1, 1, 0);
            k_msleep(250);
            continue;
        }

        if (usb_rx_byte_led_ticks > 0U) {
            led_set(0, 0, 1);
            k_msleep(250);
            continue;
        }

        if (usb_tx_err_led_ticks > 0U) {
            led_set(1, 0, 1);
            k_msleep(250);
            continue;
        }

        if (usb_tx_ok_led_ticks > 0U) {
            led_set(1, 1, 1);
            k_msleep(250);
            continue;
        }

        if (host_frame_led_ticks > 0U) {
            led_set(0, 1, 1);
            k_msleep(250);
            continue;
        }

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

/* ── USB service thread ─────────────────────────────────────────────────── */
#define USB_STACK_SIZE 1024
#define USB_PRIORITY   -1

static void usb_thread_fn(void *a, void *b, void *c)
{
    int64_t next_usb_status_ms = 0;

    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    while (1) {
        if (usb_ready) {
            butterfi_usb_poll();
            poll_usb_diag_counters();

            if (k_uptime_get() >= next_usb_status_ms) {
                (void)butterfi_usb_send_status();
                next_usb_status_ms = k_uptime_get() + 1000;
            }
        }

        k_msleep(20);
    }
}

K_THREAD_DEFINE(usb_tid, USB_STACK_SIZE,
                usb_thread_fn, NULL, NULL, NULL,
                USB_PRIORITY, 0, 0);

static void run_usb_control_loop(void)
{
    while (1) {
        if (usb_ready) {
            butterfi_usb_poll();
            poll_usb_diag_counters();
        }

        k_msleep(50);
    }
}

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

#if BUTTERFI_USB_CONTROL_DEBUG
    LOG_WRN("USB control debug build active - Sidewalk startup skipped");
    sidewalk_state = SIDEWALK_STATE_NOT_REGISTERED;
    current_led_state = LED_STATE_UNPROVISIONED;
    refresh_usb_status();

    run_usb_control_loop();
#elif BUTTERFI_INCLUDE_SIDEWALK
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
        if (usb_ready) {
            butterfi_usb_poll();
            poll_usb_diag_counters();
        }

        (void)k_sem_take(&sidewalk_event_sem, K_MSEC(100));

        if (sid_handle != NULL) {
            sid_error_t err = sid_process(sid_handle);
            if (err != SID_ERROR_NONE) {
                LOG_ERR("sid_process error: %d", err);
            }
        }
    }
#else
    LOG_WRN("Sidewalk excluded from build");
    sidewalk_state = SIDEWALK_STATE_NOT_REGISTERED;
    current_led_state = LED_STATE_UNPROVISIONED;
    refresh_usb_status();
    run_usb_control_loop();
#endif

    return 0;
}
