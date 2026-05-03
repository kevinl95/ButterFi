# XIAO Build

This is the verified bootstrap and build flow for the active Seeed XIAO nRF52840
firmware target in [firmware/xiao_nrf52840](../firmware/xiao_nrf52840).

## What You Need

- Nordic nRF Connect SDK installed locally
- the Zephyr tree inside that NCS workspace
- a local checkout of the Nordic Sidewalk SDK
- `cmake`, `ninja`, and `west`

Start here:

- nRF Connect SDK install guide: <https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation/install_ncs.html>
- nRF Connect SDK manifest repository: <https://github.com/nrfconnect/sdk-nrf>
- Sidewalk SDK repository: <https://github.com/nrfconnect/sdk-sidewalk>

The checked-in helper entrypoint is [scripts/build-xiao.sh](../scripts/build-xiao.sh).
That script activates the Nordic toolchain through [scripts/with-ncs-env.sh](../scripts/with-ncs-env.sh)
when `NCS_ENV_JSON` is set, then builds [firmware/xiao_nrf52840](../firmware/xiao_nrf52840).
It assumes:

- `ZEPHYR_BASE` points at your Zephyr tree inside the NCS workspace
- `SIDEWALK_BASE` points at your Sidewalk SDK checkout
- optionally `NCS_ENV_JSON` points at Nordic's `environment.json` if you are not already inside a Toolchain Manager shell

## Fresh Bootstrap

If you are starting from a fresh machine or a fresh SDK install, use this order:

1. Install nRF Connect SDK using Nordic's official guide.
2. Create or locate your NCS workspace.
3. Clone the Sidewalk SDK locally.
4. Export the three environment variables the ButterFi build helper expects.
5. Create the Sidewalk sibling symlink required by the current SDK layout.
6. Run a pristine XIAO rebuild from the ButterFi repo root.

For a fresh setup, the two upstream sources you actually need are:

- nRF Connect SDK docs: <https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation/install_ncs.html>
- Sidewalk SDK repo: <https://github.com/nrfconnect/sdk-sidewalk>

The app-specific configuration you are building is mainly in:

- [firmware/xiao_nrf52840/prj.conf](../firmware/xiao_nrf52840/prj.conf)
- [firmware/xiao_nrf52840/pm_static.yml](../firmware/xiao_nrf52840/pm_static.yml)
- [firmware/xiao_nrf52840/src/main.c](../firmware/xiao_nrf52840/src/main.c)
- [firmware/xiao_nrf52840/src/butterfi_usb.c](../firmware/xiao_nrf52840/src/butterfi_usb.c)

## Find Your Paths

You need three local paths.

### 1. `NCS_ENV_JSON`

If you launch builds from a normal shell instead of the Toolchain Manager shell,
point `NCS_ENV_JSON` at the toolchain's `environment.json`.

Nordic's install guide documents the default NCS layout and toolchain setup:
<https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation/install_ncs.html>

Typical location:

```bash
~/ncs/toolchains/<toolchain-id>/environment.json
```

If you do not know the exact toolchain id yet, this usually finds it:

```bash
find ~/ncs/toolchains -name environment.json
```

If you want to see how ButterFi consumes that file, check [scripts/with-ncs-env.sh](../scripts/with-ncs-env.sh).

### 2. `ZEPHYR_BASE`

This must point at the Zephyr tree inside the NCS workspace you are building
against.

The upstream NCS manifest repo is here:
<https://github.com/nrfconnect/sdk-nrf>

Typical location:

```bash
~/ncs/<version>/zephyr
```

### 3. `SIDEWALK_BASE`

This must point at your local Sidewalk SDK checkout.

Clone it from:
<https://github.com/nrfconnect/sdk-sidewalk>

Typical location:

```bash
~/sdk-sidewalk
```

## Export The Environment

Example using the same structure that was validated during bring-up:

```bash
export NCS_ENV_JSON="$HOME/ncs/toolchains/<toolchain-id>/environment.json"
export ZEPHYR_BASE="$HOME/ncs/<version>/zephyr"
export SIDEWALK_BASE="$HOME/sdk-sidewalk"
```

If you already opened a shell from Nordic Toolchain Manager, you may not need
`NCS_ENV_JSON`, but keeping it set is harmless and makes the helper script
deterministic.

## Required Sidewalk Symlink

The current Sidewalk SDK revision assumes the SDK is also visible at:

```bash
$(dirname "$ZEPHYR_BASE")/sidewalk
```

If your checkout lives somewhere else, create or refresh the symlink before you
build:

```bash
ln -sfn "$SIDEWALK_BASE" "$(dirname "$ZEPHYR_BASE")/sidewalk"
```

That sibling-path assumption is enforced by [scripts/build-xiao.sh](../scripts/build-xiao.sh).

## Build Commands

From the ButterFi repo root:

