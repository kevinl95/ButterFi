#ifndef BUTTERFI_RAK_ADAPTER_H
#define BUTTERFI_RAK_ADAPTER_H

#include "butterfi_protocol.h"
#include "butterfi_session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum butterfi_adapter_status {
    BUTTERFI_ADAPTER_OK = 0,
    BUTTERFI_ADAPTER_NEED_MORE_DATA,
    BUTTERFI_ADAPTER_TX_FAILED,
    BUTTERFI_ADAPTER_SIDEWALK_SEND_FAILED,
    BUTTERFI_ADAPTER_INVALID_ARGUMENT,
    BUTTERFI_ADAPTER_PROTOCOL_ERROR,
    BUTTERFI_ADAPTER_BUSY,
};

typedef enum butterfi_adapter_status (*butterfi_usb_write_fn)(const uint8_t *data,
                                                              size_t len,
                                                              void *context);
typedef enum butterfi_adapter_status (*butterfi_sidewalk_send_fn)(const uint8_t *data,
                                                                  size_t len,
                                                                  void *context);
typedef uint32_t (*butterfi_time_ms_fn)(void *context);

struct butterfi_rak_adapter_callbacks {
    butterfi_usb_write_fn usb_write;
    butterfi_sidewalk_send_fn sidewalk_send;
    butterfi_time_ms_fn get_time_ms;
};

struct butterfi_rak_adapter_config {
    uint32_t resend_timeout_ms;
};

struct butterfi_rak_adapter {
    struct butterfi_rak_adapter_callbacks callbacks;
    void *callback_context;
    struct butterfi_usb_parser usb_parser;
    struct butterfi_session session;
    enum butterfi_device_state device_state;
    enum butterfi_link_state link_state;
    bool serial_connected;
};

enum butterfi_adapter_status butterfi_rak_adapter_init(struct butterfi_rak_adapter *adapter,
                                                       const struct butterfi_rak_adapter_config *config,
                                                       const struct butterfi_rak_adapter_callbacks *callbacks,
                                                       void *callback_context);

enum butterfi_adapter_status butterfi_rak_adapter_set_serial_connected(struct butterfi_rak_adapter *adapter,
                                                                       bool connected);

enum butterfi_adapter_status butterfi_rak_adapter_set_sidewalk_state(struct butterfi_rak_adapter *adapter,
                                                                     enum butterfi_device_state device_state,
                                                                     enum butterfi_link_state link_state);

enum butterfi_adapter_status butterfi_rak_adapter_handle_usb_byte(struct butterfi_rak_adapter *adapter,
                                                                  uint8_t byte);

enum butterfi_adapter_status butterfi_rak_adapter_handle_sidewalk_downlink(struct butterfi_rak_adapter *adapter,
                                                                           const uint8_t *data,
                                                                           size_t len);

enum butterfi_adapter_status butterfi_rak_adapter_poll(struct butterfi_rak_adapter *adapter);

enum butterfi_adapter_status butterfi_rak_adapter_emit_status(struct butterfi_rak_adapter *adapter);

enum butterfi_adapter_status butterfi_rak_adapter_emit_error(struct butterfi_rak_adapter *adapter,
                                                             uint8_t request_id,
                                                             enum butterfi_error_code error_code,
                                                             const char *message);

#endif