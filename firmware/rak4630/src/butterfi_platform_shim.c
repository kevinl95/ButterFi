#include "butterfi_platform_shim.h"

#include <string.h>

static enum butterfi_adapter_status butterfi_platform_usb_write(const uint8_t *data,
                                                                size_t len,
                                                                void *context)
{
    struct butterfi_platform_shim *shim = (struct butterfi_platform_shim *)context;

    if ((shim == NULL) || (data == NULL) || (len > sizeof(shim->last_usb_tx.data))) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    memcpy(shim->last_usb_tx.data, data, len);
    shim->last_usb_tx.len = len;
    shim->last_event = BUTTERFI_PLATFORM_EVENT_STATUS_CHANGED;
    return BUTTERFI_ADAPTER_OK;
}

static enum butterfi_adapter_status butterfi_platform_sidewalk_send(const uint8_t *data,
                                                                    size_t len,
                                                                    void *context)
{
    struct butterfi_platform_shim *shim = (struct butterfi_platform_shim *)context;

    if ((shim == NULL) || (data == NULL) || (len > sizeof(shim->last_sidewalk_tx.data))) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    memcpy(shim->last_sidewalk_tx.data, data, len);
    shim->last_sidewalk_tx.len = len;
    return BUTTERFI_ADAPTER_OK;
}

static uint32_t butterfi_platform_now_ms(void *context)
{
    struct butterfi_platform_shim *shim = (struct butterfi_platform_shim *)context;

    if (shim == NULL) {
        return 0u;
    }

    return shim->now_ms;
}

enum butterfi_adapter_status butterfi_platform_shim_init(struct butterfi_platform_shim *shim,
                                                         uint32_t resend_timeout_ms)
{
    struct butterfi_rak_adapter_callbacks callbacks;
    struct butterfi_rak_adapter_config config;

    if (shim == NULL) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    memset(shim, 0, sizeof(*shim));

    callbacks.usb_write = butterfi_platform_usb_write;
    callbacks.sidewalk_send = butterfi_platform_sidewalk_send;
    callbacks.get_time_ms = butterfi_platform_now_ms;
    config.resend_timeout_ms = resend_timeout_ms;

    return butterfi_rak_adapter_init(&shim->adapter, &config, &callbacks, shim);
}

void butterfi_platform_shim_advance_time(struct butterfi_platform_shim *shim, uint32_t delta_ms)
{
    if (shim == NULL) {
        return;
    }

    shim->now_ms += delta_ms;
}

enum butterfi_adapter_status butterfi_platform_shim_set_serial_connected(struct butterfi_platform_shim *shim,
                                                                         bool connected)
{
    if (shim == NULL) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    shim->serial_connected = connected;
    return butterfi_rak_adapter_set_serial_connected(&shim->adapter, connected);
}

enum butterfi_adapter_status butterfi_platform_shim_set_sidewalk_ready(struct butterfi_platform_shim *shim,
                                                                       enum butterfi_link_state link_state)
{
    if (shim == NULL) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    return butterfi_rak_adapter_set_sidewalk_state(&shim->adapter,
                                                   BUTTERFI_DEVICE_STATE_SIDEWALK_READY,
                                                   link_state);
}

enum butterfi_adapter_status butterfi_platform_shim_set_sidewalk_starting(struct butterfi_platform_shim *shim)
{
    if (shim == NULL) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    return butterfi_rak_adapter_set_sidewalk_state(&shim->adapter,
                                                   BUTTERFI_DEVICE_STATE_SIDEWALK_STARTING,
                                                   BUTTERFI_LINK_STATE_UNKNOWN);
}

enum butterfi_adapter_status butterfi_platform_shim_feed_usb_bytes(struct butterfi_platform_shim *shim,
                                                                   const uint8_t *data,
                                                                   size_t len)
{
    size_t idx;
    enum butterfi_adapter_status status = BUTTERFI_ADAPTER_OK;

    if ((shim == NULL) || ((data == NULL) && (len > 0u))) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    for (idx = 0; idx < len; ++idx) {
        status = butterfi_rak_adapter_handle_usb_byte(&shim->adapter, data[idx]);
        if ((status != BUTTERFI_ADAPTER_OK) && (status != BUTTERFI_ADAPTER_NEED_MORE_DATA)) {
            return status;
        }
    }

    return status;
}

enum butterfi_adapter_status butterfi_platform_shim_feed_sidewalk_downlink(struct butterfi_platform_shim *shim,
                                                                           const uint8_t *data,
                                                                           size_t len)
{
    if ((shim == NULL) || (data == NULL)) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    return butterfi_rak_adapter_handle_sidewalk_downlink(&shim->adapter, data, len);
}

enum butterfi_adapter_status butterfi_platform_shim_poll(struct butterfi_platform_shim *shim)
{
    if (shim == NULL) {
        return BUTTERFI_ADAPTER_INVALID_ARGUMENT;
    }

    return butterfi_rak_adapter_poll(&shim->adapter);
}