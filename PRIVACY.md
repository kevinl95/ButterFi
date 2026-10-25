# ButterFi — Privacy & Data Practices

This document describes what ButterFi does and does not collect about the
students who use it, where that data lives, how long it is kept, and who can
see it. It is written for the school, library, or district that deploys ButterFi
and for the families of the students who use it.

ButterFi is **self-hosted**: each school runs its own cloud stack in its own AWS
account. There is no shared, ButterFi-operated server, and no student data ever
leaves the school's own account. The practices below describe that stack as it
ships by default.

## Summary

ButterFi is designed so that a student's browsing is seen by as few parties as
possible:

- **Websites the student reads never see the student.** Pages are fetched by the
  school's own server on the student's behalf, so outside websites see the
  school's server, not the child's device or location.
- **The neighbor's Echo/Ring that relays the signal cannot read it.** Traffic
  over Amazon Sidewalk is encrypted end to end; the neighbor whose gateway
  carries it only passes along scrambled data.
- **The browser keeps no lasting record.** The student's history lives only in
  the browser tab's memory. There are no cookies, no saved history, and no
  advertising or analytics trackers of any kind. Closing or reloading the tab —
  or pressing **New session** — erases it.
- **The school's own cloud keeps as little as possible, for as short as
  possible** (details below).

## What is collected, where it lives, and for how long

| Where | What is stored | Retention |
|-------|----------------|-----------|
| **Student's browser (Chromebook)** | Current session's page history, in memory only | Until the tab is reloaded/closed or **New session** is pressed |
| **School's activity logs (CloudWatch)** | Operational metadata only: device ID, request number, message type, query *length*, chunk counts. **Not** the search terms, URLs, or page content. | 7 days |
| **School's page store (DynamoDB)** | The text of the page being delivered, in chunks, keyed by device ID. **Not** the search term. | 2 hours (configurable), auto-deleted |
| **In-transit queue (SQS)** | The pending search/URL, in flight to the fetcher | Deleted the moment the request is processed (seconds) |

A note on the "page store": ButterFi delivers a page in many small pieces over a
slow radio link, and a device may re-request pieces it missed. The pieces are
held just long enough for that, then automatically deleted. By default that is 2
hours; the school can lower it.

## What is deliberately NOT collected

- **No search/URL text in the logs.** The school's activity logs record that a
  request happened and how big it was, never what was searched for.
- **No stored history of what each student searched.** The search term is used
  to fetch the page and then discarded; it is not written to the page store.
- **No cookies, no localStorage browsing history, no third-party analytics or
  ad trackers** in the student browser.
- **No student IP address or identity sent to outside websites.** The school's
  server fetches on the student's behalf.

## Who can see what

- **The school (as the account owner)** can see the operational metadata in its
  own logs and the page content transiently in its own page store, each keyed by
  a device ID. Whether a device ID can be tied back to a named student depends
  entirely on the school's own device records (see below). The school cannot see
  a student's past search terms, because those are not stored.
- **The neighbor's gateway and Amazon** relay the traffic but cannot read its
  contents (encrypted). Amazon's network sees connectivity metadata — which
  device connected, when, and message sizes — and the fact that a device is
  registered to the school's account. This is inherent to Amazon Sidewalk and
  cannot be removed.
- **Outside websites, DuckDuckGo, and Cloudflare DNS** see requests coming from
  the school's server, not from an identifiable student. (ButterFi resolves
  domains through [Cloudflare for Families](https://blog.cloudflare.com/introducing-1-1-1-1-for-families/),
  1.1.1.3, which filters malware and adult content.)

## Linking a device to a student

Nothing in the data above names a student — it is keyed to a device ID. A device
ID can be connected to a specific child only through the **provisioning records
the school itself creates** when it hands out dongles. To keep that link from
leaking into logs and data:

- **Do not put a student's name in a device's name.** Use neutral labels such as
  `room204-07` or a random tag.
- **Keep the device-to-student roster offline** — a sheet the librarian or IT
  staff holds — rather than inside AWS.

## Controls a school can adjust

- **`ChunkTTLHours`** (CloudFormation parameter, default `2`) — how long page
  content lingers in the page store before automatic deletion. Lower it to keep
  content at rest for even less time; raise it only if slow links need a longer
  window to re-request missing pieces.
- **Log retention** (`RetentionInDays`, default `7` on every log group) — how
  long operational metadata is kept.
- **Content policy** (allowlist / blocklist) — the school can restrict which
  sites are reachable. This is enforced on the school's server and is a safety
  control, not personal data. See the README's "Content policy" section.

## What cannot be avoided

- Amazon Sidewalk / AWS IoT Wireless sees device connectivity metadata and the
  device-to-account registration.
- Cloudflare's resolver sees the domain names the school's server looks up.
- As the account owner, a school *could* choose to add its own logging or
  analytics. ButterFi's defaults minimize collection; they cannot prevent an
  operator who deliberately sets out to monitor individual students. A school's
  own acceptable-use and student-privacy policies govern that choice.

---

*This document reflects the default behavior of the shipped stack
([template.yaml](template.yaml)) and web app ([web/](web/)). If a school
modifies either, it should update this document to match.*
