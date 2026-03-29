#include "butterfi_rak_adapter.h"

#include <string.h>

static uint32_t butterfi_adapter_now_ms(struct butterfi_rak_adapter *adapter)
{
    if ((adapter == NULL) || (adapter->callbacks.get_time_ms == NULL)) {
        return 0u;
    }

    return adapter->callbacks.get_time_ms(adapter->callback_context);
}

static enum butterfi_adapter_status butterfi_adapter_send_usb_frame(struct butterfi_rak_adapter *adapter,
                                                                    const struct butterfi_usb_frame *frame)
{
    uint8_t encoded[BUTTERFI_USB_MAX_FRAME_BYTES];
    size_t encoded_len = 0u;
    enum butterfi_protocol_status protocol_status;

    if ((adapter == NULL) || (frame == NULL) || (adapter->callbacks.usb_write == NULL)) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    protocol_status = butterfi_usb_frame_encode(frame, encoded, sizeof(encoded), &encoded_len);
    if (protocol_status != BUTTERFI_PROTOCOL_OK) {
        return BUTTERFI_ADAPTER_PROTOCOL_ERROR;
    }

    return adapter->callbacks.usb_write(encoded, encoded_len, adapter->callback_context);
}

static enum butterfi_adapter_status butterfi_adapter_send_sidewalk_frame(struct butterfi_rak_adapter *adapter,
                                                                         const uint8_t *data,
                                                                         size_t len)
{
    if ((adapter == NULL) || (data == NULL) || (adapter->callbacks.sidewalk_send == NULL)) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    if ((len == 0u) || (len > BUTTERFI_SIDEWALK_MAX_FRAME_BYTES)) {
        return BUTTERFI_ADAPTER_PROTOCOL_ERROR;
    }

    return adapter->callbacks.sidewalk_send(data, len, adapter->callback_context);
}

static enum butterfi_adapter_status butterfi_adapter_send_uplink_accepted(struct butterfi_rak_adapter *adapter,
                                                                          uint8_t request_id)
{
    struct butterfi_usb_frame frame;

    memset(&frame, 0, sizeof(frame));
    frame.frame_type = BUTTERFI_USB_DEVICE_UPLINK_ACCEPTED;
    frame.request_id = request_id;

    return butterfi_adapter_send_usb_frame(adapter, &frame);
}

static enum butterfi_adapter_status butterfi_adapter_send_transfer_complete(struct butterfi_rak_adapter *adapter,
                                                                            uint8_t request_id)
{
    struct butterfi_usb_frame frame;

    memset(&frame, 0, sizeof(frame));
    frame.frame_type = BUTTERFI_USB_DEVICE_TRANSFER_COMPLETE;
    frame.request_id = request_id;

    return butterfi_adapter_send_usb_frame(adapter, &frame);
}

static enum butterfi_adapter_status butterfi_adapter_send_pong(struct butterfi_rak_adapter *adapter,
                                                               const struct butterfi_usb_frame *request)
{
    struct butterfi_usb_frame frame;

    memset(&frame, 0, sizeof(frame));
    frame.frame_type = BUTTERFI_USB_DEVICE_PONG;
    frame.request_id = request->request_id;
    frame.flags = request->flags;
    frame.payload_len = request->payload_len;
    if (frame.payload_len > 0u) {
        memcpy(frame.payload, request->payload, frame.payload_len);
    }

    return butterfi_adapter_send_usb_frame(adapter, &frame);
}

static enum butterfi_adapter_status butterfi_adapter_send_chunk_to_browser(struct butterfi_rak_adapter *adapter,
                                                                           const uint8_t *data,
                                                                           size_t len)
{
    struct butterfi_usb_frame frame;

    if ((data == NULL) || (len > BUTTERFI_USB_MAX_PAYLOAD_BYTES) || (len < 4u)) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    memset(&frame, 0, sizeof(frame));
    frame.frame_type = BUTTERFI_USB_DEVICE_RESPONSE_CHUNK;
    frame.request_id = data[1];
    frame.payload_len = (uint16_t)len;
    memcpy(frame.payload, data, len);

    return butterfi_adapter_send_usb_frame(adapter, &frame);
}

