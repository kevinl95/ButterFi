# ButterFi To RAK4630 Integration Map

This maps the ButterFi firmware layers in this repo onto the upstream
RAK4630 Amazon Sidewalk example structure.

## Upstream Files To Touch

- `app/rak4631_rak1901_demo/src/app.c`
- `app/rak4631_rak1901_demo/src/main.c`
- `app/rak4631_rak1901_demo/src/sidewalk_events.c`
- optionally `app/rak4631_rak1901_demo/src/app_rx.c` and `app_tx.c` if you keep
  parts of the existing queueing model

## Upstream Callback Seams

The upstream app already exposes the exact seams ButterFi needs:

1. `on_sidewalk_msg_received(...)` in `app.c`
   Current behavior copies Sidewalk payloads into `app_rx_msg_received(...)`.
   ButterFi replacement: call `butterfi_rak_port_on_sidewalk_msg(...)` with the
   raw message bytes.

2. `on_sidewalk_status_changed(...)` in `app.c`
   Current behavior copies status into a queued event.
   ButterFi replacement: translate Sidewalk readiness and link mask into
   `butterfi_rak_port_on_sidewalk_status(...)`.

3. `on_sidewalk_send_error(...)` in `app.c`
   ButterFi extension: convert send failures into a browser-facing transfer
   error frame via the adapter.

4. `main.c`
   The upstream example already includes Zephyr USB and UART headers. Use this
   as the place to initialize CDC ACM and signal
   `butterfi_rak_port_on_serial_connected(...)`.

5. `sidewalk_event_send_msg(...)` in `sidewalk_events.c`
   This is the real send path that eventually calls `sid_put_msg(...)`.
   ButterFi integration should wrap the Sidewalk transmit callback used by the
   shim so it allocates and sends a `sidewalk_msg_t` using the same queue/event
   model instead of directly calling Sidewalk from arbitrary contexts.

## Recommended Wiring

### Serial RX Path

When USB CDC ACM receives bytes:

1. read bytes from the UART/CDC driver
2. pass them to `butterfi_rak_port_on_serial_rx(...)`
3. if `last_sidewalk_tx` becomes populated, wrap that payload in a real
   `sidewalk_msg_t` and schedule it through `sidewalk_event_send(...)`

### Sidewalk RX Path

In `on_sidewalk_msg_received(...)`:

1. ignore ack-only packets the same way the upstream code already does
2. pass non-ack raw payload bytes to `butterfi_rak_port_on_sidewalk_msg(...)`
3. if `last_usb_tx` becomes populated, write it to the CDC ACM interface

### Sidewalk Status Path

In `on_sidewalk_status_changed(...)`:

1. determine whether the device is Sidewalk-ready
2. map link information into ButterFi link state
3. call `butterfi_rak_port_on_sidewalk_status(...)`
4. write any resulting USB status frame to CDC ACM

### Periodic Resend Poll

The upstream app already uses timers and worker threads.

Recommended approach:

1. add a Zephyr timer or delayed work item dedicated to ButterFi
2. call `butterfi_rak_port_poll(...)` with elapsed milliseconds
3. if a resend frame is produced in `last_sidewalk_tx`, schedule it through the
   same Sidewalk send queue path as a normal query uplink

## Minimal Porting Order

1. Instantiate one `struct butterfi_rak_port`
2. Connect CDC ACM RX to `butterfi_rak_port_on_serial_rx(...)`
3. Connect Sidewalk RX callback to `butterfi_rak_port_on_sidewalk_msg(...)`
4. Connect Sidewalk status callback to `butterfi_rak_port_on_sidewalk_status(...)`
5. Connect periodic work to `butterfi_rak_port_poll(...)`
6. Replace stub TX callbacks with real USB write and Sidewalk send implementations

## Intentional Non-Goals In This Repo

- No copy of upstream Amazon proprietary demo parser code
- No guessed `sid_msg_desc` construction against headers not present locally
- No guessed Zephyr device tree or CDC ACM instance names

Those details should be added only after the real RAK workspace or app tree is
present locally.