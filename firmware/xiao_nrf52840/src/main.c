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
#include <hal/nrf_clock.h>

#include <string.h>
#include <stdio.h>

/* Guard against the build-flag footgun: BUTTERFI_USB_CONTROL_DEBUG defaults ON
 * and its branch in main() precedes the Sidewalk branch, so a build that sets
 * BUTTERFI_INCLUDE_SIDEWALK=ON but forgets BUTTERFI_USB_CONTROL_DEBUG=OFF
 * silently produces Sidewalk-free firmware that still enumerates over USB. */
#if BUTTERFI_USB_CONTROL_DEBUG && BUTTERFI_INCLUDE_SIDEWALK
#error "Set BUTTERFI_USB_CONTROL_DEBUG=OFF when BUTTERFI_INCLUDE_SIDEWALK=ON; the USB-control-debug branch takes precedence and skips sidewalk_init()."
#endif

#if BUTTERFI_INCLUDE_SIDEWALK
#include <pm_config.h>
#include <zephyr/storage/flash_map.h>

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

/* Bump to force a one-time settings_storage wipe on the next boot (see the
 * maintenance block in main()). Currently 1: wipe the foreign NVS left behind
 * when butterfi_config used to share flash with settings_storage. */
#define BUTTERFI_SETTINGS_MAINT_GEN 1

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

/* One-line boot summary emitted over USB frame 0x87 (empty until populated in
 * main()). Surfaces the actual LFCLK source — if it reads RC while the build
 * configures Xtal/50ppm, the crystal isn't running and the controller is
 * advertising the wrong SCA to the gateway — plus the resolved build flags so
 * a Sidewalk-free build can't masquerade as a live one. */
static char boot_info[80];
static int boot_info_emits_remaining = 20;

static void populate_boot_info(void)
{
    uint32_t lfstat = NRF_CLOCK->LFCLKSTAT;
    uint32_t src = (lfstat & CLOCK_LFCLKSTAT_SRC_Msk) >> CLOCK_LFCLKSTAT_SRC_Pos;
    bool running = (lfstat & CLOCK_LFCLKSTAT_STATE_Msk) != 0U;
    const char *src_str = (src == 0U) ? "RC" : (src == 1U) ? "Xtal"
                        : (src == 2U) ? "Synth" : "?";
    uint32_t maint_gen = 0;
    int gen_ret = butterfi_config_get_maint_gen(&maint_gen);

    /* gen should read back as BUTTERFI_SETTINGS_MAINT_GEN on every boot after
     * the first. gen=0 persisting across boots means the marker is not
     * sticking (the settings wipe would then re-run each boot). gen=err means
     * NVS did not mount. */
    (void)snprintk(boot_info, sizeof(boot_info),
                   "BOOT lfclk=%s,%s SW=%d DBG=%d gen=%s%u",
                   src_str, running ? "run" : "STOPPED",
                   (int)BUTTERFI_INCLUDE_SIDEWALK,
                   (int)BUTTERFI_USB_CONTROL_DEBUG,
                   (gen_ret == 0) ? "" : "err", (gen_ret == 0) ? maint_gen : 0);
}

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

/* Reconnect strategy — two distinct cases, because they need different fixes:
 *
 *  1. Idle drop AFTER the device has connected once (was_ready_once). Per
 *     sid_api.h the BLE beacon "please connect to me" flag must be re-set after
 *     each dropped connection; the auto-connect policy only arms it once, so an
 *     idle-dropped FFN link never comes back on its own. We re-arm it on the
 *     drop and every BUTTERFI_RECONNECT_REARM_INTERVAL_MS while down. This works
 *     ONLY post-connect: sid_ble_bcn_connection_request returns
 *     SID_ERROR_INVALID_STATE before the device has time-synced.
 *
 *  2. The device NEVER completes its first connect (booted with no gateway in
 *     range). The beacon request can't help (INVALID_STATE) and the auto-connect
 *     policy doesn't retry, so it hangs on "Sidewalk starting" forever until a
 *     power cycle — fatal for a dongle that boots at a student's home. The only
 *     thing that recovers it is a fresh sid_start, so a watchdog reboots after
 *     BUTTERFI_INITIAL_CONNECT_REBOOT_MS (see maybe_reboot_if_initial_connect_stuck);
 *     it reconnects the instant a gateway becomes reachable. */