static enum butterfi_adapter_status butterfi_adapter_emit_sidewalk_query(struct butterfi_rak_adapter *adapter,
                                                                         const struct butterfi_usb_frame *frame)
{
    uint8_t buffer[BUTTERFI_SIDEWALK_MAX_FRAME_BYTES];
    size_t frame_len;
    enum butterfi_adapter_status status;
    enum butterfi_session_status session_status;
    uint32_t now_ms;

    if ((adapter == NULL) || (frame == NULL) || (frame->payload_len == 0u)) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    if (adapter->device_state != BUTTERFI_DEVICE_STATE_SIDEWALK_READY) {
        return butterfi_rak_adapter_emit_error(adapter,
                                               frame->request_id,
                                               BUTTERFI_ERROR_SIDEWALK_UNAVAILABLE,
                                               "Sidewalk not ready");
    }

    if ((frame->payload_len + 2u) > BUTTERFI_SIDEWALK_MAX_FRAME_BYTES) {
        return butterfi_rak_adapter_emit_error(adapter,
                                               frame->request_id,
                                               BUTTERFI_ERROR_PROTOCOL_MISMATCH,
                                               "Query too large for Sidewalk");
    }

    now_ms = butterfi_adapter_now_ms(adapter);
    session_status = butterfi_session_begin_request(&adapter->session, frame->request_id, now_ms);
    if (session_status == BUTTERFI_SESSION_BUSY) {
        return butterfi_rak_adapter_emit_error(adapter,
                                               frame->request_id,
                                               BUTTERFI_ERROR_DEVICE_BUSY,
                                               "Request already in flight");
    }
    if (session_status != BUTTERFI_SESSION_OK) {
        return BUTTERFI_ADAPTER_PROTOCOL_ERROR;
    }

    buffer[0] = BUTTERFI_SIDEWALK_MSG_QUERY;
    buffer[1] = frame->request_id;
    memcpy(&buffer[2], frame->payload, frame->payload_len);
    frame_len = (size_t)frame->payload_len + 2u;

    status = butterfi_adapter_send_sidewalk_frame(adapter, buffer, frame_len);
    if (status != BUTTERFI_ADAPTER_OK) {
        butterfi_session_reset(&adapter->session);
        return butterfi_rak_adapter_emit_error(adapter,
                                               frame->request_id,
                                               BUTTERFI_ERROR_SIDEWALK_UNAVAILABLE,
                                               "Failed to submit Sidewalk uplink");
    }

    adapter->device_state = BUTTERFI_DEVICE_STATE_BUSY;
    status = butterfi_adapter_send_uplink_accepted(adapter, frame->request_id);
    if (status != BUTTERFI_ADAPTER_OK) {
        return status;
    }

    return butterfi_rak_adapter_emit_status(adapter);
}

static enum butterfi_adapter_status butterfi_adapter_emit_resend(struct butterfi_rak_adapter *adapter,
                                                                 uint8_t request_id,
                                                                 uint8_t chunk_idx)
{
    uint8_t buffer[3];
    enum butterfi_adapter_status status;

    buffer[0] = BUTTERFI_SIDEWALK_MSG_RESEND;
    buffer[1] = request_id;
    buffer[2] = chunk_idx;

    status = butterfi_adapter_send_sidewalk_frame(adapter, buffer, sizeof(buffer));
    if (status == BUTTERFI_ADAPTER_OK) {
        butterfi_session_note_resend(&adapter->session, butterfi_adapter_now_ms(adapter));
    }

    return status;
}

static enum butterfi_adapter_status butterfi_adapter_handle_host_resend(struct butterfi_rak_adapter *adapter,
                                                                        const struct butterfi_usb_frame *frame)
{
    enum butterfi_session_status session_status;

    if ((adapter == NULL) || (frame == NULL) || (frame->payload_len != 1u)) {
        return butterfi_rak_adapter_emit_error(adapter,
                                               frame != NULL ? frame->request_id : 0u,
                                               BUTTERFI_ERROR_INVALID_HOST_FRAME,
                                               "Resend requires a single chunk index");
    }

    session_status = butterfi_session_request_resend_from(&adapter->session,
                                                          frame->payload[0],
                                                          butterfi_adapter_now_ms(adapter));
    if (session_status != BUTTERFI_SESSION_OK) {
        return butterfi_rak_adapter_emit_error(adapter,
                                               frame->request_id,
                                               BUTTERFI_ERROR_INVALID_HOST_FRAME,
                                               "Cannot resend without active request");
    }

    return butterfi_adapter_emit_resend(adapter, adapter->session.request_id, frame->payload[0]);
}

