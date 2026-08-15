import { ButterfiDevice, deviceStateLabels } from "./protocol.js";
import { renderButterfiMarkup } from "./butterfi-markup.js";

const elements = {
    connectButton: document.querySelector("#connect-button"),
    disconnectButton: document.querySelector("#disconnect-button"),
    installButton: document.querySelector("#install-button"),
    installStatus: document.querySelector("#install-status"),
    supportIndicator: document.querySelector("#support-indicator"),
    connectionBadge: document.querySelector("#connection-badge"),
    deviceStatePill: document.querySelector("#device-state-pill"),
    browseProgress: document.querySelector("#browse-progress"),
    backButton: document.querySelector("#back-button"),
    addressInput: document.querySelector("#address-input"),
    goButton: document.querySelector("#go-button"),
    pageContent: document.querySelector("#page-content"),
    pdfButton: document.querySelector("#pdf-button"),
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

function updateButtons() {
    const connected = device.connected;
    const reconnecting = device.reconnecting;
    const loading = Boolean(device.transfer) && !device.transfer.complete;

    elements.connectButton.disabled = connected || reconnecting || !device.supported;
    elements.disconnectButton.disabled = !(connected || reconnecting || device.port);
    elements.addressInput.disabled = !connected || reconnecting;
    elements.goButton.disabled = !connected || reconnecting || loading || !elements.addressInput.value.trim();
    elements.backButton.disabled = nav.index <= 0 || loading;
    // Save PDF is available whenever a completed page is on screen (even after
    // disconnect) — it prints what's already rendered, no device needed.
    elements.pdfButton.disabled = loading || nav.index < 0;
}

function showPlaceholder(message) {
    elements.pageContent.innerHTML = "";
    const p = document.createElement("p");
    p.className = "page-placeholder";
    p.textContent = message;
    elements.pageContent.append(p);
}

function renderPage(text) {
    elements.pageContent.innerHTML = "";
    const { fragment } = renderButterfiMarkup(text);
    elements.pageContent.append(fragment);
}

function pushHistory(query, text) {
    nav.history = nav.history.slice(0, nav.index + 1);
    nav.history.push({ query, text });
    nav.index = nav.history.length - 1;
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

    const loading = Boolean(device.transfer) && !device.transfer.complete;
    if (loading) {
        return;
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
    if (event.detail.connected) {
        setPill(elements.connectionBadge, "Connected", "good");
        setPill(elements.deviceStatePill, "Connecting…", "warn");
        if (nav.history.length === 0) {
            showPlaceholder("Type a web address or search above and press Go.");
        }
        if (!nav.awaitingReady) {
            elements.browseProgress.textContent = "";
        }
    } else if (event.detail.reconnecting) {
        setPill(elements.connectionBadge, "Reconnecting", "warn");
        setPill(elements.deviceStatePill, "Reconnecting…", "warn");
        if (nav.history.length === 0) {
            showPlaceholder("Device connection dropped. Reconnecting…");
        }
        if (!nav.awaitingReady) {
            elements.browseProgress.textContent = "Device sleeping — reconnecting…";
        }
    } else {
        setPill(elements.connectionBadge, "Disconnected", "muted");
        setPill(elements.deviceStatePill, "Not connected", "muted");
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
    const transferring = device.transfer && !device.transfer.complete;
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
});

// ── Multi-chunk pull driver ──────────────────────────────────────────────
// The scraper sends only chunk 0 and stores the rest in DynamoDB; the device
// re-requests remaining chunks ONLY when the host asks (a 0x02 resend request,
// served by the DownlinkLambda). Drive that pull here: kick it off when a
// transfer starts, then re-request the next missing chunk whenever the stream
// stalls, until the page is complete. Sidewalk downlinks are rate-limited, so
// large pages trickle in over several seconds rather than arriving at once.
let pullTimer = null;
let lastReceived = -1;

function stopPull() {
    if (pullTimer !== null) {
        clearInterval(pullTimer);
        pullTimer = null;
    }
}

function startPull() {
    stopPull();
    lastReceived = -1;
    pullTimer = setInterval(() => {
        const transfer = device.transfer;
        if (!transfer || transfer.complete) {
            stopPull();
            return;
        }
        const received = transfer.chunks.filter(Boolean).length;
        const hasGaps = transfer.totalChunks > 0 && received < transfer.totalChunks;
        // Kick off on the first tick, then re-request only when stalled (no new
        // chunk since the last tick) so an in-flight burst can flow first.
        if (hasGaps && (lastReceived === -1 || received === lastReceived)) {
            device.requestMissingChunk().catch(() => {});
        }
        lastReceived = received;
    }, 2500);
}

device.addEventListener("transfer-start", () => {
    elements.browseProgress.textContent = "Loading…";
    showPlaceholder("Loading…");
    startPull();
    updateButtons();
});

device.addEventListener("chunk", () => {
    const transfer = device.transfer;
    if (!transfer) {
        return;
    }
    const received = transfer.chunks.filter(Boolean).length;
    elements.browseProgress.textContent = `Loading… ${received} / ${transfer.totalChunks} pieces`;
    // Render the ButterFi markup incrementally as pieces arrive (headings,
    // lists, links) rather than showing plain text and snapping to styled at
    // the end. renderButterfiMarkup handles a partial document: inline >N[label]
    // links render as unresolved (plain, non-clickable) until the trailing link
    // table arrives in a later piece, then resolve on the next re-render.
    renderPage(transfer.chunks.map((chunk) => chunk ?? "").join(""));
});

device.addEventListener("complete", () => {
    stopPull();
    const transfer = device.transfer;
    if (!transfer || !nav.pendingQuery) {
        return;
    }

    const text = transfer.chunks.map((chunk) => chunk ?? "").join("");
    pushHistory(nav.pendingQuery, text);
    nav.pendingQuery = null;
    elements.browseProgress.textContent = "";
    renderPage(text);
    updateButtons();
});

device.addEventListener("error", (event) => {
    stopPull();
    const { description, detail } = event.detail;
    nav.pendingQuery = null;
    elements.browseProgress.textContent = "";
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

elements.addressInput.addEventListener("input", updateButtons);

elements.backButton.addEventListener("click", () => {
    if (nav.index > 0) {
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