static bool was_ready_once;
static volatile bool reconnect_request_pending;
static int64_t next_reconnect_arm_ms;
#define BUTTERFI_RECONNECT_REARM_INTERVAL_MS 15000
/* Give the auto-connect policy a couple of minutes on the initial connect before
 * the watchdog reboots to retry from a fresh sid_start. */
#define BUTTERFI_INITIAL_CONNECT_REBOOT_MS 120000

#define BUTTERFI_SIDEWALK_MSG_QUERY 0x01
#define BUTTERFI_SIDEWALK_MSG_RESEND 0x02
#define BUTTERFI_SIDEWALK_MSG_ACK 0x03
#define BUTTERFI_SIDEWALK_MSG_RESPONSE_CHUNK 0x81
/* Sidewalk BLE caps a single message at 255 bytes; our uplink adds a 2-byte
 * header (type + request id), so the app payload must stay <= 253. A larger
 * value would let the stack reject the message rather than our bounds check. */
#define BUTTERFI_SIDEWALK_UPLINK_MAX_PAYLOAD 253

#define MAX_TIME_SYNC_INTERVALS 4
static uint16_t default_sync_intervals_h[MAX_TIME_SYNC_INTERVALS] = { 2, 4, 8, 12 };
static struct sid_time_sync_config default_time_sync_config = {
    .adaptive_sync_intervals_h = default_sync_intervals_h,
    .num_intervals = ARRAY_SIZE(default_sync_intervals_h),
};

K_SEM_DEFINE(sidewalk_event_sem, 0, 1);

/* Manufacturing-page (Sidewalk credential) write buffer. The UF2 bootloader
 * on this hardware only accepts writes to its single known app region, so
 * mfg_storage cannot be provisioned by merging it into a UF2 (see
 * docs/hardware-validation-checklist.md). Instead the browser writes it here
 * over the already-running USB runtime protocol. */
static uint8_t mfg_write_buffer[PM_MFG_STORAGE_SIZE];
static uint16_t mfg_write_received_len;

