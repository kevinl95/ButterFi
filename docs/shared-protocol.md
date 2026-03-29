# ButterFi Shared Protocol

This document is the implementation contract between the cloud stack, the
RAK4630 firmware, and the Chrome Web Serial app.

## Milestone One Scope

- One active request at a time per device
- Chrome-based browsers only
- USB CDC ACM transport between browser and device
- Sidewalk transport between device and cloud
- Chunked text responses with explicit resend requests

## Sidewalk Frames

### Device to Cloud

All device-to-cloud frames use this header:

| Byte | Field | Notes |
|------|-------|-------|
| 0 | Message type | `0x01=query`, `0x02=resend`, `0x03=ack` |
| 1 | Request ID | Rolling `0-255` request identifier |
| 2..N | Payload | Type-specific payload |

#### Query Frame

- Message type: `0x01`
- Payload: UTF-8 query string
- Example payload: `what is photosynthesis`

#### Resend Frame

- Message type: `0x02`
- Payload length: exactly 1 byte
- Payload byte 0: next needed chunk index
- Semantics: request the cloud to resume sending chunks from that index

#### Ack Frame

- Message type: `0x03`
- Payload length: exactly 1 byte
- Payload byte 0: last received chunk index
- Semantics: informational for milestone one, not a transport guarantee

### Cloud to Device

| Byte | Field | Notes |
|------|-------|-------|
| 0 | Message type | `0x81=response chunk` |
| 1 | Request ID | Matches the request |
| 2 | Chunk index | `0-254` |
| 3 | Total chunks | `1-255` |
| 4..N | UTF-8 bytes | Text chunk payload |

## USB Serial Frames

USB serial uses a separate frame layer so the browser can recover from partial
reads and resynchronize without relying on line-oriented text parsing.

### Frame Layout

| Byte | Field | Notes |
|------|-------|-------|
| 0 | Sync 1 | `0x42` |
| 1 | Sync 2 | `0x46` |
| 2 | Version | `0x01` |
| 3 | Frame type | See registry below |
| 4 | Request ID | `0-255`, or `0` when not applicable |
| 5 | Flags | Reserved for now, send `0` |
| 6 | Payload length LSB | Little-endian payload length |
| 7 | Payload length MSB | Little-endian payload length |
| 8..N | Payload | Type-specific payload |
| N+1 | Checksum | XOR of bytes `2..N` |

### Browser to Device Frame Types

| Type | Name | Payload |
|------|------|---------|
| `0x01` | Host query submit | UTF-8 query string |
| `0x02` | Host resend request | 1 byte next needed chunk index |
| `0x03` | Host cancel request | Empty |
| `0x04` | Host ping | Optional opaque payload |
| `0x05` | Host status request | Empty |

### Device to Browser Frame Types

| Type | Name | Payload |
|------|------|---------|
| `0x81` | Device status | See status payload below |
| `0x82` | Uplink accepted | Empty |
| `0x83` | Response chunk | Sidewalk chunk payload without translation |
| `0x84` | Transfer complete | Empty |
| `0x85` | Transfer error | Error code + UTF-8 message |
| `0x86` | Pong | Optional opaque payload |

### Device Status Payload

| Byte | Field | Notes |
|------|-------|-------|
| 0 | Device state | `0=idle`, `1=serial-ready`, `2=sidewalk-starting`, `3=sidewalk-ready`, `4=busy`, `5=error` |
| 1 | Link state | `0=unknown`, `1=ble`, `2=fsk`, `3=lora` |
| 2 | Active request ID | `0` if none |

### Transfer Error Payload

| Byte | Field | Notes |
|------|-------|-------|
| 0 | Error code | See registry below |
| 1..N | UTF-8 message | Optional human-readable detail |

Error code registry:

- `0x01`: invalid host frame
- `0x02`: device busy
- `0x03`: sidewalk unavailable
- `0x04`: cloud fetch failed
- `0x05`: transfer timed out
- `0x06`: protocol mismatch

## Implementation Notes

- Firmware should forward `0x81` Sidewalk chunks to the browser as USB frame type
  `0x83` without rewriting the chunk body.
- The browser should maintain user-visible progress from `chunk index` and
  `total chunks`, but the firmware remains the authority for resend behavior.
- Milestone one should reject a second active host query while one request is in
  flight.
- If the browser disconnects and reconnects, the device should emit a status
  frame immediately after the serial session is re-established.