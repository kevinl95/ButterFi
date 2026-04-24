#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>
#include <string.h>

#include "butterfi_usb.h"

LOG_MODULE_REGISTER(butterfi_usb, LOG_LEVEL_INF);

#define CDC_DEV_NAME "CDC_ACM_0"

#define BUTTERFI_USB_SYNC_1 0x42
#define BUTTERFI_USB_SYNC_2 0x46
#define BUTTERFI_USB_VERSION 0x01
#define BUTTERFI_USB_HEADER_SIZE 8
#define BUTTERFI_USB_MAX_PAYLOAD 512
#define BUTTERFI_USB_MAX_FRAME_SIZE (BUTTERFI_USB_HEADER_SIZE + BUTTERFI_USB_MAX_PAYLOAD + 1)

static const struct device *cdc_dev;
static butterfi_usb_host_frame_handler_t host_frame_handler;
static void *host_frame_context;

static struct {
    uint8_t device_state;
    uint8_t link_state;
    uint8_t active_request_id;
} cached_status = {
    .device_state = BUTTERFI_USB_DEVICE_STATE_IDLE,
    .link_state = BUTTERFI_USB_LINK_UNKNOWN,
    .active_request_id = 0,
};

static uint8_t rx_frame[BUTTERFI_USB_MAX_FRAME_SIZE];
static size_t rx_frame_len;
static size_t expected_frame_len;
static bool last_dtr_asserted;

static uint8_t frame_checksum(const uint8_t *frame, size_t frame_len)
{
    uint8_t checksum = 0;

    for (size_t index = 2; index + 1 < frame_len; index++) {
        checksum ^= frame[index];
    }

    return checksum;
}

static void parser_reset(void)
{
    rx_frame_len = 0;
    expected_frame_len = 0;
}

static int write_bytes(const uint8_t *data, size_t len)
{
    if (!cdc_dev || !device_is_ready(cdc_dev)) {
        return -ENODEV;
    }

    for (size_t index = 0; index < len; index++) {
        uart_poll_out(cdc_dev, data[index]);
    }

    return 0;
}

static int send_frame(uint8_t frame_type,
                      uint8_t request_id,
                      const uint8_t *payload,
                      uint16_t payload_len,
                      uint8_t flags)
{
    uint8_t frame[BUTTERFI_USB_MAX_FRAME_SIZE];
    size_t frame_len;

    if (payload_len > BUTTERFI_USB_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }

    frame[0] = BUTTERFI_USB_SYNC_1;
    frame[1] = BUTTERFI_USB_SYNC_2;
    frame[2] = BUTTERFI_USB_VERSION;
    frame[3] = frame_type;
    frame[4] = request_id;
    frame[5] = flags;
    frame[6] = payload_len & 0xff;
    frame[7] = (payload_len >> 8) & 0xff;

    if (payload_len > 0 && payload != NULL) {
        memcpy(&frame[8], payload, payload_len);
    }

    frame_len = BUTTERFI_USB_HEADER_SIZE + payload_len + 1;
    frame[frame_len - 1] = frame_checksum(frame, frame_len);

    return write_bytes(frame, frame_len);
}

void butterfi_usb_update_status(uint8_t device_state,
                                uint8_t link_state,
                                uint8_t active_request_id)
{
    cached_status.device_state = device_state;
    cached_status.link_state = link_state;
    cached_status.active_request_id = active_request_id;

    (void)butterfi_usb_send_status();
}

int butterfi_usb_send_status(void)
{
    uint8_t payload[3] = {
        cached_status.device_state,
        cached_status.link_state,
        cached_status.active_request_id,
    };

    return send_frame(BUTTERFI_USB_FRAME_DEVICE_STATUS, 0, payload, sizeof(payload), 0);
}

int butterfi_usb_send_uplink_accepted(uint8_t request_id)
{
    return send_frame(BUTTERFI_USB_FRAME_DEVICE_UPLINK_ACCEPTED, request_id, NULL, 0, 0);
}

int butterfi_usb_send_response_chunk(uint8_t request_id,
                                     const uint8_t *payload,
                                     uint16_t payload_len)
{
    return send_frame(BUTTERFI_USB_FRAME_DEVICE_RESPONSE_CHUNK,
                      request_id,
                      payload,
                      payload_len,
                      0);
}

