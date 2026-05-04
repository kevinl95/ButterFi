# ButterFi XIAO Firmware

Amazon Sidewalk firmware work for the Seeed Studio XIAO nRF52840.

This directory is now the active firmware track for the repo.

## Overview

This firmware currently provides:

- USB CDC provisioning path over line-delimited JSON
- NVS-backed storage for `school_id`, `device_name`, and `content_pkg`
- Sidewalk initialization skeleton
- LED state scaffolding

The integration notes for the current firmware, browser, and cloud contract are
in [docs/xiao-integration-notes.md](../../docs/xiao-integration-notes.md).

## Runtime Contract

The cloud stack and runtime browser console use the ButterFi binary transport
documented in [docs/shared-protocol.md](../../docs/shared-protocol.md).

The XIAO firmware directory also contains a USB provisioning path for device
setup and stored configuration.

## What this code does

- Starts a BLE-only Sidewalk stack for the XIAO target
- Exposes a USB CDC-ACM serial interface for provisioning
- Stores school ID, device name, and content package in NVS flash
- RGB LED reflects connection state

## Prerequisites

1. **nRF Connect SDK v2.3+**
   ```
   # Install via nRF Connect for Desktop → Toolchain Manager
   # or: https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/getting_started.html
   ```

2. **Amazon Sidewalk SDK**
   ```
   git clone https://github.com/nrfconnect/sdk-sidewalk $HOME/sidewalk
   export SIDEWALK_BASE=$HOME/sidewalk
   ```

3. **XIAO board support**
   The Seeed XIAO nRF52840 board files ship with nRF Connect SDK v2.3+
   as `xiao_ble`. No manual override needed.

## Build

```bash
cd /home/mrmemory/ButterFi

# Validated Sidewalk release runtime
export NCS_ENV_JSON=/path/to/ncs/toolchains/<toolchain-id>/environment.json
export ZEPHYR_BASE=/path/to/ncs/<version>/zephyr
export SIDEWALK_BASE=$HOME/sidewalk
BUTTERFI_USB_CONTROL_DEBUG=OFF \
BUTTERFI_INCLUDE_SIDEWALK=ON \
BUTTERFI_EXTRA_CONF_FILE=prj.sidewalk.conf \
./scripts/build-xiao.sh rebuild

# Optional: USB-control Sidewalk debug runtime
BUTTERFI_USB_CONTROL_DEBUG=ON \
BUTTERFI_INCLUDE_SIDEWALK=ON \
BUTTERFI_EXTRA_CONF_FILE=prj.sidewalk-debug.conf \
./scripts/build-xiao.sh rebuild
```

The helper build flow is documented in [docs/xiao-build.md](../../docs/xiao-build.md).

## Provisioning

After flashing, use [web/provision.html](../../web/provision.html)
for browser-based setup, or use the USB serial port directly:

```bash
screen /dev/ttyACM0 115200
```

Then send JSON commands such as:

```text
{"cmd":"ping"}
{"cmd":"status"}
{"cmd":"provision","school_id":"BOULDER-HS-01","device_name":"Room 204","content_pkg":"k12-general"}
```

## Optional diagnostics

These helpers are for quick local sanity checks after flashing. If the board
shows up on a different serial port, pass that path with `--tty`.

```bash
./scripts/probe-cdc-echo.py --tty /dev/ttyACM0
./scripts/probe-butterfi-status.py --tty /dev/ttyACM0
```

The echo probe sends one text line and prints the response. The status probe
sends the framed ButterFi status request and decodes any returned frames.

## LED States

| Color          | Pattern      | Meaning                  |
|----------------|--------------|--------------------------|
| Blue           | Slow pulse   | Booting                  |
| Red            | Slow blink   | Needs provisioning       |
| Yellow         | Fast blink   | Connecting to Sidewalk   |
| Green          | Solid        | Connected, ready         |
| White          | Flash        | Transmitting             |
| Red            | Fast blink   | Error                    |

## File Layout

```
src/
   main.c              — Sidewalk init, USB request handling, chunk forwarding, LED thread
   butterfi_config.c/h — NVS-backed config storage
   butterfi_usb.c/h    — USB CDC provisioning protocol
   butterfi_content.c/h— legacy Sidewalk content scaffold (current transport lives in main.c)
CMakeLists.txt
prj.conf              — Kconfig options
```

## Sidewalk registration

Before a device can connect to Sidewalk, it must be registered in AWS IoT
with a Sidewalk certificate. See:
https://docs.sidewalk.amazon/provisioning/ProvisioningDeveloperGuide.html
