#include "butterfi_sidewalk_shim.h"

static enum butterfi_sidewalk_bridge_status butterfi_sidewalk_bridge_from_adapter(
    enum butterfi_adapter_status adapter_status)
{
    if (adapter_status == BUTTERFI_ADAPTER_INVALID_ARGUMENT) {
        return BUTTERFI_SIDEWALK_BRIDGE_INVALID_ARGUMENT;
    }

    if ((adapter_status == BUTTERFI_ADAPTER_OK) ||
        (adapter_status == BUTTERFI_ADAPTER_NEED_MORE_DATA)) {
        return BUTTERFI_SIDEWALK_BRIDGE_OK;
    }

    return BUTTERFI_SIDEWALK_BRIDGE_ADAPTER_ERROR;
}

enum butterfi_sidewalk_bridge_status butterfi_sidewalk_bridge_on_msg_received(
    struct butterfi_platform_shim *shim,
    const uint8_t *payload,
    size_t payload_len)
{
    return butterfi_sidewalk_bridge_from_adapter(
        butterfi_platform_shim_feed_sidewalk_downlink(shim, payload, payload_len));
}

enum butterfi_sidewalk_bridge_status butterfi_sidewalk_bridge_on_status_changed(
    struct butterfi_platform_shim *shim,
    bool sidewalk_ready,
    enum butterfi_link_state link_state)
{
    if (sidewalk_ready) {
        return butterfi_sidewalk_bridge_from_adapter(
            butterfi_platform_shim_set_sidewalk_ready(shim, link_state));
    }

    return butterfi_sidewalk_bridge_from_adapter(
        butterfi_platform_shim_set_sidewalk_starting(shim));
}

enum butterfi_sidewalk_bridge_status butterfi_sidewalk_bridge_on_serial_byte_stream(
    struct butterfi_platform_shim *shim,
    const uint8_t *payload,
    size_t payload_len)
{
    return butterfi_sidewalk_bridge_from_adapter(
        butterfi_platform_shim_feed_usb_bytes(shim, payload, payload_len));
}

enum butterfi_sidewalk_bridge_status butterfi_sidewalk_bridge_poll(
    struct butterfi_platform_shim *shim)
{
    return butterfi_sidewalk_bridge_from_adapter(
        butterfi_platform_shim_poll(shim));
}