# Firmware Build

This repo keeps the ButterFi-specific firmware code and the imported RAK manifest
repo in source control, but the synced west dependencies and build outputs under
vendor are reproducible and intentionally ignored.

The checked-in scripts in scripts are portable: they do not assume a specific
machine path, and they can either run inside an already-launched nRF Connect SDK
shell or consume a local toolchain environment.json file.

## Prerequisites

- git
- python3
- cmake
- ninja
- a local nRF Connect SDK toolchain installation

You can satisfy the toolchain requirement in either of these ways:

1. Launch a shell from Nordic's toolchain manager, then run the scripts directly.
2. Set NCS_ENV_JSON to your local environment.json path before running the scripts.

Example:

```bash
export NCS_ENV_JSON=/path/to/ncs/toolchains/<toolchain-id>/environment.json
```

## Rebuild Flow

From the repository root:

```bash
./scripts/build-rak4630.sh bootstrap
./scripts/build-rak4630.sh patch
./scripts/build-rak4630.sh configure
```

For a single command that replays the full flow:

```bash
./scripts/build-rak4630.sh rebuild
```

After the first configure step, incremental builds are:

```bash
./scripts/build-rak4630.sh build
```

## What Each Step Does

- bootstrap: initializes vendor as a west workspace if needed, then runs west update
- patch: applies the vendor manifest's west patch set with west patch -a
- configure: runs a pristine west build for the ButterFi-integrated RAK app
- build: runs ninja in the existing build directory

## Defaults

The scripts default to the ButterFi-integrated RAK4631 LoRa build:

- board: rak4631
- app: app/rak4631_rak1901_demo
- overlay: lora.conf

You can override these without editing the scripts:

```bash
BUTTERFI_BOARD=rak4631 \
BUTTERFI_APP=app/rak4631_rak1901_demo \
BUTTERFI_OVERLAY=lora.conf \
./scripts/build-rak4630.sh configure
```

If your toolchain manager exposes a root directory instead of an environment file,
set NCS_TOOLCHAIN_DIR and the scripts will look for environment.json under it.

## Artifacts

The main outputs are:

- merged image: vendor/RAK4630-Amazon-Sidewalk-Example/build/merged.hex
- ELF: vendor/RAK4630-Amazon-Sidewalk-Example/build/rak4631_rak1901_demo/zephyr/zephyr.elf

## Notes

- The vendor manifest is pinned to nRF Connect SDK and Sidewalk revision v2.9.1.
- The ButterFi integration is built from the imported manifest repo at vendor/RAK4630-Amazon-Sidewalk-Example.
- If you are already inside an nRF Connect SDK shell, you do not need to set NCS_ENV_JSON.