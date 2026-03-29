#include "butterfi_protocol.h"

#include <string.h>

static uint16_t butterfi_read_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

void butterfi_usb_parser_init(struct butterfi_usb_parser *parser)
{
    if (parser == NULL) {
        return;
    }

    butterfi_usb_parser_reset(parser);
}

void butterfi_usb_parser_reset(struct butterfi_usb_parser *parser)
{
    if (parser == NULL) {
        return;
    }

    parser->expected_frame_len = 0;
    parser->payload_len = 0;
    parser->offset = 0;
    memset(parser->frame_bytes, 0, sizeof(parser->frame_bytes));
}

uint8_t butterfi_usb_checksum(const uint8_t *data, size_t len)
{
    uint8_t checksum = 0;
    size_t idx = 0;

    if (data == NULL) {
        return 0;
    }

    for (idx = 0; idx < len; ++idx) {
        checksum ^= data[idx];
    }

    return checksum;
}

static enum butterfi_protocol_status butterfi_usb_parser_finish_frame(struct butterfi_usb_parser *parser,
                                                                      struct butterfi_usb_frame *frame)
{
    uint8_t expected_checksum;
    uint8_t actual_checksum;

    if ((parser == NULL) || (frame == NULL)) {
        return BUTTERFI_PROTOCOL_INVALID_ARGUMENT;
    }

    expected_checksum = butterfi_usb_checksum(&parser->frame_bytes[2],
                                              (size_t)parser->expected_frame_len - 3u);
    actual_checksum = parser->frame_bytes[parser->expected_frame_len - 1u];
    if (expected_checksum != actual_checksum) {
        butterfi_usb_parser_reset(parser);
        return BUTTERFI_PROTOCOL_BAD_CHECKSUM;
    }

    frame->frame_type = parser->frame_bytes[3];
    frame->request_id = parser->frame_bytes[4];
    frame->flags = parser->frame_bytes[5];
    frame->payload_len = parser->payload_len;

    if (parser->payload_len > 0u) {
        memcpy(frame->payload, &parser->frame_bytes[BUTTERFI_USB_HEADER_SIZE], parser->payload_len);
    }

    butterfi_usb_parser_reset(parser);
    return BUTTERFI_PROTOCOL_FRAME_READY;
}

enum butterfi_protocol_status butterfi_usb_parser_push_byte(struct butterfi_usb_parser *parser,
                                                            uint8_t byte,
                                                            struct butterfi_usb_frame *frame)
{
    if ((parser == NULL) || (frame == NULL)) {
        return BUTTERFI_PROTOCOL_INVALID_ARGUMENT;
    }

    if ((parser->offset == 0u) && (byte != BUTTERFI_USB_SYNC_1)) {
        return BUTTERFI_PROTOCOL_BAD_SYNC;
    }

    if (parser->offset == 1u) {
        if (byte != BUTTERFI_USB_SYNC_2) {
            parser->offset = (byte == BUTTERFI_USB_SYNC_1) ? 1u : 0u;
            if (parser->offset == 1u) {
                parser->frame_bytes[0] = BUTTERFI_USB_SYNC_1;
            }
            return BUTTERFI_PROTOCOL_BAD_SYNC;
        }
    }

    if (parser->offset >= sizeof(parser->frame_bytes)) {
        butterfi_usb_parser_reset(parser);
        return BUTTERFI_PROTOCOL_BUFFER_TOO_SMALL;
    }

    parser->frame_bytes[parser->offset++] = byte;

    if (parser->offset == BUTTERFI_USB_HEADER_SIZE) {
        if (parser->frame_bytes[2] != BUTTERFI_USB_VERSION_1) {
            butterfi_usb_parser_reset(parser);
            return BUTTERFI_PROTOCOL_BAD_VERSION;
        }

        parser->payload_len = butterfi_read_u16_le(&parser->frame_bytes[6]);
        if (parser->payload_len > BUTTERFI_USB_MAX_PAYLOAD_BYTES) {
            butterfi_usb_parser_reset(parser);
            return BUTTERFI_PROTOCOL_PAYLOAD_TOO_LARGE;
        }

        parser->expected_frame_len = (uint16_t)(BUTTERFI_USB_HEADER_SIZE + parser->payload_len + 1u);
    }

    if ((parser->expected_frame_len > 0u) && (parser->offset == parser->expected_frame_len)) {
        return butterfi_usb_parser_finish_frame(parser, frame);
    }

    return BUTTERFI_PROTOCOL_NEED_MORE_DATA;
}

enum butterfi_protocol_status butterfi_usb_frame_encode(const struct butterfi_usb_frame *frame,
                                                        uint8_t *out,
                                                        size_t out_capacity,
                                                        size_t *out_len)
{
    size_t frame_len;

    if ((frame == NULL) || (out == NULL) || (out_len == NULL)) {
        return BUTTERFI_PROTOCOL_INVALID_ARGUMENT;
    }

    if (frame->payload_len > BUTTERFI_USB_MAX_PAYLOAD_BYTES) {
        return BUTTERFI_PROTOCOL_PAYLOAD_TOO_LARGE;
    }

    frame_len = BUTTERFI_USB_HEADER_SIZE + frame->payload_len + 1u;
    if (out_capacity < frame_len) {
        return BUTTERFI_PROTOCOL_BUFFER_TOO_SMALL;
    }

    out[0] = BUTTERFI_USB_SYNC_1;
    out[1] = BUTTERFI_USB_SYNC_2;
    out[2] = BUTTERFI_USB_VERSION_1;
    out[3] = frame->frame_type;
    out[4] = frame->request_id;
    out[5] = frame->flags;
    out[6] = (uint8_t)(frame->payload_len & 0xFFu);
    out[7] = (uint8_t)((frame->payload_len >> 8) & 0xFFu);

    if (frame->payload_len > 0u) {
        memcpy(&out[BUTTERFI_USB_HEADER_SIZE], frame->payload, frame->payload_len);
    }

    out[frame_len - 1u] = butterfi_usb_checksum(&out[2], frame_len - 3u);
    *out_len = frame_len;

    return BUTTERFI_PROTOCOL_OK;
}