static enum butterfi_adapter_status butterfi_adapter_dispatch_host_frame(struct butterfi_rak_adapter *adapter,
                                                                         const struct butterfi_usb_frame *frame)
{
    if ((adapter == NULL) || (frame == NULL)) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    switch (frame->frame_type) {
    case BUTTERFI_USB_HOST_QUERY_SUBMIT:
        return butterfi_adapter_emit_sidewalk_query(adapter, frame);
    case BUTTERFI_USB_HOST_RESEND_REQUEST:
        return butterfi_adapter_handle_host_resend(adapter, frame);
    case BUTTERFI_USB_HOST_CANCEL_REQUEST:
        butterfi_session_reset(&adapter->session);
        adapter->device_state = BUTTERFI_DEVICE_STATE_SIDEWALK_READY;
        return butterfi_rak_adapter_emit_status(adapter);
    case BUTTERFI_USB_HOST_PING:
        return butterfi_adapter_send_pong(adapter, frame);
    case BUTTERFI_USB_HOST_STATUS_REQUEST:
        return butterfi_rak_adapter_emit_status(adapter);
    default:
        return butterfi_rak_adapter_emit_error(adapter,
                                               frame->request_id,
                                               BUTTERFI_ERROR_INVALID_HOST_FRAME,
                                               "Unsupported host frame type");
    }
}

enum butterfi_adapter_status butterfi_rak_adapter_init(struct butterfi_rak_adapter *adapter,
                                                       const struct butterfi_rak_adapter_config *config,
                                                       const struct butterfi_rak_adapter_callbacks *callbacks,
                                                       void *callback_context)
{
    if ((adapter == NULL) || (config == NULL) || (callbacks == NULL) ||
        (callbacks->usb_write == NULL) || (callbacks->sidewalk_send == NULL)) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    memset(adapter, 0, sizeof(*adapter));
    adapter->callbacks = *callbacks;
    adapter->callback_context = callback_context;
    adapter->device_state = BUTTERFI_DEVICE_STATE_IDLE;
    adapter->link_state = BUTTERFI_LINK_STATE_UNKNOWN;

    butterfi_usb_parser_init(&adapter->usb_parser);
    butterfi_session_init(&adapter->session, config->resend_timeout_ms);

    return BUTTERFI_ADAPTER_OK;
}

enum butterfi_adapter_status butterfi_rak_adapter_set_serial_connected(struct butterfi_rak_adapter *adapter,
                                                                       bool connected)
{
    if (adapter == NULL) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    adapter->serial_connected = connected;
    if (connected && (adapter->device_state == BUTTERFI_DEVICE_STATE_IDLE)) {
        adapter->device_state = BUTTERFI_DEVICE_STATE_SERIAL_READY;
    }

    return butterfi_rak_adapter_emit_status(adapter);
}

enum butterfi_adapter_status butterfi_rak_adapter_set_sidewalk_state(struct butterfi_rak_adapter *adapter,
                                                                     enum butterfi_device_state device_state,
                                                                     enum butterfi_link_state link_state)
{
    if (adapter == NULL) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    adapter->device_state = device_state;
    adapter->link_state = link_state;

    return butterfi_rak_adapter_emit_status(adapter);
}

enum butterfi_adapter_status butterfi_rak_adapter_handle_usb_byte(struct butterfi_rak_adapter *adapter,
                                                                  uint8_t byte)
{
    struct butterfi_usb_frame frame;
    enum butterfi_protocol_status protocol_status;

    if (adapter == NULL) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    memset(&frame, 0, sizeof(frame));
    protocol_status = butterfi_usb_parser_push_byte(&adapter->usb_parser, byte, &frame);
    if (protocol_status == BUTTERFI_PROTOCOL_NEED_MORE_DATA) {
        return BUTTERFI_ADAPTER_NEED_MORE_DATA;
    }
    if (protocol_status == BUTTERFI_PROTOCOL_FRAME_READY) {
        return butterfi_adapter_dispatch_host_frame(adapter, &frame);
    }
    if ((protocol_status == BUTTERFI_PROTOCOL_BAD_SYNC) || (protocol_status == BUTTERFI_PROTOCOL_NEED_MORE_DATA)) {
        return BUTTERFI_ADAPTER_NEED_MORE_DATA;
    }

    return butterfi_rak_adapter_emit_error(adapter,
                                           0u,
                                           BUTTERFI_ERROR_INVALID_HOST_FRAME,
                                           "Invalid USB frame");
}

