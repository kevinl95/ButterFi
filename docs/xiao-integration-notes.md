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

- USB CDC provisioning over line-delimited JSON
- NVS-backed storage of `school_id`, `device_name`, and `content_pkg`
- Sidewalk initialization and status callbacks
- no implemented ButterFi runtime chunk/resend transport

## Integration Notes

1. USB protocol mismatch: XIAO uses JSON commands; the browser console uses binary framed packets.
2. Runtime protocol mismatch: the cloud expects `0x01/0x02/0x03` uplinks and `0x81` downlinks, but the XIAO scaffold does not emit or consume that payload contract.
3. Provisioning mismatch: the browser flasher writes a config blob to a UF2 volume, while the firmware itself currently provisions over USB CDC JSON.
4. Missing implementation: the original drop referenced `butterfi_content.c` but did not include it.
5. Registration path still undefined: the repo has no concrete XIAO-specific device registration and credential injection flow yet.