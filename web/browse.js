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
    const loading = Boolean(device.transfer) && !device.transfer.complete;

    elements.connectButton.disabled = connected || !device.supported;
    elements.disconnectButton.disabled = !connected;
    elements.addressInput.disabled = !connected;
    elements.goButton.disabled = !connected || loading || !elements.addressInput.value.trim();
    elements.backButton.disabled = nav.index <= 0 || loading;
}

function showPlaceholder(message) {
    elements.pageContent.innerHTML = "";
    const p = document.createElement("p");
    p.className = "page-placeholder";
    p.textContent = message;
    elements.pageContent.append(p);
}

function showStreamingText(text) {
    elements.pageContent.innerHTML = "";
    const pre = document.createElement("pre");
    pre.className = "page-streaming";
    pre.textContent = text || "Loading…";
    elements.pageContent.append(pre);
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
    elements.addressInput.value = trimmed;
    elements.browseProgress.textContent = "Requesting…";
    showPlaceholder("Requesting…");
    updateButtons();

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
        showPlaceholder("Type a web address or search above and press Go.");
    } else {
        setPill(elements.connectionBadge, "Disconnected", "muted");
        setPill(elements.deviceStatePill, "Not connected", "muted");
        showPlaceholder("Connect a device, then type a web address or search above and press Go.");
        nav.history = [];
        nav.index = -1;
        elements.addressInput.value = "";
        elements.browseProgress.textContent = "";
    }
    updateButtons();
});

device.addEventListener("status", (event) => {
    const { deviceState } = event.detail;
    const label = deviceStateLabels[deviceState] ?? `State ${deviceState}`;
    const tone = deviceState === 3 ? "good" : deviceState === 5 ? "danger" : "warn";
    setPill(elements.deviceStatePill, label, tone);
});

device.addEventListener("transfer-start", () => {
    elements.browseProgress.textContent = "Loading…";
    showPlaceholder("Loading…");
    updateButtons();
});

device.addEventListener("chunk", () => {
    const transfer = device.transfer;
    if (!transfer) {
        return;
    }
    const received = transfer.chunks.filter(Boolean).length;
    elements.browseProgress.textContent = `Loading… ${received} / ${transfer.totalChunks} pieces`;
    showStreamingText(transfer.chunks.map((chunk) => chunk ?? "").join(""));
});

device.addEventListener("complete", () => {
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