enum butterfi_adapter_status butterfi_rak_adapter_handle_sidewalk_downlink(struct butterfi_rak_adapter *adapter,
                                                                           const uint8_t *data,
                                                                           size_t len)
{
    enum butterfi_session_status session_status;
    bool is_complete = false;
    bool is_new_chunk = false;
    enum butterfi_adapter_status adapter_status;

    if ((adapter == NULL) || (data == NULL) || (len < 4u)) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    if (data[0] != BUTTERFI_SIDEWALK_MSG_RESPONSE_CHUNK) {
        return BUTTERFI_ADAPTER_PROTOCOL_ERROR;
    }

    session_status = butterfi_session_record_chunk(&adapter->session,
                                                   data[1],
                                                   data[2],
                                                   data[3],
                                                   butterfi_adapter_now_ms(adapter),
                                                   &is_complete,
                                                   &is_new_chunk);
    if ((session_status != BUTTERFI_SESSION_OK) && (session_status != BUTTERFI_SESSION_DUPLICATE_CHUNK)) {
        return BUTTERFI_ADAPTER_PROTOCOL_ERROR;
    }

    adapter_status = butterfi_adapter_send_chunk_to_browser(adapter, data, len);
    if (adapter_status != BUTTERFI_ADAPTER_OK) {
        return adapter_status;
    }

    if (is_complete) {
        adapter_status = butterfi_adapter_send_transfer_complete(adapter, data[1]);
        if (adapter_status != BUTTERFI_ADAPTER_OK) {
            return adapter_status;
        }

        butterfi_session_reset(&adapter->session);
        adapter->device_state = BUTTERFI_DEVICE_STATE_SIDEWALK_READY;
        return butterfi_rak_adapter_emit_status(adapter);
    }

    return BUTTERFI_ADAPTER_OK;
}

enum butterfi_adapter_status butterfi_rak_adapter_poll(struct butterfi_rak_adapter *adapter)
{
    uint8_t chunk_idx;
    enum butterfi_session_status session_status;

    if (adapter == NULL) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    if (!butterfi_session_should_resend(&adapter->session, butterfi_adapter_now_ms(adapter))) {
        return BUTTERFI_ADAPTER_OK;
    }

    session_status = butterfi_session_get_next_needed_chunk(&adapter->session, &chunk_idx);
    if (session_status != BUTTERFI_SESSION_OK) {
        return BUTTERFI_ADAPTER_PROTOCOL_ERROR;
    }

    return butterfi_adapter_emit_resend(adapter, adapter->session.request_id, chunk_idx);
}

enum butterfi_adapter_status butterfi_rak_adapter_emit_status(struct butterfi_rak_adapter *adapter)
{
    struct butterfi_usb_frame frame;

    if (adapter == NULL) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    memset(&frame, 0, sizeof(frame));
    frame.frame_type = BUTTERFI_USB_DEVICE_STATUS;
    frame.payload_len = 3u;
    frame.payload[0] = (uint8_t)adapter->device_state;
    frame.payload[1] = (uint8_t)adapter->link_state;
    frame.payload[2] = adapter->session.active ? adapter->session.request_id : 0u;

    return butterfi_adapter_send_usb_frame(adapter, &frame);
}

enum butterfi_adapter_status butterfi_rak_adapter_emit_error(struct butterfi_rak_adapter *adapter,
                                                             uint8_t request_id,
                                                             enum butterfi_error_code error_code,
                                                             const char *message)
{
    struct butterfi_usb_frame frame;
    size_t message_len = 0u;

    if (adapter == NULL) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    memset(&frame, 0, sizeof(frame));
    frame.frame_type = BUTTERFI_USB_DEVICE_TRANSFER_ERROR;
    frame.request_id = request_id;
    frame.payload[0] = (uint8_t)error_code;
    frame.payload_len = 1u;

    if (message != NULL) {
        while ((message[message_len] != '\0') && ((message_len + 1u) < BUTTERFI_USB_MAX_PAYLOAD_BYTES)) {
            frame.payload[1u + message_len] = (uint8_t)message[message_len];
            ++message_len;
        }
        frame.payload_len = (uint16_t)(1u + message_len);
    }

    adapter->device_state = BUTTERFI_DEVICE_STATE_ERROR;
    return butterfi_adapter_send_usb_frame(adapter, &frame);
}