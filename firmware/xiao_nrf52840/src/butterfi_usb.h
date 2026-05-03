/* Shared ButterFi USB framing used by the browser runtime console. */

#ifndef BUTTERFI_USB_H
#define BUTTERFI_USB_H

#include <stddef.h>
#include <stdint.h>

enum butterfi_usb_frame_type {
	BUTTERFI_USB_FRAME_HOST_QUERY_SUBMIT = 0x01,
	BUTTERFI_USB_FRAME_HOST_RESEND_REQUEST = 0x02,
	BUTTERFI_USB_FRAME_HOST_CANCEL_REQUEST = 0x03,
	BUTTERFI_USB_FRAME_HOST_PING = 0x04,
	BUTTERFI_USB_FRAME_HOST_STATUS_REQUEST = 0x05,
	BUTTERFI_USB_FRAME_HOST_CONFIG_SAVE = 0x06,
	BUTTERFI_USB_FRAME_DEVICE_STATUS = 0x81,
	BUTTERFI_USB_FRAME_DEVICE_UPLINK_ACCEPTED = 0x82,
	BUTTERFI_USB_FRAME_DEVICE_RESPONSE_CHUNK = 0x83,
	BUTTERFI_USB_FRAME_DEVICE_TRANSFER_COMPLETE = 0x84,
	BUTTERFI_USB_FRAME_DEVICE_TRANSFER_ERROR = 0x85,
	BUTTERFI_USB_FRAME_DEVICE_PONG = 0x86,
	BUTTERFI_USB_FRAME_DEVICE_DEBUG_TEXT = 0x87,
	BUTTERFI_USB_FRAME_DEVICE_CONFIG_SAVED = 0x88,
};

enum butterfi_usb_device_state {
	BUTTERFI_USB_DEVICE_STATE_IDLE = 0,
	BUTTERFI_USB_DEVICE_STATE_SERIAL_READY = 1,
	BUTTERFI_USB_DEVICE_STATE_SIDEWALK_STARTING = 2,
	BUTTERFI_USB_DEVICE_STATE_SIDEWALK_READY = 3,
	BUTTERFI_USB_DEVICE_STATE_BUSY = 4,
	BUTTERFI_USB_DEVICE_STATE_ERROR = 5,
	BUTTERFI_USB_DEVICE_STATE_SIDEWALK_NOT_REGISTERED = 6,
};

enum butterfi_usb_link_state {
	BUTTERFI_USB_LINK_UNKNOWN = 0,
	BUTTERFI_USB_LINK_BLE = 1,
	BUTTERFI_USB_LINK_FSK = 2,
	BUTTERFI_USB_LINK_LORA = 3,
};

enum butterfi_usb_error_code {
	BUTTERFI_USB_ERROR_INVALID_HOST_FRAME = 0x01,
	BUTTERFI_USB_ERROR_DEVICE_BUSY = 0x02,
	BUTTERFI_USB_ERROR_SIDEWALK_UNAVAILABLE = 0x03,
	BUTTERFI_USB_ERROR_CLOUD_FETCH_FAILED = 0x04,
	BUTTERFI_USB_ERROR_TRANSFER_TIMED_OUT = 0x05,
	BUTTERFI_USB_ERROR_PROTOCOL_MISMATCH = 0x06,
	BUTTERFI_USB_ERROR_CONFIG_SAVE_FAILED = 0x07,
};

typedef void (*butterfi_usb_host_frame_handler_t)(uint8_t frame_type,
												  uint8_t request_id,
												  const uint8_t *payload,
												  uint16_t payload_len,
												  void *context);

struct butterfi_usb_diag_counters {
	uint32_t rx_bytes;
	uint32_t rx_frames;
	uint32_t rx_errors;
	int32_t last_tx_result;
	uint8_t last_frame_type;
};

int  butterfi_usb_init(butterfi_usb_host_frame_handler_t handler, void *context);
void butterfi_usb_poll(void);
void butterfi_usb_get_diag_counters(struct butterfi_usb_diag_counters *diag);
void butterfi_usb_update_status(uint8_t device_state,
								uint8_t link_state,
								uint8_t active_request_id);
int  butterfi_usb_send_status(void);
int  butterfi_usb_send_uplink_accepted(uint8_t request_id);
int  butterfi_usb_send_response_chunk(uint8_t request_id,
									  const uint8_t *payload,
									  uint16_t payload_len);
int  butterfi_usb_send_transfer_complete(uint8_t request_id);
int  butterfi_usb_send_transfer_error(uint8_t request_id,
									  uint8_t error_code,
									  const char *message);
int  butterfi_usb_send_pong(uint8_t request_id,
							const uint8_t *payload,
							uint16_t payload_len);
int  butterfi_usb_send_debug_text(const char *message);
int  butterfi_usb_send_config_saved(uint8_t request_id, const char *message);

#endif /* BUTTERFI_USB_H */
