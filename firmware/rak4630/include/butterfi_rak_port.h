#ifndef BUTTERFI_RAK_PORT_H
#define BUTTERFI_RAK_PORT_H

#include "butterfi_platform_shim.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct butterfi_rak_port {
    struct butterfi_platform_shim shim;
};

enum butterfi_adapter_status butterfi_rak_port_init(struct butterfi_rak_port *port,
                                                    uint32_t resend_timeout_ms);

enum butterfi_adapter_status butterfi_rak_port_on_serial_connected(struct butterfi_rak_port *port,
                                                                   bool connected);

enum butterfi_adapter_status butterfi_rak_port_on_serial_rx(struct butterfi_rak_port *port,
                                                            const uint8_t *data,
                                                            size_t len);

enum butterfi_adapter_status butterfi_rak_port_on_sidewalk_status(struct butterfi_rak_port *port,
                                                                  bool sidewalk_ready,
                                                                  enum butterfi_link_state link_state);

enum butterfi_adapter_status butterfi_rak_port_on_sidewalk_msg(struct butterfi_rak_port *port,
                                                               const uint8_t *data,
                                                               size_t len);

enum butterfi_adapter_status butterfi_rak_port_poll(struct butterfi_rak_port *port,
                                                    uint32_t elapsed_ms);

const struct butterfi_platform_tx_buffer *butterfi_rak_port_last_usb_tx(
    const struct butterfi_rak_port *port);

const struct butterfi_platform_tx_buffer *butterfi_rak_port_last_sidewalk_tx(
    const struct butterfi_rak_port *port);

#endif