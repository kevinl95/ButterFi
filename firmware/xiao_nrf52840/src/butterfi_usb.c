#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/usb/usb_device.h>
#include <string.h>

#include "butterfi_usb.h"

LOG_MODULE_REGISTER(butterfi_usb, LOG_LEVEL_INF);

#define BUTTERFI_USB_SYNC_1 0x42
#define BUTTERFI_USB_SYNC_2 0x46
#define BUTTERFI_USB_VERSION 0x01
#define BUTTERFI_USB_HEADER_SIZE 8
#define BUTTERFI_USB_MAX_PAYLOAD 512
#define BUTTERFI_USB_MAX_FRAME_SIZE (BUTTERFI_USB_HEADER_SIZE + BUTTERFI_USB_MAX_PAYLOAD + 1)
#define BUTTERFI_USB_RX_RING_SIZE 1024
#define BUTTERFI_USB_TX_RING_SIZE 1024

static const struct device *const cdc_dev = DEVICE_DT_GET(DT_NODELABEL(board_cdc_acm_uart));
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
static bool host_session_active;
static bool rx_throttled;
static uint8_t rx_ring_buffer[BUTTERFI_USB_RX_RING_SIZE];
static struct ring_buf rx_ring;
static uint8_t tx_ring_buffer[BUTTERFI_USB_TX_RING_SIZE];
static struct ring_buf tx_ring;
static struct butterfi_usb_diag_counters usb_diag = {
    .last_tx_result = 0,
    .last_frame_type = 0,
};

static void parse_byte(uint8_t byte);

static bool host_ready_for_tx(void)
{
    return last_dtr_asserted || host_session_active;
}

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

static void cdc_interrupt_handler(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);

    while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
        if (!rx_throttled && uart_irq_rx_ready(dev)) {
            uint8_t buffer[64];
            size_t len = MIN(ring_buf_space_get(&rx_ring), sizeof(buffer));
            int recv_len;
#if !BUTTERFI_USB_CONTROL_DEBUG
            uint32_t stored_len;
#endif

            if (len == 0U) {
                uart_irq_rx_disable(dev);
                rx_throttled = true;
                continue;
            }

            recv_len = uart_fifo_read(dev, buffer, len);
            if (recv_len <= 0) {
                continue;
            }

#if BUTTERFI_USB_CONTROL_DEBUG
            usb_diag.rx_bytes += (uint32_t)recv_len;

            for (int index = 0; index < recv_len; index++) {
                parse_byte(buffer[index]);
            }
#else
            stored_len = ring_buf_put(&rx_ring, buffer, recv_len);
            if (stored_len < (uint32_t)recv_len) {
                LOG_WRN("Dropped %d USB bytes", recv_len - (int)stored_len);
            }

            usb_diag.rx_bytes += (uint32_t)stored_len;
#endif
        }

        if (uart_irq_tx_ready(dev)) {
            uint8_t buffer[64];
            uint32_t queued_len;
            int send_len;

            queued_len = ring_buf_get(&tx_ring, buffer, sizeof(buffer));
            if (queued_len == 0U) {
                uart_irq_tx_disable(dev);
                continue;
            }

            if (rx_throttled) {
                uart_irq_rx_enable(dev);
                rx_throttled = false;
            }

            send_len = uart_fifo_fill(dev, buffer, queued_len);
            if (send_len < 0) {
                LOG_WRN("Failed to write UART FIFO: %d", send_len);
                continue;
            }

            if ((uint32_t)send_len < queued_len) {
                uint32_t restored_len = ring_buf_put(&tx_ring,
                                                     &buffer[send_len],
                                                     queued_len - (uint32_t)send_len);

                if (restored_len < queued_len - (uint32_t)send_len) {
                    LOG_WRN("Dropped %u USB TX bytes",
                            queued_len - (uint32_t)send_len - restored_len);
                }
            }
        }
    }
}

static int write_bytes(const uint8_t *data, size_t len)
{
    uint32_t queued_len;

    if (!cdc_dev || !device_is_ready(cdc_dev)) {
        return -ENODEV;
    }

    if (!host_ready_for_tx()) {
        return -EAGAIN;
    }

    queued_len = ring_buf_put(&tx_ring, data, len);
    if (queued_len < len) {
        return -ENOSPC;
    }

    uart_irq_tx_enable(cdc_dev);

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

    usb_diag.last_tx_result = write_bytes(frame, frame_len);
    return usb_diag.last_tx_result;
}