static int write_mfg_storage(const uint8_t *data, size_t len)
{
    const struct flash_area *fa;
    int rc;

    rc = flash_area_open(PM_MFG_STORAGE_ID, &fa);
    if (rc != 0) {
        return rc;
    }

    rc = flash_area_erase(fa, 0, fa->fa_size);
    if (rc == 0) {
        rc = flash_area_write(fa, 0, data, len);
    }

    flash_area_close(fa);
    return rc;
}
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

    case BUTTERFI_USB_FRAME_HOST_MFG_WRITE: {
#if BUTTERFI_INCLUDE_SIDEWALK
        uint16_t chunk_offset;
        uint16_t total_len;
        uint16_t chunk_len;

        if (payload_len < 4) {
            (void)butterfi_usb_send_transfer_error(request_id,
                                                   BUTTERFI_USB_ERROR_INVALID_HOST_FRAME,
                                                   "mfg write frame too short");
            break;
        }

        chunk_offset = payload[0] | (payload[1] << 8);
        total_len = payload[2] | (payload[3] << 8);
        chunk_len = payload_len - 4;

        if (chunk_offset == 0) {
            mfg_write_received_len = 0;
        }

        if (total_len == 0 || total_len > sizeof(mfg_write_buffer) ||
            chunk_offset != mfg_write_received_len ||
            (uint32_t)chunk_offset + chunk_len > total_len) {
            (void)butterfi_usb_send_transfer_error(request_id,
                                                   BUTTERFI_USB_ERROR_INVALID_HOST_FRAME,
                                                   "mfg write out of sequence");
            mfg_write_received_len = 0;
            break;
        }

        memcpy(&mfg_write_buffer[chunk_offset], &payload[4], chunk_len);
        mfg_write_received_len += chunk_len;

        if (mfg_write_received_len < total_len) {
            (void)butterfi_usb_send_uplink_accepted(request_id);
            break;
        }

        mfg_write_received_len = 0;

        if (write_mfg_storage(mfg_write_buffer, total_len) != 0) {
            (void)butterfi_usb_send_transfer_error(request_id,
                                                   BUTTERFI_USB_ERROR_MFG_WRITE_FAILED,
                                                   "flash write failed");
            break;
        }

        (void)butterfi_usb_send_mfg_write_ok(request_id, "mfg storage written");
        LOG_INF("Sidewalk mfg_storage written (%u bytes); rebooting", total_len);
        /* USB TX is asynchronous (drains from a ring buffer via UART IRQ),
         * so give the ok frame above time to actually go out over the wire
         * before resetting - otherwise the host never sees it. */
        k_msleep(150);
        sys_reboot(SYS_REBOOT_COLD);
        break;
#else
        (void)butterfi_usb_send_transfer_error(request_id,
                                               BUTTERFI_USB_ERROR_SIDEWALK_UNAVAILABLE,
                                               "sidewalk not built in");
        break;
#endif
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
    char dbg[64];

    ARG_UNUSED(context);

    LOG_ERR("MSG TX ERR: %d (id=%u)", error, msg_desc->id);
    (void)snprintk(dbg, sizeof(dbg), "SID send err=%d id=%u", error, msg_desc->id);
    (void)butterfi_usb_send_debug_text(dbg);
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
    char dbg[96];

    LOG_INF("Sidewalk status: state=%d, reg=%d, time=%d, link_mask=0x%08x",
            status->state,
            status->detail.registration_status,
            status->detail.time_sync_status,
            status->detail.link_status_mask);

    /* Mirror the Sidewalk status detail out over USB so it can be observed
     * without a debug probe (the Zephyr console is disabled on this build). */
    (void)snprintk(dbg, sizeof(dbg),
                   "SID state=%d reg=%d time=%d linkmask=0x%08x",
                   status->state,
                   status->detail.registration_status,
                   status->detail.time_sync_status,
                   status->detail.link_status_mask);
    (void)butterfi_usb_send_debug_text(dbg);

    switch (status->state) {
    case SID_STATE_READY:
    case SID_STATE_SECURE_CHANNEL_READY:
        sidewalk_state = SIDEWALK_STATE_READY;
        current_led_state = LED_STATE_CONNECTED;
        was_ready_once = true;
        reconnect_request_pending = false;
        LOG_INF("Sidewalk READY — ButterFi online");
        break;

    case SID_STATE_NOT_READY:
        sidewalk_state = SIDEWALK_STATE_INIT;
        current_led_state = LED_STATE_CONNECTING;
        /* Link dropped after being up — flag a beacon connect-request re-arm.
         * Deferred to the main loop: sid_* APIs must not be called from inside
         * this callback, which runs re-entrantly within sid_process(). */
        if (was_ready_once) {
            reconnect_request_pending = true;
        }
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

    /* The Sidewalk stack retains the pointer passed to sid_init(), so this
     * config (like the callbacks it references) must live in static storage,
     * not on sidewalk_init()'s stack — otherwise it is a use-after-scope once
     * this function returns, which manifests as the stack silently doing
     * nothing (no crash, no error). Assigned at runtime because
     * app_get_ble_config() is not a constant initializer. */
    static struct sid_config config;

    config.link_mask = SID_LINK_TYPE_1;
    config.dev_ch = dev_ch;
    config.callbacks = &sidewalk_callbacks;
    config.link_config = app_get_ble_config();
    config.sub_ghz_link_config = NULL;
    config.log_config = NULL;
    config.time_sync_config = &default_time_sync_config;

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

    /* Ask the stack to actively establish and hold a BLE gateway connection.
     * Without an auto-connect policy the device only passively accepts brief
     * gateway connections, which drop before time sync / registration can
     * complete — so it never leaves SID_STATUS_NOT_REGISTERED. This mirrors
     * the CONFIG_SID_END_DEVICE_AUTO_CONN_REQ path in the Nordic
     * sid_end_device reference sample. */
    {
        enum sid_link_connection_policy conn_policy =
            SID_LINK_CONNECTION_POLICY_AUTO_CONNECT;
        struct sid_link_auto_connect_params ac_params = {
            .link_type = SID_LINK_TYPE_1,
            .enable = true,
            .priority = 0,
            .connection_attempt_timeout_seconds = 30,
        };

        err = sid_option(sid_handle, SID_OPTION_SET_LINK_CONNECTION_POLICY,
                         &conn_policy, sizeof(conn_policy));
        if (err != SID_ERROR_NONE) {
            LOG_ERR("set connection policy failed: %d", err);
        }

        err = sid_option(sid_handle, SID_OPTION_SET_LINK_POLICY_AUTO_CONNECT_PARAMS,
                         &ac_params, sizeof(ac_params));
        if (err != SID_ERROR_NONE) {
            LOG_ERR("set auto-connect params failed: %d", err);
        }
    }

    LOG_INF("Sidewalk stack started");
    return 0;
}

/* Re-arm the BLE beacon connection request after a post-connect idle drop.
 * Called from the main loop (never a Sidewalk callback). Gated on
 * was_ready_once: the request needs a prior time-sync, so before the first
 * successful connect it returns SID_ERROR_INVALID_STATE and is useless — that
 * case is handled by the reboot watchdog instead. Scoped to
 * SIDEWALK_STATE_INIT (connecting / NOT_READY) so it never fires during
 * registration, faults, or once connected. A post-READY drop sets
 * reconnect_request_pending for a prompt re-arm; otherwise it re-issues every
 * BUTTERFI_RECONNECT_REARM_INTERVAL_MS until a gateway answers. */
static void maybe_rearm_connection_request(void)
{
    int64_t now;
    sid_error_t err;

    if (sid_handle == NULL || !was_ready_once) {
        return;
    }

    /* Only while connecting (INIT) — not registration issues, faults, or READY. */
    if (sidewalk_state != SIDEWALK_STATE_INIT) {
        return;
    }

    now = k_uptime_get();
    if (!reconnect_request_pending && now < next_reconnect_arm_ms) {
        return;
    }

    reconnect_request_pending = false;
    next_reconnect_arm_ms = now + BUTTERFI_RECONNECT_REARM_INTERVAL_MS;

    err = sid_ble_bcn_connection_request(sid_handle, true);
    /* ALREADY_EXISTS just means a connection came up in the meantime — benign. */
    if (err == SID_ERROR_NONE || err == SID_ERROR_ALREADY_EXISTS) {
        (void)butterfi_usb_send_debug_text("reconn req armed");
    } else {
        char dbg[48];

        (void)snprintk(dbg, sizeof(dbg), "reconn req err=%d", err);
        (void)butterfi_usb_send_debug_text(dbg);
    }
}

/* Watchdog for a device that never completed its FIRST connect. The beacon
 * re-arm above can't help pre-time-sync, and the auto-connect policy doesn't
 * retry, so without this the device hangs on "Sidewalk starting" forever after
 * booting with no gateway in range. A fresh sid_start is the only recovery, so
 * reboot once we've been stuck-and-never-ready for BUTTERFI_INITIAL_CONNECT_REBOOT_MS.
 * Only fires before the first successful connect (was_ready_once == false), so a
 * device that has ever connected is left to the beacon re-arm and never reboots. */
static void maybe_reboot_if_initial_connect_stuck(void)
{
    if (was_ready_once) {
        return;
    }
    if (sidewalk_state != SIDEWALK_STATE_INIT) {
        return;  /* not connecting (e.g. NOT_REGISTERED / ERROR) — don't loop-reboot */
    }
    if (k_uptime_get() < BUTTERFI_INITIAL_CONNECT_REBOOT_MS) {
        return;
    }
    LOG_WRN("Initial Sidewalk connect stuck — rebooting to retry sid_start");
    (void)butterfi_usb_send_debug_text("initial connect stuck; rebooting");
    k_msleep(150);  /* let the debug frame drain over USB before reset */
    sys_reboot(SYS_REBOOT_COLD);
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
/* This thread now only emits the 1 Hz status + boot-info frames — it no longer
 * polls or handles host frames (the main thread is the sole poller). Its peak
 * is one send_frame (~519B frame buffer) + call overhead, so 1024 is ample.
 * The big config-save/query buffers that once overflowed 1024 here now live on
 * the main thread's stack instead (CONFIG_MAIN_STACK_SIZE, bumped to 8192).
 * THREAD_ANALYZER reports the real high-water marks within 30s of boot. */
#define USB_STACK_SIZE 1024
#define USB_PRIORITY   -1

static void usb_thread_fn(void *a, void *b, void *c)
{
    int64_t next_usb_status_ms = 0;

    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    while (1) {
        /* NOTE: this thread does NOT call butterfi_usb_poll(). RX parsing and
         * host-frame handling run only on the main thread (see the Sidewalk
         * loop / run_usb_control_loop), so there is a single owner of the RX
         * ring and parser state — and so host frames that issue Sidewalk API
         * calls run in the same context as sid_process(). This thread only
         * emits periodic TX (serialized by tx_mutex in butterfi_usb.c). */
        if (usb_ready && k_uptime_get() >= next_usb_status_ms) {
            (void)butterfi_usb_send_status();
            /* Emit the boot summary only for the first ~20s so a watcher that
             * attaches shortly after boot still catches it, without flooding
             * the debug channel during later protocol tests. */
            if (boot_info[0] != '\0' && boot_info_emits_remaining > 0) {
                (void)butterfi_usb_send_debug_text(boot_info);
                boot_info_emits_remaining--;
            }
            next_usb_status_ms = k_uptime_get() + 1000;
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

#if BUTTERFI_INCLUDE_SIDEWALK
    /* One-time maintenance: earlier firmware mounted butterfi_config's NVS on
     * the DTS storage_partition, which physically overlaps settings_storage
     * (where the Sidewalk stack persists its registration/time-sync key
     * store). That left a foreign NVS filesystem inside settings_storage that
     * the Sidewalk NVS still mounts and reads as stale/garbage state. Wipe it
     * once — before sidewalk_init() so Sidewalk mounts a clean store — then
     * never again. Runs pre-radio, so no flash/radio contention here.
     *
     * Fail closed: record the new generation FIRST and verify it stuck; only
     * then wipe. A wipe we cannot record is a wipe we must not perform — the
     * alternative is a unit that silently erases the Sidewalk key store on
     * every boot and can never register. */
    {
        uint32_t maint_gen = 0;

        if (butterfi_config_get_maint_gen(&maint_gen) == 0 &&
            maint_gen < BUTTERFI_SETTINGS_MAINT_GEN) {
            uint32_t check = 0;

            if (butterfi_config_set_maint_gen(BUTTERFI_SETTINGS_MAINT_GEN) == 0 &&
                butterfi_config_get_maint_gen(&check) == 0 &&
                check == BUTTERFI_SETTINGS_MAINT_GEN) {
                const struct flash_area *fa;

                if (flash_area_open(PM_SETTINGS_STORAGE_ID, &fa) == 0) {
                    int erase_ret = flash_area_erase(fa, 0, fa->fa_size);

                    flash_area_close(fa);
                    LOG_WRN("Wiped stale settings_storage (gen %u->%u): %d",
                            maint_gen, BUTTERFI_SETTINGS_MAINT_GEN, erase_ret);
                }
            } else {
                LOG_ERR("Skipping settings_storage wipe: cannot persist marker");
            }
        }
    }
#endif

    /* Capture LFCLK source + build flags + the maintenance generation for the
     * 0x87 boot summary. Done AFTER the wipe so gen reflects the persisted
     * marker — if it ever reads back 0 on a later boot, the marker is not
     * sticking (the fail-open wipe hazard). */
    populate_boot_info();

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

        maybe_rearm_connection_request();
        maybe_reboot_if_initial_connect_stuck();
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
