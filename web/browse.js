import { ButterfiDevice, deviceStateLabels } from "./protocol.js";
import { renderButterfiMarkup } from "./butterfi-markup.js";
import { BrotliDecode } from "./vendor/brotli-decode.js";

const elements = {
    connectButton: document.querySelector("#connect-button"),
    disconnectButton: document.querySelector("#disconnect-button"),
    installButton: document.querySelector("#install-button"),
    installStatus: document.querySelector("#install-status"),
    supportIndicator: document.querySelector("#support-indicator"),
    connectionBadge: document.querySelector("#connection-badge"),
    deviceStatePill: document.querySelector("#device-state-pill"),
    browseProgress: document.querySelector("#browse-progress"),
    browseBar: document.querySelector("#browse-bar"),
    browseBarFill: document.querySelector("#browse-bar-fill"),
    backButton: document.querySelector("#back-button"),
    addressInput: document.querySelector("#address-input"),
    goButton: document.querySelector("#go-button"),
    pageContent: document.querySelector("#page-content"),
    pdfButton: document.querySelector("#pdf-button"),
    newSessionButton: document.querySelector("#new-session-button"),
    printHeader: document.querySelector("#print-header"),
};

const device = new ButterfiDevice();

// Local navigation history: each entry is { query, text }. history[index] is
// the page currently shown. Back replays an already-fetched entry with no
// new query; Go always fetches and truncates any forward entries.
const nav = {
    history: [],
    index: -1,
    pendingQuery: null,
    // The page transfer being loaded. Kept here, not only on `device`, because a
    // disconnect drops device.transfer and an interrupted page should survive it.
    transfer: null,
    interrupted: false,
    // Pull-driver counters (see pullTick).
    waitTicks: 0,
    stallTicks: 0,
    resendGap: 1,
    nextResendTick: 1,
};

const pwaState = {
    installPromptEvent: null,
    serviceWorkerRegistration: null,
};

function setPill(element, text, tone) {
    element.textContent = text;
    element.className = "pill";
    element.classList.add(
        tone === "good" ? "pill-good" : tone === "warn" ? "pill-warn" : tone === "danger" ? "pill-danger" : "pill-muted"
    );
}

// Sidewalk is up. The firmware reports Busy (4) only while the link is up, so
// Ready or Busy both mean chunks can flow.
function deviceOnline() {
    const { deviceState } = device.deviceStatus;
    return device.connected && (deviceState === 3 || deviceState === 4);
}

function receivedCount(transfer) {
    return transfer.chunks.filter(Boolean).length;
}

// A page is loading while its transfer is incomplete and the dongle is on
// Sidewalk. A page stalled by a lost connection doesn't lock the controls: Go,
// Back, or New session replace it.
function isLoading() {
    return Boolean(nav.transfer) && !nav.transfer.complete && nav.pendingQuery !== null && deviceOnline();
}

function updateButtons() {
    const connected = device.connected;
    const reconnecting = device.reconnecting;
    const loading = isLoading();

    elements.connectButton.disabled = connected || reconnecting || !device.supported;
    elements.disconnectButton.disabled = !(connected || reconnecting || device.port);
    elements.addressInput.disabled = !connected || reconnecting;
    elements.goButton.disabled = !connected || reconnecting || loading || !elements.addressInput.value.trim();
    elements.backButton.disabled = nav.index <= 0 || loading;
    // Save PDF is available whenever a completed page is on screen (even after
    // disconnect) — it prints what's already rendered, no device needed.
    elements.pdfButton.disabled = loading || nav.index < 0;
    // New session is available whenever there's something to clear (a page in
    // history or text in the address bar) and nothing is mid-load.
    elements.newSessionButton.disabled = loading || (nav.index < 0 && !elements.addressInput.value.trim());
}

function showPlaceholder(message) {
    elements.pageContent.innerHTML = "";
    const p = document.createElement("p");
    p.className = "page-placeholder";
    p.textContent = message;
    elements.pageContent.append(p);
}

// Determinate load bar driven by received/total chunks. Especially useful for
// compressed pages, which can't render progressively — the bar is the only live
// feedback during the Sidewalk transfer.
function setBar(received, total) {
    const pct = total > 0 ? Math.min(100, Math.round((received / total) * 100)) : 0;
    elements.browseBar.hidden = false;
    elements.browseBarFill.style.width = `${pct}%`;
    elements.browseBar.setAttribute("aria-valuenow", String(pct));
}

