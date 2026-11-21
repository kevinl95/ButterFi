# Vendored third-party code

## brotli-decode.js

Google's reference Brotli decoder in plain JavaScript (an ES module exporting
`BrotliDecode`). `browse.js` uses it to decode response format `0x02`. Chrome
has no native brotli `DecompressionStream`, so the student browser ships its own
decoder. It's served from GitHub Pages and cached by the service worker, never
sent over Sidewalk.

- Source: `js/decode.js` from https://github.com/google/brotli at tag `v1.2.0`
  (fetched via `https://cdn.jsdelivr.net/gh/google/brotli@v1.2.0/js/decode.js`)
- SHA-256: `7fd3ecb658c9c7e389eaf0aaf7e0adcc8ba47e043c21bdefb8c3caa83d893d35`
- License: MIT, see `brotli-LICENSE`
- Unmodified. It has no imports, network access or DOM access.

To update it, fetch the new tag's `js/decode.js`, record the tag and SHA-256
here, and bump `CACHE_NAME` in `web/sw.js`.
