# XIAO Integration Notes

This note captures the current relationship between the XIAO firmware tree and
the existing ButterFi cloud/browser contract.

## Cloud Expectations

The Lambda logic in [template.yaml](../template.yaml) currently expects:

- Sidewalk uplinks with `msg_type` byte `0x01`, `0x02`, or `0x03`
- `requestId` in byte `1`
- `0x02` resend requests carrying a 1-byte `startIdx`
- downlinks with byte `0` set to `0x81`, followed by `requestId`, `chunkIdx`, and `totalChunks`
- Sidewalk device identity from `WirelessMetadata.Sidewalk.SidewalkId`

## Browser Expectations

The runtime browser app in [web/app.js](../web/app.js) currently expects:

- framed binary USB packets with sync bytes `0x42 0x46`
- host query submit, resend, cancel, and status frame types
- device status, chunk, transfer complete, and transfer error frames

## XIAO Firmware

The XIAO firmware in [firmware/xiao_nrf52840/](../firmware/xiao_nrf52840) currently provides:

- USB CDC control over framed binary ButterFi packets
- host query submit, resend request, and cancel handling over USB
- Sidewalk uplinks using `0x01=query` and `0x02=resend`
- Sidewalk downlink chunk handling for `0x81=response chunk`, forwarded to the browser as USB frame `0x83`
- transfer completion once all chunks for the active request are received
- post-flash config save over USB frame `0x06` with JSON payload persisted into NVS
- NVS-backed storage of `school_id`, `device_name`, and `content_pkg`
- Sidewalk initialization and status callbacks

## Integration Notes

1. USB runtime framing is aligned: both the browser console and the XIAO firmware now use the shared ButterFi binary frame layer.
2. The milestone-one query/resend/chunk contract is implemented end to end in source: the browser sends framed USB query/resend requests, the firmware emits Sidewalk `0x01`/`0x02` uplinks, and the cloud returns `0x81` chunk payloads that the firmware forwards back to the browser.
3. Provisioning is now two-phase in the browser: the UF2 bootloader step writes the firmware image and Sidewalk credential, then the tool reconnects over runtime USB serial and saves the classroom config into NVS. The JSON files left on the UF2 volume are audit artifacts, not the authoritative config store.
4. `butterfi_content.c` is now legacy scaffolding, not the active runtime transport path. The live Sidewalk message handling is implemented directly in `main.c`.
5. Registration path is partially closed: the browser provisioning flow can now merge either a raw AWS `certificate.json` or a supplied Sidewalk manufacturing credential hex/bin into the UF2 at the `mfg_storage` partition. The repo still includes `scripts/build-sidewalk-credential.py` as an admin-side wrapper around the official Sidewalk provisioner for CLI-only AWS exports and offline asset generation.
6. AWS stack output is not itself a device credential: the deployed CloudFormation stack exposes the Sidewalk destination name, while device-specific Sidewalk credential material still must be produced separately and then fed into the browser provisioning flow.
7. Remaining risk is validation, not protocol shape: the source contracts line up, but this repo still needs a real hardware round-trip against a dev Sidewalk destination before it should be treated as production-ready.