function hideBar() {
    elements.browseBar.hidden = true;
    elements.browseBarFill.style.width = "0%";
    elements.browseBar.setAttribute("aria-valuenow", "0");
}

function renderPage(text) {
    elements.pageContent.innerHTML = "";
    const { fragment } = renderButterfiMarkup(text);
    elements.pageContent.append(fragment);
}

// ── Response payload decode ───────────────────────────────────────────────
// The scraper sends [format][data] chunked as opaque bytes: format 0x00 = plain
// UTF-8, 0x01 = gzip, 0x02 = brotli. Reassemble the chunk bytes, read the format,
// decode once. A compressed stream can't be decoded partially, so compressed
// transfers render only when complete; plain transfers still render
// progressively from the prefix. Brotli is the scraper's normal format (gzip only
// if its Brotli layer is missing). Chrome has no native brotli
// DecompressionStream, so it uses Google's reference decoder, vendored in
// ./vendor/.
const RESP_FORMAT_PLAIN = 0x00;
const RESP_FORMAT_GZIP = 0x01;
const RESP_FORMAT_BROTLI = 0x02;
const pageDecoder = new TextDecoder();

// Concatenate chunk byte arrays. contiguousOnly stops at the first gap (for
// progressive plain rendering); otherwise skips gaps (all present at complete).
function concatChunks(transfer, contiguousOnly) {
    const parts = [];
    for (let i = 0; i < transfer.chunks.length; i++) {
        const c = transfer.chunks[i];
        if (!c) {
            if (contiguousOnly) break;
            continue;
        }
        parts.push(c);
    }
    let total = 0;
    for (const p of parts) total += p.length;
    const out = new Uint8Array(total);
    let off = 0;
    for (const p of parts) {
        out.set(p, off);
        off += p.length;
    }
    return out;
}

async function inflateGzip(bytes) {
    const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip"));
    return new Uint8Array(await new Response(stream).arrayBuffer());
}

function decodeBrotli(bytes) {
    const out = BrotliDecode(new Int8Array(bytes.buffer, bytes.byteOffset, bytes.length));
    return new Uint8Array(out.buffer, out.byteOffset, out.length);
}

// Decoded page text, or null when it can't be decoded yet (partial compressed
// stream, or chunk 0 not received, or a corrupt/incomplete stream).
async function decodeTransfer(transfer, { partial }) {
    const first = transfer.chunks[0];
    if (!first || first.length < 1) {
        return null; // need chunk 0 for the format byte
    }
    const format = first[0];
    if (format === RESP_FORMAT_GZIP || format === RESP_FORMAT_BROTLI) {
        if (partial) {
            return null; // can't decode a partial compressed stream
        }
        try {
            const body = concatChunks(transfer, false).subarray(1);
            const bytes = format === RESP_FORMAT_BROTLI ? decodeBrotli(body) : await inflateGzip(body);
            return pageDecoder.decode(bytes);
        } catch {
            return null;
        }
    }
    // Plain UTF-8 — decode the contiguous prefix (progressive) or all of it.
    return pageDecoder.decode(concatChunks(transfer, partial).subarray(1));
}

function pushHistory(query, text) {
    nav.history = nav.history.slice(0, nav.index + 1);
    nav.history.push({ query, text });
    nav.index = nav.history.length - 1;
}

// Shared-device privacy: wipe this student's in-memory browsing history and the
// on-screen page so the next person on a shared Chromebook can't see what they
// looked at. History is never persisted (no localStorage/cookies), so once this
// runs — or the tab is reloaded — nothing of the session remains client-side.
function clearSession() {
    abandonPage();
    nav.history = [];
    nav.index = -1;
    nav.awaitingReady = false;
    clearTimeout(nav.readyTimer);
    elements.addressInput.value = "";
    elements.browseProgress.textContent = "";
    elements.printHeader.textContent = "";
    hideBar();
    showPlaceholder(device.connected
        ? "Type a web address or search above and press Go."
        : "Connect a device, then type a web address or search above and press Go.");
    updateButtons();
}

