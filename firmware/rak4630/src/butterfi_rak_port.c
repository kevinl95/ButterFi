#include "butterfi_rak_port.h"

enum butterfi_adapter_status butterfi_rak_port_init(struct butterfi_rak_port *port,
                                                    uint32_t resend_timeout_ms)
{
    if (port == NULL) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    return butterfi_platform_shim_init(&port->shim, resend_timeout_ms);
}

enum butterfi_adapter_status butterfi_rak_port_on_serial_connected(struct butterfi_rak_port *port,
                                                                   bool connected)
{
    if (port == NULL) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    return butterfi_platform_shim_set_serial_connected(&port->shim, connected);
}

enum butterfi_adapter_status butterfi_rak_port_on_serial_rx(struct butterfi_rak_port *port,
                                                            const uint8_t *data,
                                                            size_t len)
{
    if (port == NULL) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    return butterfi_platform_shim_feed_usb_bytes(&port->shim, data, len);
}

enum butterfi_adapter_status butterfi_rak_port_on_sidewalk_status(struct butterfi_rak_port *port,
                                                                  bool sidewalk_ready,
                                                                  enum butterfi_link_state link_state)
{
    if (port == NULL) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    if (sidewalk_ready) {
        return butterfi_platform_shim_set_sidewalk_ready(&port->shim, link_state);
    }

    return butterfi_platform_shim_set_sidewalk_starting(&port->shim);
}

enum butterfi_adapter_status butterfi_rak_port_on_sidewalk_msg(struct butterfi_rak_port *port,
                                                               const uint8_t *data,
                                                               size_t len)
{
    if (port == NULL) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    return butterfi_platform_shim_feed_sidewalk_downlink(&port->shim, data, len);
}

enum butterfi_adapter_status butterfi_rak_port_poll(struct butterfi_rak_port *port,
                                                    uint32_t elapsed_ms)
{
    if (port == NULL) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    butterfi_platform_shim_advance_time(&port->shim, elapsed_ms);
    return butterfi_platform_shim_poll(&port->shim);
}

const struct butterfi_platform_tx_buffer *butterfi_rak_port_last_usb_tx(
    const struct butterfi_rak_port *port)
{
    if (port == NULL) {
        return NULL;
    }

    return &port->shim.last_usb_tx;
}

const struct butterfi_platform_tx_buffer *butterfi_rak_port_last_sidewalk_tx(
    const struct butterfi_rak_port *port)
{
    if (port == NULL) {
        return NULL;
    }

    return &port->shim.last_sidewalk_tx;
}