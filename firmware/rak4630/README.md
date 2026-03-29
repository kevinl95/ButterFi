# ButterFi RAK4630 Firmware Scaffold

This folder contains the host-independent firmware foundation for ButterFi on
the RAK4630 starter kit.

The intent is to keep Sidewalk bring-up, USB CDC ACM transport, and ButterFi
request tracking separated so the final dongle can reuse the same protocol and
session code.

## Layout

- `include/butterfi_protocol.h`: shared constants and USB serial frame codec API
- `include/butterfi_session.h`: single-request transfer state machine API
- `include/butterfi_rak_adapter.h`: thin adapter API for USB, Sidewalk, and status callbacks
- `include/butterfi_platform_shim.h`: Zephyr/runtime-facing shim surface for time, serial, and polling
- `include/butterfi_sidewalk_shim.h`: Sidewalk callback bridge surface for the RAK app
- `include/butterfi_rak_port.h`: narrow port API that mirrors the real RAK callback seams
- `src/butterfi_protocol.c`: frame encoding, checksum, and streaming parser
- `src/butterfi_session.c`: chunk tracking, resend deadlines, and completion state
- `src/butterfi_rak_adapter.c`: adapter stub that consumes USB bytes and emits Sidewalk actions
- `src/butterfi_platform_shim.c`: hostable platform shim that stores the last USB and Sidewalk TX buffers
- `src/butterfi_sidewalk_shim.c`: small bridge that translates Sidewalk and serial callbacks into adapter calls
- `src/butterfi_rak_port.c`: concrete port wrapper to use from real RAK callback sites
- `integration-map.md`: exact mapping from the upstream RAK example files into the ButterFi layers

## How This Maps To The RAK Sidewalk Example

Use the upstream RAK example as the board and Sidewalk integration layer, then
drop these modules into the application layer.

Recommended mapping:

1. USB CDC ACM task
   Read bytes from the Zephyr UART/CDC device and feed them into
   `butterfi_usb_parser_push_byte()`.

2. Host command dispatcher
   When a USB frame is complete, map host commands to Sidewalk actions:
   - `0x01`: start a new query uplink if the session is idle
   - `0x02`: override the next-needed chunk index and trigger resend
   - `0x03`: cancel the active request
   - `0x05`: emit an immediate device status frame

3. Sidewalk receive callback
   When a Sidewalk downlink arrives, pass the chunk header into
   `butterfi_session_record_chunk()`, then forward the downlink payload to the
   browser as USB frame type `0x83`.

4. Sidewalk status callback
   Convert link and device status changes into USB status frames for the browser.

5. Resend scheduler
   Run a periodic timer or work item that checks
   `butterfi_session_should_resend()` and, when true, sends a Sidewalk resend
   frame using the next needed chunk index.

## Milestone One Rules

- Only one active request at a time
- Browser disconnect does not cancel the Sidewalk request automatically
- Firmware owns resend timing and gap detection
- Browser receives progress as forwarded chunk frames and status frames

## Next Integration Step

Create a thin RAK-specific adapter that wires:

- USB CDC ACM RX/TX
- Sidewalk `on_msg_received`
- Sidewalk `on_status_changed`
- a resend timer or Zephyr work queue

onto the APIs in this folder.

That adapter API now exists in `butterfi_rak_adapter.*`; the remaining work is
to plug the real RAK4630 Zephyr and Sidewalk callback signatures into it.

The new shim layer narrows that remaining work further:

1. Replace the in-memory USB write callback in `butterfi_platform_shim.c` with a
   Zephyr CDC ACM write path.
2. Replace the in-memory Sidewalk send callback in `butterfi_platform_shim.c`
   with the real `sid_put_msg` wrapper used by the RAK app.
3. Call `butterfi_sidewalk_bridge_on_serial_byte_stream()` from the USB RX path.
4. Call `butterfi_sidewalk_bridge_on_msg_received()` from the Sidewalk receive callback.
5. Call `butterfi_sidewalk_bridge_on_status_changed()` from the Sidewalk status callback.
6. Call `butterfi_sidewalk_bridge_poll()` from a periodic Zephyr work item or timer.

If you are wiring against the upstream example directly, start with
`integration-map.md` and the `butterfi_rak_port.*` surface rather than calling
the lower layers yourself.