function showHistoryEntry(index) {
    const entry = nav.history[index];
    if (!entry) {
        return;
    }
    nav.index = index;
    elements.addressInput.value = entry.query;
    renderPage(entry.text);
    updateButtons();
}

async function navigate(query) {
    const trimmed = query.trim();
    if (!trimmed || !device.connected) {
        return;
    }

    if (isLoading()) {
        return;
    }

    // A new page replaces one stalled by a lost connection. Also release any
    // request the device is still tracking from a page that died with the link
    // down — otherwise it would answer this query with "Device busy" (and,
    // reporting Busy rather than Ready, never let a queued query fire).
    const stalled = nav.transfer;
    abandonPage();
    const leftover = device.deviceStatus.activeRequest;
    if (leftover && (!stalled || leftover !== stalled.requestId)) {
        device.releaseRequest(leftover);
    }

    nav.pendingQuery = trimmed;
    nav.awaitingReady = false;
    clearTimeout(nav.readyTimer);
    elements.addressInput.value = trimmed;
    updateButtons();

    // The firmware only accepts a query while Sidewalk is READY. After an idle
    // period the device drops the gateway link and reconnects on its own, so if
    // it isn't ready yet, hold the query and let the status handler fire it once
    // the device reaches READY — instead of failing immediately.
    if (device.deviceStatus?.deviceState !== 3) {
        nav.awaitingReady = true;
        elements.browseProgress.textContent = "Connecting to Sidewalk…";
        showPlaceholder("Connecting to Sidewalk… (the dongle reconnects after being idle)");
        nav.readyTimer = setTimeout(() => {
            if (nav.awaitingReady) {
                nav.awaitingReady = false;
                nav.pendingQuery = null;
                elements.browseProgress.textContent = "";
                showPlaceholder("Sidewalk didn't reconnect in time — press Go to try again.");
                updateButtons();
            }
        }, 90000);
        return;
    }

    elements.browseProgress.textContent = "Requesting…";
    showPlaceholder("Requesting…");
    try {
        await device.sendQuery(trimmed);
    } catch (error) {
        elements.browseProgress.textContent = "";
        showPlaceholder(`Could not send request: ${error.message}`);
        updateButtons();
    }
}

function goToLink(anchor) {
    const url = anchor.dataset.ref ? anchor.href : null;
    if (!url) {
        return;
    }
    navigate(url);
}

device.addEventListener("connection-change", (event) => {
    // A partly received page is kept through disconnects; the pull driver
    // resumes it once the dongle is back on Sidewalk.
    const partial = hasPartialPage();

    if (event.detail.connected) {
        setPill(elements.connectionBadge, "Connected", "good");
        setPill(elements.deviceStatePill, "Connecting…", "warn");
        if (partial) {
            showInterrupted(nav.transfer);
        } else {
            if (nav.history.length === 0) {
                showPlaceholder("Type a web address or search above and press Go.");
            }
            if (!nav.awaitingReady) {
                elements.browseProgress.textContent = "";
            }
        }
    } else if (event.detail.reconnecting) {
        setPill(elements.connectionBadge, "Reconnecting", "warn");
        setPill(elements.deviceStatePill, "Reconnecting…", "warn");
        if (partial) {
            showInterrupted(nav.transfer);
        } else {
            if (nav.history.length === 0) {
                showPlaceholder("Device connection dropped. Reconnecting…");
            }
            if (!nav.awaitingReady) {
                elements.browseProgress.textContent = "Device sleeping — reconnecting…";
            }
        }
    } else if (partial) {
        setPill(elements.connectionBadge, "Disconnected", "muted");
        setPill(elements.deviceStatePill, "Not connected", "muted");
        showInterrupted(nav.transfer);
    } else {
        setPill(elements.connectionBadge, "Disconnected", "muted");
        setPill(elements.deviceStatePill, "Not connected", "muted");
        stopPull();
        nav.transfer = null;
        hideBar();
        if (nav.history.length === 0) {
            showPlaceholder("Device disconnected. Click “Connect device” to reconnect, then type an address and press Go.");
            elements.addressInput.value = "";
            elements.browseProgress.textContent = "";
        } else {
            elements.browseProgress.textContent = "Device disconnected. Reconnect to load another page.";
        }
        nav.awaitingReady = false;
        nav.pendingQuery = null;
        clearTimeout(nav.readyTimer);
    }
    updateButtons();
});

