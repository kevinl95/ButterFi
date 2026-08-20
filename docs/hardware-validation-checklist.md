# Hardware Validation Checklist

The source contracts between firmware, cloud, and browser line up (see
[docs/xiao-integration-notes.md](./xiao-integration-notes.md)). This
checklist sequences a real hardware round trip — a physical XIAO board, a
real Amazon Sidewalk network, and a deployed AWS stack — so a failure is
localized to one layer at a time instead of debugging the whole chain at
once.

Run each step in order. Don't move to the next step until the current one
passes.

## Results as of 2026-08-05

A first real round trip was run against a live `us-east-1` deployment.
Steps 1-3 pass (after fixes made during this run). Step 4 (Sidewalk
pairing) did not complete; steps 5-8 were not reached. Two open bugs and
one AWS gotcha came out of this run:

- **Fixed during this run**: the original credential-provisioning design
  (merge the Sidewalk credential into the UF2, flash it as one file) does
  not work — the UF2 bootloader on this hardware only accepts writes to its
  single known app region and silently ignores/hangs on blocks targeting
  any other address, including `mfg_storage`. Replaced with a runtime-serial
  write (USB frame `0x07`, see `docs/shared-protocol.md`). Also fixed:
  `provision.html` treated the bootloader's mid-write disconnect (expected
  behavior on this bootloader) as a fatal error, and had two bugs in its
  post-reboot reconnect logic (a double-close that masked real errors, and
  retrying a stale `SerialPort` object instead of re-resolving it via
  `getPorts()`).
- **Open bug**: the `0x06` config-save frame reliably hangs the device (LED
  goes dark, no further USB responses, requires a physical reset to
  recover). Reproduced multiple times, independent of the credential-write
  work above. Root cause not found — needs a debug probe (SWD/RTT) for real
  firmware logs, which wasn't available during this test.
- **Open item (see 2026-08-06 deep-dive below)**: Sidewalk BLE registration
  did not complete. `link_state` flickers to `BLE` briefly but never holds
  long enough to reach `SID_STATE_READY`.
- **AWS gotcha**: Sidewalk device profile/wireless device creation returned
  `AccessDeniedException` in `us-west-2` for this account even though
  general IoT Wireless calls worked there — it only worked in `us-east-1`.
  Deploy the stack in `us-east-1` unless you've confirmed otherwise.
- **Cosmetic**: the firmware's USB diagnostic LEDs (yellow/magenta/cyan/
  white overlays in `led_thread_fn`) latch on and are only cleared by the
  *next* diagnostic event, not a timeout. A stale "USB RX error" yellow can
  sit there indefinitely from an old, harmless event and look like a hang.
  Don't trust the LED alone — confirm with `probe-butterfi-status.py`.

## Sidewalk registration deep-dive as of 2026-08-06 (still unresolved)

Second session, focused entirely on the registration block. Added live
Sidewalk telemetry over USB debug frame `0x87` (`on_status_changed` and
`on_send_error` now emit `SID state=.. reg=.. time=.. linkmask=..`), which is
the only reason any of this is observable without an SWD/RTT probe. Decode:
`reg=1` = NOT_REGISTERED, `time=1` = NO_TIME, `state=1` = NOT_READY,
`linkmask=0x1` = BLE link up (per `sid_api.h`). Confirmed correct against
Amazon's registration doc: success is reg=0/time=0/link=0.

## Session 2026-08-09 — narrowed to the FFN handshake (measured, not inferred)

Best-characterized state yet. What is now ruled out *by measurement*:

- **Not a reboot loop.** BOOT-frame counter test (0x87 boot summary re-arms
  its 20-emit counter only on a fresh boot): 0 BOOT frames over 40s on the
  already-running device, and the USB device number is stable across 10-min
  windows (a cold reboot re-enumerates). `gen=1` persists in the boot frame.
- **Beacon is correct.** A BLE scanner (nRF Connect) shows a well-formed
  Sidewalk FFN advertisement: service UUID 0xFE03, Amazon company ID 0x0171,
  18-byte manufacturer payload, local name `SID_APP` (from the Sidewalk PAL,
  not our CONFIG_BT_DEVICE_NAME). So credential construction, the radio, and
  `sid_start()` all work.
- **CloudWatch event pipe is confirmed working** — a test message published to
  a temp rule landed in `/butterfi/sidewalk-events`. So the empty event log is
  real evidence: Amazon's network never sees our device (no Proximity/reg
  events), consistent with the link dying before any cloud relay.
