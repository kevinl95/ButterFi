# Web Console

This is a static Chrome Web Serial client for the ButterFi USB framing protocol.
It is packaged as a small installable PWA with an offline app shell.

## Serve Locally

Web Serial requires a secure context. The simplest local option is localhost:

```bash
python3 -m http.server 4173 --directory web
```

Then open:

```text
http://localhost:4173
```

## What It Does

- connects to the ButterFi USB CDC ACM serial port
- sends host query, status, cancel, and resend frames
- parses ButterFi USB frames from the device
- assembles chunked Sidewalk response payloads into readable text
- shows device status, transfer progress, and a session log
- registers a service worker to cache the app shell for offline launch
- exposes the Chromium install prompt when the browser decides the app is installable

## Installability Notes

- serve the app from `http://localhost` or `https://`
- load the page once so the service worker can cache the shell assets
- in Chrome or Edge, use the in-app install button or the browser install affordance

The app shell can open offline after installation, but Web Serial device access still requires the browser and hardware to be available.

## Browser Support

- Google Chrome
- Microsoft Edge

Other browsers may not expose the Web Serial API.