```bash
./scripts/build-xiao.sh configure
./scripts/build-xiao.sh build
```

For a clean end-to-end rebuild:

```bash
./scripts/build-xiao.sh rebuild
```

The plain commands above build the base USB runtime. The canonical Sidewalk
profiles are:

- release runtime: [firmware/xiao_nrf52840/prj.sidewalk.conf](../firmware/xiao_nrf52840/prj.sidewalk.conf)
- USB control debug runtime: [firmware/xiao_nrf52840/prj.sidewalk-debug.conf](../firmware/xiao_nrf52840/prj.sidewalk-debug.conf)

Build the validated non-debug Sidewalk runtime with:

```bash
BUTTERFI_USB_CONTROL_DEBUG=OFF \
BUTTERFI_INCLUDE_SIDEWALK=ON \
BUTTERFI_EXTRA_CONF_FILE=prj.sidewalk.conf \
./scripts/build-xiao.sh rebuild
```

Build the Sidewalk USB-control debug runtime with:

```bash
BUTTERFI_USB_CONTROL_DEBUG=ON \
BUTTERFI_INCLUDE_SIDEWALK=ON \
BUTTERFI_EXTRA_CONF_FILE=prj.sidewalk-debug.conf \
./scripts/build-xiao.sh rebuild
```

The exact helper implementation is in [scripts/build-xiao.sh](../scripts/build-xiao.sh).

The helper defaults are:

- board: `xiao_ble`
- app dir: `firmware/xiao_nrf52840`
- build dir: `firmware/xiao_nrf52840/build`

You can override them if needed:

```bash
BUTTERFI_BOARD=xiao_ble \
BUTTERFI_APP=/absolute/path/to/firmware/xiao_nrf52840 \
BUTTERFI_BUILD_DIR=/absolute/path/to/build \
./scripts/build-xiao.sh configure
```

## Output Artifacts

Successful builds place artifacts under:

```bash
firmware/xiao_nrf52840/build/xiao_nrf52840/zephyr/
```

The files you usually care about are:

- [firmware/xiao_nrf52840/build/xiao_nrf52840/zephyr/zephyr.uf2](../firmware/xiao_nrf52840/build/xiao_nrf52840/zephyr/zephyr.uf2)
- [firmware/xiao_nrf52840/build/xiao_nrf52840/zephyr/zephyr.hex](../firmware/xiao_nrf52840/build/xiao_nrf52840/zephyr/zephyr.hex)
- [firmware/xiao_nrf52840/build/xiao_nrf52840/zephyr/zephyr.bin](../firmware/xiao_nrf52840/build/xiao_nrf52840/zephyr/zephyr.bin)
- [firmware/xiao_nrf52840/build/xiao_nrf52840/zephyr/zephyr.elf](../firmware/xiao_nrf52840/build/xiao_nrf52840/zephyr/zephyr.elf)
- [firmware/xiao_nrf52840/build/merged.hex](../firmware/xiao_nrf52840/build/merged.hex)

The latest verified rebuild produced all of those artifacts successfully.

## Flashing Notes

For the XIAO UF2 bootloader flow, [firmware/xiao_nrf52840/build/xiao_nrf52840/zephyr/zephyr.uf2](../firmware/xiao_nrf52840/build/xiao_nrf52840/zephyr/zephyr.uf2)
is the most convenient artifact.
Put the board into UF2 bootloader mode and copy that file onto the mounted UF2
drive.

If you are using a different flashing workflow, use
[firmware/xiao_nrf52840/build/xiao_nrf52840/zephyr/zephyr.hex](../firmware/xiao_nrf52840/build/xiao_nrf52840/zephyr/zephyr.hex)
or [firmware/xiao_nrf52840/build/merged.hex](../firmware/xiao_nrf52840/build/merged.hex)
as appropriate for your tool.

## Sanity Checks

If a fresh setup fails, check these first:

1. `ZEPHYR_BASE` is set and points at a real Zephyr tree.
2. `SIDEWALK_BASE` is set and points at a real Sidewalk checkout.
3. `$(dirname "$ZEPHYR_BASE")/sidewalk` exists, even if it is only a symlink.
4. `NCS_ENV_JSON` points at a real `environment.json` when building outside Toolchain Manager.
5. You are running the helper from the ButterFi repo root.

If you have not installed the SDKs yet, use these upstream sources instead of any local-machine path examples:

- nRF Connect SDK install docs: <https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation/install_ncs.html>
- nRF Connect SDK source manifest: <https://github.com/nrfconnect/sdk-nrf>
- Sidewalk SDK source: <https://github.com/nrfconnect/sdk-sidewalk>

## Related Docs

- The runtime contract shared by firmware, browser, and cloud is in [docs/shared-protocol.md](shared-protocol.md).
- XIAO-specific integration notes are in [docs/xiao-integration-notes.md](xiao-integration-notes.md).
- The repo-level overview is in [README.md](../README.md).