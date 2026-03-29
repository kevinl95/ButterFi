#ifndef BUTTERFI_SIDEWALK_SHIM_H
#define BUTTERFI_SIDEWALK_SHIM_H

#include "butterfi_platform_shim.h"

#include <stddef.h>
#include <stdint.h>

enum butterfi_sidewalk_bridge_status {
    BUTTERFI_SIDEWALK_BRIDGE_OK = 0,
    BUTTERFI_SIDEWALK_BRIDGE_INVALID_ARGUMENT,
    BUTTERFI_SIDEWALK_BRIDGE_ADAPTER_ERROR,
};

enum butterfi_sidewalk_bridge_status butterfi_sidewalk_bridge_on_msg_received(
    struct butterfi_platform_shim *shim,
    const uint8_t *payload,
    size_t payload_len);

enum butterfi_sidewalk_bridge_status butterfi_sidewalk_bridge_on_status_changed(
    struct butterfi_platform_shim *shim,
    bool sidewalk_ready,
    enum butterfi_link_state link_state);

enum butterfi_sidewalk_bridge_status butterfi_sidewalk_bridge_on_serial_byte_stream(
    struct butterfi_platform_shim *shim,
    const uint8_t *payload,
    size_t payload_len);

enum butterfi_sidewalk_bridge_status butterfi_sidewalk_bridge_poll(
    struct butterfi_platform_shim *shim);

#endif