device.addEventListener("status", (event) => {
    const { deviceState } = event.detail;
    const label = deviceStateLabels[deviceState] ?? `State ${deviceState}`;
    const tone = deviceState === 3 ? "good" : deviceState === 5 ? "danger" : "warn";
    setPill(elements.deviceStatePill, label, tone);

    // Fire a query that was held while the device (re)connected to Sidewalk.
    // nav.transfer, not device.transfer: a stray chunk for an abandoned request
    // can recreate device.transfer and would block the queued query forever.
    const transferring = nav.transfer && !nav.transfer.complete;
    if (deviceState === 3 && nav.awaitingReady && nav.pendingQuery && !transferring) {
        nav.awaitingReady = false;
        clearTimeout(nav.readyTimer);
        const queued = nav.pendingQuery;
        elements.browseProgress.textContent = "Requesting…";
        showPlaceholder("Requesting…");
        device.sendQuery(queued).catch((error) => {
            elements.browseProgress.textContent = "";
            showPlaceholder(`Could not send request: ${error.message}`);
            updateButtons();
        });
    }

    // Online/offline changes whether a stalled page locks the controls.
    updateButtons();
});

// ── Multi-chunk pull driver ──────────────────────────────────────────────
// The scraper sends only chunk 0 and stores the rest in DynamoDB; the device
// re-requests remaining chunks ONLY when the host asks (a 0x02 resend request,
// served by the DownlinkLambda). Drive that pull here: kick it off when a
// transfer starts, then re-request the next missing chunk whenever the stream
// stalls, until the page is complete. Sidewalk downlinks are rate-limited, so
// large pages trickle in over several seconds rather than arriving at once.
//
// The pull survives interruptions. While the dongle is unplugged or off
// Sidewalk it waits, keeping the pieces already here; once Sidewalk is back the
// next tick asks for the rest. The cloud keeps chunks for ChunkTTLHours, and the
// device adopts the request even if it rebooted in between.
let pullTimer = null;
let lastReceived = -1;  // -1: first tick after a start or resume — ask right away
// Ticks are 2.5s. Stop a page whose chunk 0 hasn't come after a minute online
// (generous: the scraper alone can take most of ScraperTimeoutSeconds).
const FIRST_CHUNK_TIMEOUT_TICKS = 24;
// While online but no new chunk arrives, re-request with doubling gaps up to
// once a minute, and stop after 10 minutes. A dead transfer (chunks past
// ChunkTTLHours, or a link that reads Ready but carries nothing) then doesn't
// keep spending Sidewalk airtime on a neighbor's gateway. Time offline doesn't
// count — that's an interruption, not a stall.
const MAX_RESEND_GAP_TICKS = 24;
const GIVE_UP_STALLED_TICKS = 240;

function stopPull() {
    if (pullTimer !== null) {
        clearInterval(pullTimer);
        pullTimer = null;
    }
}

function resetStall() {
    nav.stallTicks = 0;
    nav.resendGap = 1;
    nav.nextResendTick = 1;
}

function startPull() {
    stopPull();
    lastReceived = -1;
    nav.waitTicks = 0;
    resetStall();
    pullTimer = setInterval(pullTick, 2500);
}

