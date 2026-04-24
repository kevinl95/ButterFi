/*
 * butterfi_usb.h / butterfi_usb.c
 *
 * USB CDC-ACM interface for the web provisioning tool.
 *
 * Simple line-based protocol — the web app sends JSON commands,
 * the device responds with JSON. This keeps the web side simple
 * (WebSerial reads lines) and is easy to extend.
 *
 * Commands (host → device):
 *   {"cmd":"ping"}
 *   {"cmd":"status"}
 *   {"cmd":"provision","school_id":"X","device_name":"Y","content_pkg":"Z"}
 *   {"cmd":"reset"}
 *
 * Responses (device → host):
 *   {"ok":true,"fw":"1.0.0","school_id":"X","sidewalk":"READY"}
 *   {"ok":false,"error":"..."}
 */

#ifndef BUTTERFI_USB_H
#define BUTTERFI_USB_H

int  butterfi_usb_init(void);
void butterfi_usb_poll(void);
void butterfi_usb_send_status(const char *sidewalk_state);

#endif /* BUTTERFI_USB_H */
