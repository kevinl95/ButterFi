#ifndef BUTTERFI_PLATFORM_SHIM_H
#define BUTTERFI_PLATFORM_SHIM_H

#include "butterfi_rak_adapter.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum butterfi_platform_event {
    BUTTERFI_PLATFORM_EVENT_NONE = 0,
    BUTTERFI_PLATFORM_EVENT_STATUS_CHANGED,
    BUTTERFI_PLATFORM_EVENT_TRANSFER_ERROR,
    BUTTERFI_PLATFORM_EVENT_TRANSFER_COMPLETE,
};

struct butterfi_platform_tx_buffer {
    uint8_t data[BUTTERFI_USB_MAX_FRAME_BYTES];
    size_t len;
};

struct butterfi_platform_shim {
    struct butterfi_rak_adapter adapter;
    struct butterfi_platform_tx_buffer last_usb_tx;
    struct butterfi_platform_tx_buffer last_sidewalk_tx;
    enum butterfi_platform_event last_event;
    uint32_t now_ms;
    bool serial_connected;
};

enum butterfi_adapter_status butterfi_platform_shim_init(struct butterfi_platform_shim *shim,
                                                         uint32_t resend_timeout_ms);

void butterfi_platform_shim_advance_time(struct butterfi_platform_shim *shim, uint32_t delta_ms);

enum butterfi_adapter_status butterfi_platform_shim_set_serial_connected(struct butterfi_platform_shim *shim,
                                                                         bool connected);

enum butterfi_adapter_status butterfi_platform_shim_set_sidewalk_ready(struct butterfi_platform_shim *shim,
                                                                       enum butterfi_link_state link_state);

enum butterfi_adapter_status butterfi_platform_shim_set_sidewalk_starting(struct butterfi_platform_shim *shim);

enum butterfi_adapter_status butterfi_platform_shim_feed_usb_bytes(struct butterfi_platform_shim *shim,
                                                                   const uint8_t *data,
                                                                   size_t len);

enum butterfi_adapter_status butterfi_platform_shim_feed_sidewalk_downlink(struct butterfi_platform_shim *shim,
                                                                           const uint8_t *data,
                                                                           size_t len);

enum butterfi_adapter_status butterfi_platform_shim_poll(struct butterfi_platform_shim *shim);

#endif