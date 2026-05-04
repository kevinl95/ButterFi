# Web Tools

This directory now contains two browser-side tools:

- `index.html`: the existing Chrome Web Serial console for the ButterFi USB framing protocol
- `provision.html`: the XIAO provisioning and web-flash flow

The console is packaged as a small installable PWA with an offline app shell.

## Serve Locally

Web Serial requires a secure context. The simplest local option is localhost:

```bash
python3 -m http.server 4173 --directory web
```

Then open either:

```text
http://localhost:4173
http://localhost:4173/provision.html
```

For end users, the same pages can be hosted on GitHub Pages or any other
HTTPS static host. Chromium-based browsers treat `https://` as a secure
context for Web Serial.

## Runtime Console

- connects to the ButterFi USB CDC ACM serial port
- sends host query, status, cancel, and resend frames
- parses ButterFi USB frames from the device
- assembles chunked Sidewalk response payloads into readable text
- shows device status, transfer progress, and a session log
- registers a service worker to cache the app shell for offline launch
- exposes the Chromium install prompt when the browser decides the app is installable

The runtime console still assumes the binary ButterFi USB framing protocol from
[docs/shared-protocol.md](../docs/shared-protocol.md).

## Provisioning

`provision.html` is the XIAO setup flow. It is oriented around the
UF2 bootloader flow and a per-device config payload.

The primary customer flow is a single-device, owner-managed setup using a
prebuilt ButterFi UF2 plus a Sidewalk credential generated from the buyer's own
AWS account. The concrete flow is documented in
[docs/self-hosted-owner-setup.md](../docs/self-hosted-owner-setup.md).

The browser provisioning page now supports:

- uploading a ButterFi release UF2
- optionally uploading a raw AWS `certificate.json` or a Sidewalk manufacturing credential package in `.hex` or `.bin` form
- merging the credential package into the UF2 at the firmware `mfg_storage` partition before writing the file to the XIAO bootloader drive
- writing audit files (`butterfi_config.json` and `butterfi_provisioning_manifest.json`) before the UF2 copy
- reconnecting over the ButterFi runtime serial port after reboot to save `school_id`, `device_name`, and `content_pkg` into NVS
- loading a single `butterfi-batch-v1` JSON package for multi-device rollout

The browser page does not currently consume CloudFormation outputs directly.
Today the important stack output is `SidewalkDestinationName`, but it is used
upstream during AWS IoT Wireless device registration rather than inside the page
itself.

When a batch package is loaded, `provision.html` tracks which device entries are already used in browser local storage keyed by the package `batch_id`. The intended flow is:

- a backend or admin tool generates one batch JSON containing the shared UF2 and per-device credentials
- the operator loads that JSON once in the browser
- the browser keeps the next unused device staged for flashing
- each successful flash marks that entry complete locally and advances the queue

The browser page does not create the Sidewalk device registration itself. That still comes from the external AWS IoT Wireless / Sidewalk provisioning flow.

If you have the combined AWS `certificate.json`, `provision.html` can convert it in-browser during flash. Use [scripts/build-sidewalk-credential.py](../scripts/build-sidewalk-credential.py) when you only have `get-wireless-device`/`get-device-profile` JSON exports or when you want offline `.hex` or `.bin` assets for batch packaging.

The batch package format and optional multi-device rollout flow are documented in [docs/xiao-batch-provisioning.md](../docs/xiao-batch-provisioning.md).

For admin-side packaging, use [scripts/build-batch-package.py](../scripts/build-batch-package.py) to bundle one UF2 plus many device credential files into a single `butterfi-batch-v1` JSON file.

## Installability Notes

- serve the app from `http://localhost` or `https://`
- load the page once so the service worker can cache the shell assets
- in Chrome or Edge, use the in-app install button or the browser install affordance

The app shell can open offline after installation, but Web Serial device access still requires the browser and hardware to be available.

## Browser Support

- Google Chrome
- Microsoft Edge

Other browsers may not expose the Web Serial API.