function pullTick() {
    const transfer = nav.transfer;
    if (!transfer || transfer.complete || nav.pendingQuery === null) {
        stopPull();
        return;
    }

    if (!deviceOnline()) {
        // Wait it out, and re-request on the first tick after Sidewalk is back.
        lastReceived = -1;
        nav.waitTicks = 0;
        resetStall();
        showInterrupted(transfer);
        return;
    }

    if (device.transfer !== transfer) {
        device.resumeTransfer(transfer);  // a disconnect dropped it; re-attach
    }

    const received = receivedCount(transfer);
    if (nav.interrupted) {
        nav.interrupted = false;
        elements.browseProgress.textContent = transfer.totalChunks > 0
            ? `Resuming… ${received} / ${transfer.totalChunks} pieces`
            : "Resuming…";
        updateButtons();
    }

    if (transfer.totalChunks === 0) {
        // Waiting for chunk 0, which is deliberately never re-requested. Request
        // IDs restart at 1 on every page load, and the scraper clears a request
        // ID's old chunks only after it finishes fetching — so a resend before
        // the first chunk could return a page from an earlier session.
        nav.waitTicks += 1;
        if (nav.waitTicks >= FIRST_CHUNK_TIMEOUT_TICKS) {
            giveUpPage("Stopped: no response for this page.");
        }
        return;
    }

    if (received >= transfer.totalChunks) {
        return;
    }

    // Kick off on the first tick, then re-request only when stalled (no new
    // chunk since the last tick) so an in-flight burst can flow first.
    const kickoff = lastReceived === -1;
    if (!kickoff && received > lastReceived) {
        lastReceived = received;
        resetStall();
        return;
    }
    lastReceived = received;
    nav.stallTicks += 1;
    if (nav.stallTicks >= GIVE_UP_STALLED_TICKS) {
        giveUpPage("Stopped: the rest of this page isn't arriving.");
        return;
    }
    if (kickoff || nav.stallTicks >= nav.nextResendTick) {
        device.requestMissingChunk().catch(() => {});
        nav.nextResendTick = nav.stallTicks + nav.resendGap;
        nav.resendGap = Math.min(nav.resendGap * 2, MAX_RESEND_GAP_TICKS);
    }
}

// Stop a page that has stopped arriving while the dongle is online. Whatever
// already rendered stays on screen.
function giveUpPage(message) {
    abandonPage();
    elements.browseProgress.textContent = `${message} Press Go to try again.`;
    updateButtons();
}

function hasPartialPage() {
    return nav.pendingQuery !== null && Boolean(nav.transfer) && !nav.transfer.complete
        && receivedCount(nav.transfer) > 0;
}

function showInterrupted(transfer) {
    nav.interrupted = true;
    const kept = transfer.totalChunks > 0
        ? ` (${receivedCount(transfer)} of ${transfer.totalChunks} pieces kept)`
        : "";
    elements.browseProgress.textContent = device.connected
        ? `Lost the Sidewalk connection — the page will finish loading when the dongle reconnects${kept}.`
        : `Dongle disconnected — reconnect it to finish loading the page${kept}.`;
    updateButtons();
}

// Give up on the page being loaded (a new query, Back, or New session).
function abandonPage() {
    if (nav.transfer && !nav.transfer.complete) {
        device.abandonTransfer(nav.transfer);
    }
    stopPull();
    nav.transfer = null;
    nav.pendingQuery = null;
    nav.interrupted = false;
    hideBar();
}

device.addEventListener("transfer-start", () => {
    nav.transfer = device.transfer;
    if (!nav.transfer) {
        return;  // a cancel, not a new page
    }
    nav.interrupted = false;
    elements.browseProgress.textContent = "Loading…";
    setBar(0, 0);
    showPlaceholder("Loading…");
    startPull();
    updateButtons();
});

device.addEventListener("chunk", async () => {
    const transfer = device.transfer;
    // Only the page being loaded — not stray chunks for an abandoned request.
    if (!transfer || transfer !== nav.transfer) {
        return;
    }
    const received = receivedCount(transfer);
    elements.browseProgress.textContent = `Loading… ${received} / ${transfer.totalChunks} pieces`;
    setBar(received, transfer.totalChunks);
    // Render incrementally as pieces arrive for PLAIN pages (headings, lists,
    // links fill in; unresolved >N[label] links resolve once the trailing link
    // table arrives). decodeTransfer returns null for a compressed page (can't
    // decode a partial stream), so those keep the progress meter until "complete".
    const text = await decodeTransfer(transfer, { partial: true });
    if (text !== null && transfer === nav.transfer && !transfer.complete) {
        renderPage(text);
    }
});

device.addEventListener("complete", async () => {
    const transfer = device.transfer;
    const query = nav.pendingQuery;
    if (!transfer || transfer !== nav.transfer || !query) {
        return;
    }
    stopPull();

    const text = await decodeTransfer(transfer, { partial: false });
    if (nav.transfer !== transfer) {
        return;  // replaced (Go / New session) while decoding
    }
    nav.pendingQuery = null;
    nav.transfer = null;
    elements.browseProgress.textContent = "";
    hideBar();
    if (text === null) {
        showPlaceholder("Could not decode the page (corrupt or incomplete response).");
        updateButtons();
        return;
    }
    pushHistory(query, text);
    renderPage(text);
    updateButtons();
});

