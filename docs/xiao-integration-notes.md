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
- post-flash Sidewalk manufacturing-page write over USB frame `0x07`, chunked, writing directly to the `mfg_storage` flash partition and rebooting on success
- NVS-backed storage of `school_id`, `device_name`, and `content_pkg`
- Sidewalk initialization and status callbacks

## Integration Notes

1. USB runtime framing is aligned: both the browser console and the XIAO firmware now use the shared ButterFi binary frame layer.
2. The milestone-one query/resend/chunk contract is implemented end to end in source: the browser sends framed USB query/resend requests, the firmware emits Sidewalk `0x01`/`0x02` uplinks, and the cloud returns `0x81` chunk payloads that the firmware forwards back to the browser.
3. Provisioning is three-phase in the browser: the UF2 bootloader step writes the firmware image only, then the tool reconnects over runtime USB serial to write the Sidewalk manufacturing page (frame `0x07`, causes a reboot), reconnects again, and saves the classroom config into NVS (frame `0x06`). The JSON files left on the UF2 volume are audit artifacts, not the authoritative config or credential store. **This was originally a two-phase flow that merged the credential into the UF2 itself — that approach does not work on real hardware; see the hardware validation findings below.**
4. The legacy `butterfi_content.c`/`butterfi_content.h` scaffolding has been removed. Sidewalk message handling was already implemented directly in `main.c`; those files were dead code (compiled into Sidewalk builds but never called).
5. Registration path is partially closed: the browser provisioning flow resolves either a raw AWS `certificate.json` or a supplied Sidewalk manufacturing credential hex/bin into flat bytes and writes them to `mfg_storage` over runtime serial after the UF2 boots. The repo still includes `scripts/build-sidewalk-credential.py` as an admin-side wrapper around the official Sidewalk provisioner for CLI-only AWS exports and offline asset generation.
6. AWS stack output is not itself a device credential: the deployed CloudFormation stack exposes the Sidewalk destination name, while device-specific Sidewalk credential material still must be produced separately and then fed into the browser provisioning flow. **Sidewalk resources (device profiles, wireless devices) are only usable in `us-east-1` for at least some AWS accounts, even though general IoT Wireless API calls succeed in other regions like `us-west-2`. Deploy the stack in `us-east-1` unless you've confirmed otherwise for your account.**
7. First real hardware round-trip completed 2026-08-05 against a live `us-east-1` deployment — see [docs/hardware-validation-checklist.md](./hardware-validation-checklist.md) for the detailed results. Summary: USB framing, query/resend/chunk transport, and Sidewalk credential provisioning are confirmed working on real hardware. Open items:
   - **`0x06` config-save reliably hangs the device** (LED goes dark, device stops responding to all USB traffic until physically reset). Not caused by the credential-write work above — pre-existing, never previously exercised on hardware. Root cause unknown; needs a debug probe (SWD/RTT) to get real firmware logs, which wasn't available during this test.
   - **Sidewalk BLE registration did not complete** despite a valid credential present and the Echo Show gateway placed right next to the board. The BLE link to the gateway comes up then drops within the same second, so the device never time-syncs or registers (stays `SID_STATUS_NOT_REGISTERED` / `NO_TIME`; AWS device stays `PROVISIONED`, no cloud contact). A 2026-08-06 deep-dive fixed several real firmware bugs along the way — a stack-local `sid_config` (use-after-scope; now `static`), a missing `AUTO_CONNECT` BLE link policy in `sidewalk_init()`, a zero `CONFIG_HEAP_MEM_POOL_SIZE`, and added live Sidewalk telemetry over USB frame `0x87` — but none changed the instant link drop. Evidence points below the app layer (gateway/Amazon-network relay of our prototype device, or a board RF quirk); the definitive next test is stock Nordic `sid_end_device` firmware on a Nordic nRF52840 DK through the same Echo. Full analysis and the AWS diagnostic scaffolding left in place are in [docs/hardware-validation-checklist.md](./hardware-validation-checklist.md).