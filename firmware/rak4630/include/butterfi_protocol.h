#ifndef BUTTERFI_PROTOCOL_H
#define BUTTERFI_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BUTTERFI_USB_SYNC_1 0x42u
#define BUTTERFI_USB_SYNC_2 0x46u
#define BUTTERFI_USB_VERSION_1 0x01u

#define BUTTERFI_USB_HEADER_SIZE 8u
#define BUTTERFI_USB_OVERHEAD_BYTES 9u
#define BUTTERFI_USB_MAX_PAYLOAD_BYTES 512u
#define BUTTERFI_USB_MAX_FRAME_BYTES (BUTTERFI_USB_MAX_PAYLOAD_BYTES + BUTTERFI_USB_OVERHEAD_BYTES)

#define BUTTERFI_SIDEWALK_MAX_FRAME_BYTES 250u
#define BUTTERFI_SIDEWALK_CHUNK_OVERHEAD_BYTES 4u
#define BUTTERFI_SIDEWALK_MAX_CHUNK_TEXT_BYTES 236u

enum butterfi_sidewalk_msg_type {
    BUTTERFI_SIDEWALK_MSG_QUERY = 0x01,
    BUTTERFI_SIDEWALK_MSG_RESEND = 0x02,
    BUTTERFI_SIDEWALK_MSG_ACK = 0x03,
    BUTTERFI_SIDEWALK_MSG_RESPONSE_CHUNK = 0x81,
};

enum butterfi_usb_frame_type {
    BUTTERFI_USB_HOST_QUERY_SUBMIT = 0x01,
    BUTTERFI_USB_HOST_RESEND_REQUEST = 0x02,
    BUTTERFI_USB_HOST_CANCEL_REQUEST = 0x03,
    BUTTERFI_USB_HOST_PING = 0x04,
    BUTTERFI_USB_HOST_STATUS_REQUEST = 0x05,
    BUTTERFI_USB_DEVICE_STATUS = 0x81,
    BUTTERFI_USB_DEVICE_UPLINK_ACCEPTED = 0x82,
    BUTTERFI_USB_DEVICE_RESPONSE_CHUNK = 0x83,
    BUTTERFI_USB_DEVICE_TRANSFER_COMPLETE = 0x84,
    BUTTERFI_USB_DEVICE_TRANSFER_ERROR = 0x85,
    BUTTERFI_USB_DEVICE_PONG = 0x86,
};

enum butterfi_device_state {
    BUTTERFI_DEVICE_STATE_IDLE = 0,
    BUTTERFI_DEVICE_STATE_SERIAL_READY = 1,
    BUTTERFI_DEVICE_STATE_SIDEWALK_STARTING = 2,
    BUTTERFI_DEVICE_STATE_SIDEWALK_READY = 3,
    BUTTERFI_DEVICE_STATE_BUSY = 4,
    BUTTERFI_DEVICE_STATE_ERROR = 5,
};

enum butterfi_link_state {
    BUTTERFI_LINK_STATE_UNKNOWN = 0,
    BUTTERFI_LINK_STATE_BLE = 1,
    BUTTERFI_LINK_STATE_FSK = 2,
    BUTTERFI_LINK_STATE_LORA = 3,
};

enum butterfi_error_code {
    BUTTERFI_ERROR_INVALID_HOST_FRAME = 0x01,
    BUTTERFI_ERROR_DEVICE_BUSY = 0x02,
    BUTTERFI_ERROR_SIDEWALK_UNAVAILABLE = 0x03,
    BUTTERFI_ERROR_CLOUD_FETCH_FAILED = 0x04,
    BUTTERFI_ERROR_TRANSFER_TIMED_OUT = 0x05,
    BUTTERFI_ERROR_PROTOCOL_MISMATCH = 0x06,
};

enum butterfi_protocol_status {
    BUTTERFI_PROTOCOL_OK = 0,
    BUTTERFI_PROTOCOL_NEED_MORE_DATA,
    BUTTERFI_PROTOCOL_FRAME_READY,
    BUTTERFI_PROTOCOL_BUFFER_TOO_SMALL,
    BUTTERFI_PROTOCOL_PAYLOAD_TOO_LARGE,
    BUTTERFI_PROTOCOL_BAD_SYNC,
    BUTTERFI_PROTOCOL_BAD_VERSION,
    BUTTERFI_PROTOCOL_BAD_CHECKSUM,
    BUTTERFI_PROTOCOL_INVALID_ARGUMENT,
};

struct butterfi_usb_frame {
    uint8_t frame_type;
    uint8_t request_id;
    uint8_t flags;
    uint16_t payload_len;
    uint8_t payload[BUTTERFI_USB_MAX_PAYLOAD_BYTES];
};

struct butterfi_usb_parser {
    uint16_t expected_frame_len;
    uint16_t payload_len;
    size_t offset;
    uint8_t frame_bytes[BUTTERFI_USB_MAX_FRAME_BYTES];
};

void butterfi_usb_parser_init(struct butterfi_usb_parser *parser);
void butterfi_usb_parser_reset(struct butterfi_usb_parser *parser);

enum butterfi_protocol_status butterfi_usb_parser_push_byte(struct butterfi_usb_parser *parser,
                                                            uint8_t byte,
                                                            struct butterfi_usb_frame *frame);

enum butterfi_protocol_status butterfi_usb_frame_encode(const struct butterfi_usb_frame *frame,
                                                        uint8_t *out,
                                                        size_t out_capacity,
                                                        size_t *out_len);

uint8_t butterfi_usb_checksum(const uint8_t *data, size_t len);

#endif