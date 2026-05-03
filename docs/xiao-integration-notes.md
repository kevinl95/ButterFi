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
- post-flash config save over USB frame `0x06` with JSON payload persisted into NVS
- NVS-backed storage of `school_id`, `device_name`, and `content_pkg`
- Sidewalk initialization and status callbacks
- no implemented ButterFi runtime chunk/resend transport

## Integration Notes

1. USB runtime framing is aligned: both the browser console and the XIAO firmware now use the shared ButterFi binary frame layer.
2. Runtime protocol mismatch: the cloud expects `0x01/0x02/0x03` uplinks and `0x81` downlinks, but the XIAO scaffold does not emit or consume that payload contract.
3. Provisioning is now two-phase in the browser: the UF2 bootloader step writes the firmware image and Sidewalk credential, then the tool reconnects over runtime USB serial and saves the classroom config into NVS. The JSON files left on the UF2 volume are audit artifacts, not the authoritative config store.
4. Missing implementation: the original drop referenced `butterfi_content.c` but did not include it.
5. Registration path is only partially closed: the browser provisioning flow can now merge a supplied Sidewalk manufacturing credential hex/bin into the UF2 at the `mfg_storage` partition, but credential generation still happens outside the repo.
6. AWS stack output is not itself a device credential: the deployed CloudFormation stack exposes the Sidewalk destination name, while device-specific Sidewalk credential material still must be produced separately and then fed into the browser provisioning flow.