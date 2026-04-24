# XIAO Build

This repo no longer builds the retired RAK firmware path. The active build
target is the Seeed XIAO nRF52840 firmware under
[firmware/xiao_nrf52840/](../firmware/xiao_nrf52840).

## Prerequisites

- `west`
- `cmake`
- `ninja`
- an nRF Connect SDK toolchain
- a local clone of the Amazon Sidewalk SDK, exported as `SIDEWALK_BASE`

You can satisfy the toolchain requirement in either of these ways:

1. Launch a shell from Nordic's Toolchain Manager and run the script directly.
2. Set `NCS_ENV_JSON` to your local `environment.json` file and let the script activate it.

Example:

```bash
export NCS_ENV_JSON=/path/to/ncs/toolchains/<toolchain-id>/environment.json
export SIDEWALK_BASE=$HOME/sidewalk
```

## Build Flow

From the repo root:

```bash
./scripts/build-xiao.sh configure
./scripts/build-xiao.sh build
```

To do a pristine rebuild:

```bash
./scripts/build-xiao.sh rebuild
```

## Defaults

- board: `xiao_ble`
- app dir: `firmware/xiao_nrf52840`
- build dir: `firmware/xiao_nrf52840/build`

You can override these without editing the script:

```bash
BUTTERFI_BOARD=xiao_ble \
BUTTERFI_APP=firmware/xiao_nrf52840 \
./scripts/build-xiao.sh configure
```

## Notes

- The firmware, browser tools, and cloud stack share the ButterFi transport
  contract documented in [docs/shared-protocol.md](shared-protocol.md).
- The XIAO-specific integration details are summarized in
  [docs/xiao-integration-notes.md](xiao-integration-notes.md).