void butterfi_usb_get_diag_counters(struct butterfi_usb_diag_counters *diag)
{
    unsigned int key;

    if (diag == NULL) {
        return;
    }

    key = irq_lock();
    *diag = usb_diag;
    irq_unlock(key);
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

int butterfi_usb_send_debug_text(const char *message)
{
    size_t message_len = 0;

    if (message != NULL) {
        message_len = strnlen(message, BUTTERFI_USB_MAX_PAYLOAD);
    }

    return send_frame(BUTTERFI_USB_FRAME_DEVICE_DEBUG_TEXT,
                      0,
                      (const uint8_t *)message,
                      (uint16_t)message_len,
                      0);
}

int butterfi_usb_send_config_saved(uint8_t request_id, const char *message)
{
    size_t message_len = 0;

    if (message != NULL) {
        message_len = strnlen(message, BUTTERFI_USB_MAX_PAYLOAD);
    }

    return send_frame(BUTTERFI_USB_FRAME_DEVICE_CONFIG_SAVED,
                      request_id,
                      (const uint8_t *)message,
                      (uint16_t)message_len,
                      0);
}

static void maybe_emit_status_on_dtr(void)
{
    uint32_t dtr = 0;
    bool dtr_asserted = false;

    if (!cdc_dev || !device_is_ready(cdc_dev)) {
        return;
    }

    if (uart_line_ctrl_get(cdc_dev, UART_LINE_CTRL_DTR, &dtr) == 0) {
        dtr_asserted = dtr != 0U;
    }

    if (dtr_asserted && !last_dtr_asserted) {
        host_session_active = true;
        (void)uart_line_ctrl_set(cdc_dev, UART_LINE_CTRL_DCD, 1);
        (void)uart_line_ctrl_set(cdc_dev, UART_LINE_CTRL_DSR, 1);
        (void)butterfi_usb_send_status();
    }

    if (!dtr_asserted && last_dtr_asserted) {
        host_session_active = false;
    }

    last_dtr_asserted = dtr_asserted;
}

static void handle_complete_frame(void)
{
    uint16_t payload_len;
    uint8_t checksum;

    payload_len = rx_frame[6] | (rx_frame[7] << 8);
    checksum = frame_checksum(rx_frame, expected_frame_len);

    if (checksum != rx_frame[expected_frame_len - 1]) {
        usb_diag.rx_errors++;
        (void)butterfi_usb_send_transfer_error(rx_frame[4],
                                               BUTTERFI_USB_ERROR_INVALID_HOST_FRAME,
                                               "checksum mismatch");
        parser_reset();
        return;
    }

    host_session_active = true;
    usb_diag.rx_frames++;
    usb_diag.last_frame_type = rx_frame[3];

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
            usb_diag.rx_errors++;
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
    uint8_t buffer[64];
    uint8_t byte;
    uint32_t recv_len;

    maybe_emit_status_on_dtr();

    if (!cdc_dev || !device_is_ready(cdc_dev)) {
        return;
    }

    while ((recv_len = ring_buf_get(&rx_ring, buffer, sizeof(buffer))) > 0U) {
        for (uint32_t index = 0; index < recv_len; index++) {
            parse_byte(buffer[index]);
        }
    }

    /* Fallback for CDC ACM backends where the UART IRQ callback path is unreliable. */
    while (uart_poll_in(cdc_dev, &byte) == 0) {
        usb_diag.rx_bytes++;
        parse_byte(byte);
    }

    if (rx_throttled && ring_buf_space_get(&rx_ring) > 0U) {
        rx_throttled = false;
        uart_irq_rx_enable(cdc_dev);
    }
}

int butterfi_usb_init(butterfi_usb_host_frame_handler_t handler, void *context)
{
    int ret;

    host_frame_handler = handler;
    host_frame_context = context;
    parser_reset();
    last_dtr_asserted = false;
    host_session_active = false;
    rx_throttled = false;
    ring_buf_init(&rx_ring, sizeof(rx_ring_buffer), rx_ring_buffer);
    ring_buf_init(&tx_ring, sizeof(tx_ring_buffer), tx_ring_buffer);

    if (!device_is_ready(cdc_dev)) {
        LOG_ERR("CDC ACM device not ready");
        return -ENODEV;
    }

    ret = usb_enable(NULL);
    if (ret < 0) {
        LOG_ERR("USB enable failed: %d", ret);
        return ret;
    }

    uart_irq_callback_set(cdc_dev, cdc_interrupt_handler);
    uart_irq_rx_enable(cdc_dev);

    LOG_INF("USB CDC-ACM ready");
    return 0;
}