int butterfi_usb_send_transfer_complete(uint8_t request_id)
{
    return send_frame(BUTTERFI_USB_FRAME_DEVICE_TRANSFER_COMPLETE, request_id, NULL, 0, 0);
}

int butterfi_usb_send_transfer_error(uint8_t request_id,
                                     uint8_t error_code,
                                     const char *message)
{
    uint8_t payload[1 + BUTTERFI_USB_MAX_PAYLOAD];
    size_t message_len = 0;

    payload[0] = error_code;

    if (message != NULL) {
        message_len = strnlen(message, BUTTERFI_USB_MAX_PAYLOAD - 1);
        memcpy(&payload[1], message, message_len);
    }

    return send_frame(BUTTERFI_USB_FRAME_DEVICE_TRANSFER_ERROR,
                      request_id,
                      payload,
                      1 + message_len,
                      0);
}

int butterfi_usb_send_pong(uint8_t request_id,
                           const uint8_t *payload,
                           uint16_t payload_len)
{
    return send_frame(BUTTERFI_USB_FRAME_DEVICE_PONG, request_id, payload, payload_len, 0);
}

static void maybe_emit_status_on_dtr(void)
{
    uint32_t dtr = 0;
    int ret;

    if (!cdc_dev || !device_is_ready(cdc_dev)) {
        return;
    }

    ret = uart_line_ctrl_get(cdc_dev, UART_LINE_CTRL_DTR, &dtr);
    if (ret < 0) {
        return;
    }

    if (dtr != 0U && !last_dtr_asserted) {
        (void)butterfi_usb_send_status();
    }

    last_dtr_asserted = (dtr != 0U);
}

static void handle_complete_frame(void)
{
    uint16_t payload_len;
    uint8_t checksum;

    payload_len = rx_frame[6] | (rx_frame[7] << 8);
    checksum = frame_checksum(rx_frame, expected_frame_len);

    if (checksum != rx_frame[expected_frame_len - 1]) {
        (void)butterfi_usb_send_transfer_error(rx_frame[4],
                                               BUTTERFI_USB_ERROR_INVALID_HOST_FRAME,
                                               "checksum mismatch");
        parser_reset();
        return;
    }

    if (host_frame_handler != NULL) {
        host_frame_handler(rx_frame[3],
                           rx_frame[4],
                           &rx_frame[8],
                           payload_len,
                           host_frame_context);
    }

    parser_reset();
}

static void parse_byte(uint8_t byte)
{
    if (rx_frame_len == 0 && byte != BUTTERFI_USB_SYNC_1) {
        return;
    }

    if (rx_frame_len == 1 && byte != BUTTERFI_USB_SYNC_2) {
        rx_frame_len = (byte == BUTTERFI_USB_SYNC_1) ? 1 : 0;
        rx_frame[0] = BUTTERFI_USB_SYNC_1;
        expected_frame_len = 0;
        return;
    }

    rx_frame[rx_frame_len++] = byte;

    if (rx_frame_len == BUTTERFI_USB_HEADER_SIZE) {
        uint16_t payload_len = rx_frame[6] | (rx_frame[7] << 8);

        if (rx_frame[2] != BUTTERFI_USB_VERSION || payload_len > BUTTERFI_USB_MAX_PAYLOAD) {
            (void)butterfi_usb_send_transfer_error(rx_frame[4],
                                                   BUTTERFI_USB_ERROR_INVALID_HOST_FRAME,
                                                   "invalid header");
            parser_reset();
            return;
        }

        expected_frame_len = BUTTERFI_USB_HEADER_SIZE + payload_len + 1;
    }

    if (expected_frame_len > 0 && rx_frame_len == expected_frame_len) {
        handle_complete_frame();
    }
}

void butterfi_usb_poll(void)
{
    unsigned char byte;

    maybe_emit_status_on_dtr();

    if (!cdc_dev || !device_is_ready(cdc_dev)) {
        return;
    }

    while (uart_poll_in(cdc_dev, &byte) == 0) {
        parse_byte(byte);
    }
}

int butterfi_usb_init(butterfi_usb_host_frame_handler_t handler, void *context)
{
    int ret;

    host_frame_handler = handler;
    host_frame_context = context;
    parser_reset();
    last_dtr_asserted = false;

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
    (void)butterfi_usb_send_status();
    return 0;
}