// Link-loss errors: Sidewalk unavailable, send failed, timed out. On a page
// that's already partly here they don't end it — the pieces are kept and the
// pull resumes once Sidewalk is back (the cloud still holds the chunks).
const RESUMABLE_ERRORS = new Set([0x03, 0x04, 0x05]);

device.addEventListener("error", (event) => {
    const { requestId, errorCode, description, detail } = event.detail;
    const transfer = nav.transfer;
    // Only errors about the page being loaded matter. Others are noise — e.g.
    // the device answering the release of a finished or abandoned request.
    if (nav.pendingQuery === null || !transfer || requestId !== transfer.requestId) {
        return;
    }

    if (receivedCount(transfer) > 0 && RESUMABLE_ERRORS.has(errorCode)) {
        showInterrupted(transfer);
        return;
    }

    stopPull();
    nav.pendingQuery = null;
    nav.transfer = null;
    elements.browseProgress.textContent = "";
    hideBar();
    showPlaceholder(`${description}${detail ? ` — ${detail}` : ""}`);
    updateButtons();
});

elements.connectButton.addEventListener("click", () => {
    device.connect().catch((error) => showPlaceholder(`Connect failed: ${error.message}`));
});

elements.disconnectButton.addEventListener("click", () => {
    device.disconnect().catch(() => {});
});

elements.goButton.addEventListener("click", () => {
    navigate(elements.addressInput.value);
});

elements.pdfButton.addEventListener("click", () => {
    // Put the current page's address on the printed output, then let the
    // browser's print dialog "Save as PDF". Print styles hide the app chrome.
    const entry = nav.history[nav.index];
    elements.printHeader.textContent = entry ? entry.query : (elements.addressInput.value || "");
    window.print();
});

elements.addressInput.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
        navigate(elements.addressInput.value);
    }
});

elements.newSessionButton.addEventListener("click", clearSession);

elements.addressInput.addEventListener("input", updateButtons);

elements.backButton.addEventListener("click", () => {
    if (nav.index > 0) {
        abandonPage();  // a page stalled by a lost connection; Back replaces it
        elements.browseProgress.textContent = "";
        showHistoryEntry(nav.index - 1);
    }
});

elements.pageContent.addEventListener("click", (event) => {
    const anchor = event.target.closest("a");
    if (!anchor) {
        return;
    }
    event.preventDefault();
    goToLink(anchor);
});

async function registerServiceWorker() {
    if (!("serviceWorker" in navigator)) {
        updateInstallUi();
        return;
    }

    try {
        pwaState.serviceWorkerRegistration = await navigator.serviceWorker.register("./sw.js", { scope: "./" });
    } catch (error) {
        // Offline shell is a nice-to-have; browsing still works without it.
    }

    updateInstallUi();
}

function updateInstallUi() {
    const installed = window.matchMedia("(display-mode: standalone)").matches;

    if (installed) {
        elements.installButton.hidden = true;
        elements.installStatus.textContent = "Installed in standalone mode";
        return;
    }

    if (pwaState.installPromptEvent) {
        elements.installButton.hidden = false;
        elements.installStatus.textContent = "Ready to install on this device";
        return;
    }

    elements.installButton.hidden = true;
    elements.installStatus.textContent = "serviceWorker" in navigator
        ? "Preparing offline shell for installation"
        : "Install unavailable in this browser";
}

window.addEventListener("beforeinstallprompt", (event) => {
    event.preventDefault();
    pwaState.installPromptEvent = event;
    updateInstallUi();
});

window.addEventListener("appinstalled", () => {
    pwaState.installPromptEvent = null;
    updateInstallUi();
});

elements.installButton.addEventListener("click", async () => {
    if (!pwaState.installPromptEvent) {
        return;
    }
    const promptEvent = pwaState.installPromptEvent;
    pwaState.installPromptEvent = null;
    updateInstallUi();
    await promptEvent.prompt();
    await promptEvent.userChoice;
    updateInstallUi();
});

if (!device.supported) {
    elements.supportIndicator.textContent = "Web Serial is not available in this browser. Use Chrome or Edge.";
    setPill(elements.connectionBadge, "Unsupported browser", "danger");
} else {
    elements.supportIndicator.textContent = "Web Serial is available in this browser.";
}

updateButtons();
registerServiceWorker();