- **Echo Show is a confirmed BLE Sidewalk gateway** (per Amazon's gateway
  table). Rebooting it + waiting the full 10 min (Amazon's troubleshooting for
  "no success log") did NOT change the symptom.
- **Cloud de-register is N/A**: `deregister-wireless-device` returns
  ResourceNotFound because the device never registered (status stays
  PROVISIONED). No cloud-side "registered" state to desync with our wipe.

The actual finding: the SID_APP advertiser cycles its **resolvable private
address (RPA)** — one identity (identical manufacturer payload), many MACs,
several within a single scan (far faster than the 15-min RPA rotation). A
gateway connects to an address that then stops existing, so the FFN link dies
in under a second and nothing reaches the network. Config context:
`CONFIG_BT_PRIVACY=y` (Sidewalk `imply`s it) with `CONFIG_BT_SETTINGS` **off**
— a broken privacy config (IRK not persisted). Since the device is NOT
rebooting, the churn is advertising-restart, i.e. downstream of the handshake
failing — a symptom, not the root.

Attempted fix `CONFIG_BT_SETTINGS=y`: **faults at boot** (dark board, halts
with `CONFIG_RESET_ON_FATAL_ERROR=n`, does not recover on reset). It does not
coexist with the way the Sidewalk stack already owns the settings subsystem /
settings_storage — making it work would be its own integration task, and it
only treats the address symptom anyway. Reverted.

**Where it stands:** the FFN *handshake* fails (post-connection), the RPA churn
is downstream, and the failure reason is not visible over USB telemetry.
Everything reachable without more visibility is exhausted.

