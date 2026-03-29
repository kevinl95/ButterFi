# ButterFi

Serverless backend for ButterFi — a USB dongle that gives Chromebooks internet-like
access over Amazon Sidewalk for students without home connectivity.

## Architecture

```
Chromebook ←WebSerial→ ButterFi Dongle ←Sidewalk→ Neighbor's Echo ←Internet→ AWS

                        ┌─────────────────────────────────────────┐
                        │  AWS (us-east-1)                        │
                        │                                         │
  Sidewalk Uplink ────→ │  IoT Core Rule → Uplink Lambda          │
                        │                      │                  │
                        │                      ▼                  │
                        │                  SQS Queue              │
                        │                      │                  │
                        │                      ▼                  │
                        │              Scraper Lambda              │
                        │               │          │              │
                        │               ▼          ▼              │
                        │          DynamoDB    Sidewalk Downlink   │
                        │          (chunks)    (first chunk)       │
                        │               │                         │
                        │               ▼                         │
  Sidewalk Downlink ←── │      Downlink Lambda                    │
  (on chunk request)    │      (resends from DynamoDB)            │
                        └─────────────────────────────────────────┘
```

## Message Protocol

The system has two related protocols:

- Sidewalk frames between the device and AWS
- USB serial frames between Chrome and the device

The authoritative shared contract for both is in [docs/shared-protocol.md](/home/mrmemory/ButterFi/docs/shared-protocol.md).

### Uplink (device → cloud)

| Byte | Field         | Values                                     |
|------|---------------|--------------------------------------------|
| 0    | Message type  | 0x01=query, 0x02=resend request, 0x03=ack  |
| 1    | Request ID    | 0-255, rolling counter                     |
| 2..N | Payload       | Type-specific payload                      |

Message payloads:

- `0x01`: UTF-8 query string
- `0x02`: 1 byte next needed chunk index
- `0x03`: 1 byte last received chunk index

### Downlink (cloud → device)

| Byte | Field         | Values                                     |
|------|---------------|--------------------------------------------|
| 0    | Message type  | 0x81=response chunk                        |
| 1    | Request ID    | Matches the uplink request                 |
| 2    | Chunk index   | 0-based                                    |
| 3    | Total chunks  | Total number of chunks for this response   |
| 4..N | Payload      | UTF-8 text content                         |

### Flow

1. Student types a query in the ButterFi PWA on their Chromebook
2. PWA sends query over Web Serial to the dongle
3. Dongle transmits uplink (type=0x01) over Sidewalk
4. IoT Core routes to Uplink Lambda → SQS → Scraper Lambda
5. Scraper fetches the page, strips to text, chunks into 236-byte pieces
6. Chunks stored in DynamoDB, first chunk sent as immediate downlink
7. Device receives chunk 0, forwards it to the browser, and requests more via type=0x02 with the next needed chunk index
8. Downlink Lambda reads from DynamoDB and sends next batch of chunks

## Implementation Tracks

- Firmware: RAK4630 Sidewalk client plus USB CDC ACM bridge
- Web app: Chrome-only ButterFi page using the Web Serial API
- Shared protocol: versioned binary frame contract for both workstreams

The initial firmware scaffold, adapter stubs, shim layer, and RAK port mapping are in [firmware/rak4630/README.md](/home/mrmemory/ButterFi/firmware/rak4630/README.md).

## Firmware Build

The portable rebuild flow for the ButterFi-integrated RAK4631 firmware is in [docs/firmware-build.md](/home/mrmemory/ButterFi/docs/firmware-build.md).

Use the checked-in scripts in `scripts/` to bootstrap the vendor west workspace, apply vendor patches, and build without hard-coding any machine-specific paths. If you are not already inside an nRF Connect SDK shell, point `NCS_ENV_JSON` at your local toolchain manager `environment.json` file before running the script.

## Browser App

The Chrome Web Serial client lives in [web/README.md](/home/mrmemory/ButterFi/web/README.md). It is a static installable PWA with no build step: serve the `web/` directory on localhost and open it in a Chromium-based browser.

## Deploy

```bash
# Prerequisites: AWS CLI configured with us-east-1 credentials

aws cloudformation deploy \
  --template-file template.yaml \
  --stack-name butterfi-dev \
  --capabilities CAPABILITY_NAMED_IAM \
  --region us-east-1 \
  --parameter-overrides Stage=dev
```

## Parameters

| Parameter              | Default              | Description                                  |
|------------------------|----------------------|----------------------------------------------|
| SidewalkDestinationName| ButterFiDestination  | IoT Core Sidewalk destination name           |
| MaxChunkBytes          | 240                  | Max bytes per downlink chunk (FSK=250 max)   |
| ScraperTimeoutSeconds  | 30                   | Scraper Lambda timeout                       |
| ChunkTTLHours          | 24                   | Hours before DynamoDB TTL cleans up chunks   |
| Stage                  | dev                  | Deployment stage (dev/staging/prod)          |

## Scaling Notes

- **DynamoDB**: PAY_PER_REQUEST mode scales automatically. No capacity planning needed.
- **SQS**: Decouples uplink processing from scraping. Scraper can take 15-30s per request
  without blocking the uplink handler.
- **Lambda concurrency**: Default account limits apply. For large deployments, request
  concurrency increases for the scraper Lambda.
- **Sidewalk rate limits**: The network itself is the bottleneck, not the backend.
  Downlink chunks are sent in small batches (3 at a time) to respect Sidewalk's
  rate enforcement.
- **Dead letter queue**: Failed scrapes land in the DLQ after 3 retries. CloudWatch
  alarm fires if messages accumulate.

## Cost Estimate (1,000 students, ~10 queries/day each)

- Lambda: ~$2-5/month (short invocations, small memory)
- DynamoDB: ~$1-3/month (on-demand, TTL keeps table small)
- SQS: ~$0.50/month
- IoT Core: Free tier covers Sidewalk message routing
- **Total: ~$5-10/month** (excluding any LLM API costs if you add one later)

## Future Enhancements

- [ ] Add LLM endpoint for direct question answering (skip scraping entirely)
- [ ] ButterFi-native content format for teacher-created pages
- [ ] Static site generator for ButterFi-optimized content
- [ ] Content caching layer (popular queries served from cache, no re-scrape)
- [ ] Usage analytics dashboard for school administrators
- [ ] OTA firmware update delivery over Sidewalk