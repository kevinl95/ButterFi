# XIAO Batch Provisioning

This document defines the browser-side batch provisioning workflow for the Seeed XIAO nRF52840 ButterFi path.

This is an optional advanced flow for multi-device rollout. The primary
self-hosted owner setup path is documented in
[docs/self-hosted-owner-setup.md](./self-hosted-owner-setup.md).

## Operator Goal

The target operator is an IT staff member or teacher who is comfortable plugging boards in and following a web UI, but is not expected to use Zephyr, `nrfjprog`, or raw Sidewalk tooling.

The intended workflow is:

1. Download one batch package from the admin or backend system.
2. Open [web/provision.html](../web/provision.html) in a Chromium-based browser.
3. Load the batch package once.
4. Plug in a board, enter UF2 bootloader mode, and flash it.
5. Let the browser advance to the next unused device entry.

## Batch Format

The browser expects a JSON file with `format` set to `butterfi-batch-v1`.

```json
{
  "format": "butterfi-batch-v1",
  "batch_id": "spring-2026-room-204",
  "label": "Room 204 · Spring 2026",
  "firmware": {
    "file_name": "butterfi-xiao-sidewalk.uf2",
    "format": "uf2",
    "content_base64": "<base64-encoded-uf2>"
  },
  "defaults": {
    "school_id": "BOULDER-HS-01",
    "content_pkg": "k12-general"
  },
  "devices": [
    {
      "device_id": "204-001",
      "device_name": "Room 204 / Student 01",
      "credential": {
        "file_name": "204-001.hex",
        "format": "hex",
        "content_base64": "<base64-encoded-hex-file>"
      }
    },
    {
      "device_id": "204-002",
      "device_name": "Room 204 / Student 02",
      "content_pkg": "k12-stem",
      "credential": {
        "file_name": "204-002.bin",
        "format": "bin",
        "content_base64": "<base64-encoded-bin-file>"
      }
    }
  ]
}
```

## Required Fields

- `format`: must be `butterfi-batch-v1`
- `batch_id`: stable identifier used for local progress tracking
- `devices`: array of per-device entries

## Optional Fields

- `label`: operator-friendly name shown in the browser
- `firmware`: embedded common UF2 for the whole batch
- `defaults.school_id`: default school/location for all devices
- `defaults.content_pkg`: default content package for all devices

## Device Fields

Each item in `devices` can provide:

- `device_id`: unique device entry identifier
- `device_name`: operator-facing or student-facing label
- `school_id`: overrides the batch default
- `content_pkg`: overrides the batch default
- `credential`: embedded Sidewalk manufacturing credential asset

The embedded credential can be either:

- `format: "json"` with `content_base64` or text representing the original AWS `certificate.json`
- `format: "hex"` with `content_base64` representing the original Intel HEX file text
- `format: "bin"` with `content_base64` representing raw bytes to be written at `mfg_storage`

## Current Browser Behavior

When a batch package is loaded, the browser:

1. Selects the next unused device entry.
2. Prefills `school_id`, `device_name`, and `content_pkg`.
3. Uses the embedded UF2 when present.
4. Uses the selected device credential when present.
5. Merges the credential into the UF2 at `mfg_storage` (`0xEB000`).
6. Writes `butterfi_config.json`, `butterfi_provisioning_manifest.json`, and the provisioned UF2 to the XIAO bootloader drive.
7. Reconnects to the ButterFi runtime serial port after reboot and saves `school_id`, `device_name`, and `content_pkg` into NVS.
8. Marks the batch entry complete in browser local storage keyed by `batch_id`.

## Current Limitation

The browser tracks batch completion only in local storage. If the operator switches browsers or machines, the queue state does not automatically follow them unless that state is also tracked by a backend system.

## Converting AWS Exports Into Credential Assets

For single-device manual flashing, the browser can consume raw AWS
`certificate.json` directly. For batch packaging, CLI-only export paths, or
offline asset generation, you can still pre-convert AWS exports into `.hex` or
`.bin` files.

This repo includes [scripts/build-sidewalk-credential.py](../scripts/build-sidewalk-credential.py),
which wraps the official Sidewalk SDK provisioner and forces the ButterFi XIAO
`mfg_storage` address from [firmware/xiao_nrf52840/pm_static.yml](../firmware/xiao_nrf52840/pm_static.yml).

The wrapper runs the official Sidewalk SDK provisioner with your current Python
environment. If that environment is missing `yaml` or `intelhex`, install the
SDK tool dependencies from `SIDEWALK_BASE/tools/provision/requirements.txt`.

If you downloaded a combined `certificate.json` from the AWS IoT console:

```bash
python3 scripts/build-sidewalk-credential.py \
  --certificate-json provisioning/aws/204-001/certificate.json \
  --basename 204-001 \
  --output-dir provisioning/credentials
```

If you are working from AWS CLI exports instead:

```bash
python3 scripts/build-sidewalk-credential.py \
  --wireless-device-json provisioning/aws/204-001/wireless_device.json \
  --device-profile-json provisioning/aws/device_profile.json \
  --basename 204-001 \
  --output-dir provisioning/credentials
```

Each command produces:

- `provisioning/credentials/204-001.bin`
- `provisioning/credentials/204-001.hex`

Either file can be uploaded in [web/provision.html](../web/provision.html) or referenced as `credential_path` when building a batch package.

## Admin Packaging Script

This repo now includes [scripts/build-batch-package.py](../scripts/build-batch-package.py) to generate `butterfi-batch-v1` packages for the browser tool.

Example:

```bash
python3 scripts/build-batch-package.py \
  --uf2 artifacts/butterfi-xiao-sidewalk.uf2 \
  --manifest provisioning/classroom-204.csv \
  --output artifacts/classroom-204-batch.json \
  --batch-id classroom-204-spring-2026 \
  --label "Room 204 · Spring 2026" \
  --default-school-id BOULDER-HS-01 \
  --default-content-pkg k12-general
```

CSV manifests should include these columns:

- `device_id`
- `device_name`
- `credential_path` to a raw `certificate.json` file or a converted `.hex` or `.bin` file
- optional `school_id`
- optional `content_pkg`

## Packaging Guidance

The batch package should be produced by a manufacturing, admin, or backend
workflow, not by school staff. That workflow should:

1. Choose a shared release UF2 for the rollout.
2. Generate or import one Sidewalk credential package per device.
3. Embed those assets into one batch JSON, either directly in backend code or by calling [scripts/build-batch-package.py](../scripts/build-batch-package.py).
4. Hand the operator a single downloadable file.