Building the stock `sid_end_device` sample **on the XIAO** was attempted and
abandoned: the sample is DK-only and sysbuild-forces MCUboot, so a XIAO port
needs SDK Kconfig patches (one malformed `MCUBOOT_FPROTECT...` entry), forcibly
disabling MCUboot (the XIAO's Adafruit bootloader has no `slot0_partition`),
and a hand-authored `xiao_ble` overlay for the sample's state-notifier GPIO
aliases + external flash — a multi-hour port with uncertain payoff.

**Recommended unlock: a Nordic nRF52840 DK (~$50).** It resolves both remaining
paths cleanly: (1) the sample builds/flashes on it with zero porting
(`west build -b nrf52840dk`) — run it through the same Echo to split "our XIAO
board/build" from "credential / gateway / Amazon network"; and (2) its onboard
J-Link is a full SWD probe — wire it to the XIAO SWD pads and read RTT off the
XIAO (the RTT firmware is already built/flashed) to get the exact FFN
disconnect reason. Two answers, one part.

## Third session update (2026-08-07, review-driven)

An external code review surfaced several real bugs; all fixed and verified on
hardware, but **registration still does not complete** (device stays
`PROVISIONED`).

- **`0x06` config-save hang = stack overflow, FIXED.** `usb_thread` had a
  1024-byte stack; the config-save path (`json_buffer[513]` + config structs +
  NVS/flash driver + `send_frame`'s ~519B frame) blows past it, and with
  `CONFIG_HW_STACK_PROTECTION=y` + no compiled-in log backend it faulted
  silently (dark LED). Bumped `USB_STACK_SIZE` to 4096; the device now
  survives `0x06`. NOTE: config-save still returns no `0x88` while the radio
  is up (USB thread stays alive, so it's not a crash) — consistent with
  `nvs_write` blocking on nRF flash/radio timeslot contention. Not on the
  browsing path; left as a follow-up.
- **Partition collision, FIXED.** `butterfi_config.c` mounted NVS on the DTS
  `storage_partition` (`0xEC000`) — the exact flash PM assigns to
  `settings_storage`, where Sidewalk persists its registration/time-sync keys
  (`CONFIG_SETTINGS_NVS=y`). `butterfi_config_load()` runs before
  `sidewalk_init()` and writes NVS sector headers on mount, corrupting the
  Sidewalk key store every boot. Added a dedicated `butterfi_storage` PM
  partition at `0xE8000` and repointed `butterfi_config` at it via
  `PM_BUTTERFI_STORAGE_ID`. After this, the registration signature changed
  from ~7s link flicker to quiet (plausibly the flicker was the stack
  thrashing on the corrupted store) — but it still does not register.
- **LFCLK = cleared, PROVEN.** New `0x87` boot frame reads
  `NRF_CLOCK->LFCLKSTAT` and reports `lfclk=Xtal,run` — the crystal is present
  and sourcing LFCLK, so the advertised 50 ppm SCA is accurate and the
  RC-fallback drift hypothesis does not apply to this unit. (Kept the readout;
  it's cheap and definitive.)
- Also: capped the oversized `BUTTERFI_SIDEWALK_UPLINK_MAX_PAYLOAD` (512→253,
  Sidewalk BLE max msg is 255), added a build-flag `#error`, emitted resolved
  build flags in the boot frame (`SW=1 DBG=0` confirmed), removed the dead
  `LOG_BACKED_RPC` typo and the no-op `LOG_PROCESS_THREAD_STACK_SIZE`.

**Still open after all of the above:** Sidewalk registration. The definitive
next test remains the stock Nordic `sid_end_device` sample on an **nRF52840
DK** through the same Echo (bisects XIAO-board/provisioning vs
gateway/Amazon-side). Deferred review follow-ups, in priority order: (1) get
**RTT** logging up (`CONFIG_USE_SEGGER_RTT` + `CONFIG_LOG_BACKEND_RTT`, XIAO
exposes SWD pads) — every session here is bottlenecked on not having firmware
logs; (2) `CONFIG_THREAD_ANALYZER` + `_AUTO` so stack overflows self-report;
(3) the two-thread `butterfi_usb_poll()` race (both `main` and `usb_thread`
call it, sharing `rx_ring`/`tx_ring`/parser with no lock — pick one owner);
(4) resolve the config-save flash/radio blocking; (5) move remaining large
stack buffers to static; (6) gate the `0x87` boot-info emission (currently
every 1s, which floods the debug channel during protocol tests).

**Consistent symptom across every change below:** device boots, actively
attempts BLE, the link comes up (`linkmask=0x1`) then drops in the *same
second*, `time` stays NO_TIME and `reg` stays NOT_REGISTERED. AWS device
status stays `PROVISIONED`; no uplink stats; no Lambda logs. The device never
completes time-sync (which registration requires).

**Firmware bugs found and fixed this session (kept — all genuine):**

1. **Stack-local `sid_config`** in `sidewalk_init()`. The Sidewalk stack
   retains the pointer passed to `sid_init()`, so a function-local config is
   a use-after-scope once the function returns — classic "no crash, no error,
   just nothing" failure mode. Now `static`. (Callbacks were already static.)
2. **Missing `AUTO_CONNECT` link policy.** The Nordic `sid_end_device`
   reference sets `SID_LINK_CONNECTION_POLICY_AUTO_CONNECT` +
   `sid_link_auto_connect_params` (BLE, 30s attempt timeout) right after
   `sid_start` under `CONFIG_SID_END_DEVICE_AUTO_CONN_REQ`; our firmware never
   did. Without it a BLE device only passively accepts brief gateway
   connections. Added in `sidewalk_init()` — confirmed active (connection
   retry cadence went from ~80s to ~7s), but did **not** fix registration.
3. **`CONFIG_HEAP_MEM_POOL_SIZE` was 0.** Gave it a 4096 pool. A real defect,
   but NOT the blocker (link still dropped). Also learned the actual Sidewalk
   heap is a separate knob, `CONFIG_SIDEWALK_HEAP_SIZE=5120` (already the
   default), so the Zephyr-pool experiments were largely the wrong knob.
   `CONFIG_MBEDTLS_HEAP_SIZE=4096` was tried and **reliably faults at early
   boot** (4/4 dead boots, LED never lights) — do not set it; Sidewalk crypto
   uses PSA/nrf_security (static memory), not the legacy mbedTLS heap.

**Ruled out this session (with live evidence):** app-level config (all of the
above), credential identity (the `mfg_storage` `.bin` header is `SID0`, SMSN
at offset 12 exactly matches the AWS `SidewalkManufacturingSn`, `.hex` targets
`0xEB000`, and `sid_init` succeeds), BLE buffer/param config (standard
Sidewalk defaults: `BT_MAX_CONN=1`, ACL 251, MTU 247, 4s supervision), and
distance (tested with the Echo Show literally on the desk next to the board).
LFCLK is `K32SRC_XTAL / 50PPM`; the XIAO board has an LFXO, and for a BLE-only
device even RC (~250ppm) is within Sidewalk's ±500ppm tolerance — so LFCLK is
an unlikely cause here (it matters mainly for sub-GHz, which the XIAO can't do
anyway).

**Where it points:** below the app layer — the BLE connection to the gateway
won't sustain long enough to time-sync/register, unchanged by any app fix.
Leading remaining hypotheses, in rough order: (a) the Echo Show gateway isn't
completing the relay of our **prototype**-cert device to Amazon's Sidewalk
network; (b) an Amazon-network-side authorization/propagation issue for the
device; (c) a board-level RF/BLE-timing quirk on the XIAO.

**The definitive next test (not yet run):** flash the **stock** Nordic
`sid_end_device` sample on a **Nordic nRF52840 DK** (the sample ships board
configs only for the DKs, not `xiao_ble`), provision fresh credentials, and
try to register through the same Echo. Registers → the problem is our XIAO
board or our provisioning; fails → it's the gateway / Amazon-network side, not
our device. Cheaper interim steps: verify the cloud-event plumbing actually
delivers (below) so a zero-events result becomes trustworthy, and a long
unattended soak near the Echo.

**AWS diagnostic scaffolding left in place** (for whoever resumes — safe/cheap
to leave, or delete when done):
- Device event notifications enabled on wireless device
  `1c455dd7-6b7b-4c74-b148-5f76d9f3a488` (Proximity, DeviceRegistrationState,
  MessageDeliveryStatus) — `aws iotwireless get-resource-event-configuration`.
- IoT topic rule `butterfi_sidewalk_events` (`SELECT * FROM
  '$aws/iotwireless/events/#'`) → CloudWatch log group
  `/butterfi/sidewalk-events` (1-day retention). During this session the log
  group stayed empty, but the event pipe was never confirmed working end to
  end, so empty is not yet proof the gateway isn't relaying. Tear down with
  `aws iot delete-topic-rule --rule-name butterfi_sidewalk_events` and
  `aws logs delete-log-group --log-group-name /butterfi/sidewalk-events`.

## 1. USB-only smoke test (no Sidewalk yet)

Build and flash the USB control debug profile, which skips Sidewalk startup
entirely so you can validate framing first:

```bash
BUTTERFI_USB_CONTROL_DEBUG=ON \
BUTTERFI_INCLUDE_SIDEWALK=OFF \
./scripts/build-xiao.sh rebuild
```

Flash `firmware/xiao_nrf52840/build/xiao_nrf52840/zephyr/zephyr.uf2` to the
board (UF2 bootloader drag-and-drop, or via `web/provision.html` — see step
2), then with the board enumerated as a CDC ACM serial port:

```bash
python3 scripts/probe-butterfi-status.py --tty /dev/ttyACM0
python3 scripts/probe-cdc-echo.py --tty /dev/ttyACM0 --payload ping
```

Expect: `probe-butterfi-status.py` decodes a `device_status` (`0x81`) frame
with `state=... Sidewalk not registered` or similar (Sidewalk isn't running
in this profile) and no checksum errors. If this step fails, the problem is
in USB framing/build config, not Sidewalk or the cloud — fix it here before
going further.

## 2. Browser flash (full provisioning flow)

Using a real board and a fresh Sidewalk-profile UF2, run the full
`web/provision.html` flow per
[docs/self-hosted-owner-setup.md](./self-hosted-owner-setup.md). This is
now a three-step flow, not two:

1. Serve `web/` locally (`python3 -m http.server 4173 --directory web`) or
   from HTTPS.
2. Select the release UF2 and a Sidewalk credential (`certificate.json`,
   `.hex`, or `.bin`).
3. Double-tap reset, click **Select UF2 Drive** fresh (a handle from an
   earlier bootloader session goes stale), then **Build and Write UF2**. A
   write error during this specific step is expected on this bootloader —
   it flashes and reboots as soon as it receives a complete image, which
   can cut the browser's write stream. The flow should still continue to
   the runtime-provisioning prompt.
4. Click **Connect Runtime Serial**. This writes the Sidewalk credential
   over USB frame `0x07`, waits for the device to reboot, reconnects
   automatically, then saves `school_id` / `device_name` / `content_pkg`
   over frame `0x06`.

Expect: the flash completes (ignoring the expected mid-write disconnect),
the device re-enumerates after the credential-write reboot, and the
config-save step returns a `config_saved` (`0x88`) frame. **As of
2026-08-05 the config-save step reliably hangs the device instead — see the
open bug above.** If you hit it, the credential write itself has already
succeeded and persisted; you don't need to redo it, just recover the board
(single-tap reset) and treat config-save as a known-broken step for now.

## 3. Stack deploy + Sidewalk device registration

Deploy the cloud stack and register a real Sidewalk wireless device against
it, per [docs/self-hosted-owner-setup.md](./self-hosted-owner-setup.md).
**Use `us-east-1`** — Sidewalk device profile/wireless device creation has
been observed to fail with `AccessDeniedException` in other regions (e.g.
`us-west-2`) even when general IoT Wireless API calls succeed there:

```bash
aws cloudformation deploy \
  --template-file template.yaml \
  --stack-name butterfi \
  --capabilities CAPABILITY_NAMED_IAM \
  --region us-east-1
```

Note the `SidewalkDestinationName` output, register the device against it in
AWS IoT Wireless, and confirm in the console that the destination's IoT rule
points at the deployed `SidewalkUplinkRule`. `aws iotwireless get-wireless-device`
returns the device's certificates and private keys directly — you don't need
the console's "download credentials" flow to get a usable credential.

## 4. Sidewalk pairing

Bring the device near a Sidewalk-enabled Echo or Ring device (confirm the
account-level Sidewalk toggle is on in the Alexa app: **More → Settings →
Account Settings → Amazon Sidewalk**). No mobile-app pairing action is
needed on the ButterFi device itself — it's not a per-device pairing flow,
it associates with any nearby enabled gateway automatically once it has a
valid credential and starts advertising.

Watch the board's RGB LED (see the `led_state_t` states in
`firmware/xiao_nrf52840/src/main.c`) — but see the cosmetic LED-latching
note above; confirm state with `probe-butterfi-status.py`, not the LED
alone:

- blue pulse → booting
- yellow blink → connecting
- **green solid → `SID_STATE_READY`, paired and online**
- red slow blink → unprovisioned/not registered
- red fast blink → error

Confirm via `probe-butterfi-status.py` that `device_status` reports
`state=Sidewalk ready` and `link=BLE`. **As of 2026-08-05, `link_state` was
observed flickering to `BLE` intermittently without ever holding — give it
significantly longer than a few minutes before concluding it's stuck, and
watch for changes with a long-running monitor rather than one-off polls**
(a stale poll can also spuriously trip the device's USB RX diagnostic LED
into a misleading state — see the cosmetic note above).

## 5. End-to-end query

From `web/browse.html`, connect to the device and submit a query (a plain
search term is the simplest first test). Trace it through CloudWatch Logs
for each Lambda, in order:

1. `/aws/lambda/<UplinkLambda>` — logs the decoded uplink (`type=0x01`,
   device ID, query text) and the SQS enqueue.
2. `ScrapeQueue` → `/aws/lambda/<ScraperLambda>` — logs the fetch, chunk
   count, and first downlink send.
3. `/aws/lambda/<DownlinkLambda>` — logs subsequent chunk resends if the
   device requests more.

Expect: the query renders as a readable page in `browse.html` (headings,
lists, clickable links) once the transfer completes, matching the chunk
count logged by the scraper.

## 6. Resend path

Force a dropped downlink — e.g. temporarily revoke
`iotwireless:SendDataToWirelessDevice` on `DownlinkLambdaRole` for one
resend attempt, or trigger a query for a long page so multiple resend
rounds are needed — and confirm the device's `0x02` resend uplink recovers
the transfer once the permission (or connectivity) is restored, without
requiring a fresh query from the browser.

## 7. DNS filter sanity check

The scraper Lambda forces DNS resolution through Cloudflare for Families
(1.1.1.3) as a content-safety net for student traffic (see the
`_cf_getaddrinfo` monkey-patch in the `ScraperLambda` inline code in
`template.yaml`). Before a pilot, fetch a handful of sites the classroom
will actually use and confirm none of them are false-positived by that
filter. If a needed site is blocked, that's a Cloudflare for Families
policy question, not a bug in this repo.

## 8. Record the result

Once all steps pass, tag the firmware build/commit used (e.g. a git tag or
a note in `artifacts/`) as the known-good validated build, and update
[docs/xiao-integration-notes.md](./xiao-integration-notes.md) item 7 to
reflect that hardware validation has been completed, with the date and
Sidewalk destination/region used.
