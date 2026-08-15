import {
    ButterfiDevice,
    deviceStateLabels,
    linkStateLabels,
} from "./protocol.js";

const pwaAssets = [
    "./",
    "./index.html",
    "./browse.html",
    "./logo.png",
    "./styles.css",
    "./app.js",
    "./browse.js",
    "./butterfi-markup.js",
    "./protocol.js",
    "./manifest.webmanifest",
    "./icons/icon-any.svg",
    "./icons/icon-maskable.svg",
];

function formatTimestamp(date = new Date()) {
    return date.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
}

const elements = {
    connectButton: document.querySelector("#connect-button"),
    disconnectButton: document.querySelector("#disconnect-button"),
    statusButton: document.querySelector("#status-button"),
    sendButton: document.querySelector("#send-button"),
    cancelButton: document.querySelector("#cancel-button"),
    resendButton: document.querySelector("#resend-button"),
    clearLogButton: document.querySelector("#clear-log-button"),
    installButton: document.querySelector("#install-button"),
    queryInput: document.querySelector("#query-input"),
    installStatus: document.querySelector("#install-status"),
    supportIndicator: document.querySelector("#support-indicator"),
    connectionBadge: document.querySelector("#connection-badge"),
    queryBadge: document.querySelector("#query-badge"),
    deviceState: document.querySelector("#device-state"),
    linkState: document.querySelector("#link-state"),
    activeRequest: document.querySelector("#active-request"),
    portInfo: document.querySelector("#port-info"),
    queryMeta: document.querySelector("#query-meta"),
    responseRequest: document.querySelector("#response-request"),
    chunkProgress: document.querySelector("#chunk-progress"),
    missingCount: document.querySelector("#missing-count"),
    transferState: document.querySelector("#transfer-state"),
    responseOutput: document.querySelector("#response-output"),
    logOutput: document.querySelector("#log-output"),
};

const device = new ButterfiDevice();

const state = {
    installPromptEvent: null,
    serviceWorkerRegistration: null,
};

function logEvent(direction, message) {
    const item = document.createElement("li");
    const time = document.createElement("span");
    const dir = document.createElement("span");
    const body = document.createElement("span");

    time.className = "log-time";
    dir.className = "log-direction";
    body.className = "log-message";

    time.textContent = formatTimestamp();
    dir.textContent = direction;
    body.textContent = message;

    item.append(time, dir, body);
    elements.logOutput.prepend(item);
}

function setPill(element, text, tone) {
    element.textContent = text;
    element.className = "pill";
    element.classList.add(
        tone === "good" ? "pill-good" : tone === "warn" ? "pill-warn" : tone === "danger" ? "pill-danger" : "pill-muted"
    );
}

function resetTransfer(message = "No response yet.") {
    elements.responseRequest.textContent = "None";
    elements.chunkProgress.textContent = "0 / 0 chunks";
    elements.missingCount.textContent = "0";
    elements.transferState.textContent = "Idle";
    elements.responseOutput.textContent = message;
    setPill(elements.queryBadge, "Idle", "muted");
    elements.queryMeta.textContent = device.connected ? "No request in flight." : "Connect a device to send a query.";
}

function updateInstallUi() {
    const installed = window.matchMedia("(display-mode: standalone)").matches;

    if (installed) {
        elements.installButton.hidden = true;
        elements.installStatus.textContent = "Installed in standalone mode";
        return;
    }

    if (state.installPromptEvent) {
        elements.installButton.hidden = false;
        elements.installStatus.textContent = "Ready to install on this device";
        return;
    }

    elements.installButton.hidden = true;
    if (state.serviceWorkerRegistration) {
        elements.installStatus.textContent = "Install becomes available after Chromium raises the install prompt";
    } else if ("serviceWorker" in navigator) {
        elements.installStatus.textContent = "Preparing offline shell for installation";
    } else {
        elements.installStatus.textContent = "Install unavailable in this browser";
    }
}

function updateButtons() {
    const connected = device.connected;
    const reconnecting = device.reconnecting;
    const hasTransfer = Boolean(device.transfer);
    const missingChunk = hasTransfer ? device.getNextMissingChunkIndex() !== null : false;

    elements.connectButton.disabled = connected || reconnecting || !device.supported;
    elements.disconnectButton.disabled = !(connected || reconnecting || device.port);
    elements.statusButton.disabled = !connected;
    elements.sendButton.disabled = !connected || reconnecting || !elements.queryInput.value.trim();
    elements.cancelButton.disabled = !connected || reconnecting || !hasTransfer;
    elements.resendButton.disabled = !connected || reconnecting || !hasTransfer || !missingChunk;
}

function updateConnectionUi() {
    if (device.connected) {
        setPill(elements.connectionBadge, "Connected", "good");
    } else if (device.reconnecting) {
        setPill(elements.connectionBadge, "Reconnecting", "warn");
        elements.deviceState.textContent = "Reconnecting";
        elements.linkState.textContent = "Unknown";
    } else {
        setPill(elements.connectionBadge, "Disconnected", "muted");
        elements.deviceState.textContent = "Unknown";
        elements.linkState.textContent = "Unknown";
        elements.activeRequest.textContent = "None";
        elements.portInfo.textContent = "Not connected";
    }
    updateButtons();
    updateInstallUi();
}

function updateStatusUi() {
    const { deviceState, linkState, activeRequest } = device.deviceStatus;
    elements.deviceState.textContent = deviceStateLabels[deviceState] ?? `State ${deviceState}`;
    elements.linkState.textContent = linkStateLabels[linkState] ?? `Link ${linkState}`;
    elements.activeRequest.textContent = activeRequest ? String(activeRequest) : "None";
}

function renderTransfer() {
    if (!device.transfer) {
        resetTransfer();
        updateButtons();
        return;
    }

    const transfer = device.transfer;
    const received = transfer.chunks.filter(Boolean).length;
    const missing = transfer.chunks.length - received;
    const partialText = transfer.chunks
        .map((chunk, index) => (chunk === null ? `\n[missing chunk ${index}]\n` : chunk))
        .join("");

    elements.responseRequest.textContent = String(transfer.requestId);
    elements.chunkProgress.textContent = `${received} / ${transfer.totalChunks} chunks`;
    elements.missingCount.textContent = String(missing);
    elements.transferState.textContent = transfer.complete ? "Complete" : "Receiving";
    elements.responseOutput.textContent = partialText || "Waiting for first chunk…";
    elements.queryMeta.textContent = transfer.complete
        ? `Request ${transfer.requestId} finished.`
        : `Request ${transfer.requestId} is in flight.`;
    setPill(elements.queryBadge, transfer.complete ? "Complete" : "Waiting", transfer.complete ? "good" : "warn");
    updateButtons();
}

async function sendQuery() {
    const query = elements.queryInput.value.trim();
    if (!query) {
        return;
    }

    await device.sendQuery(query);
    updateButtons();
}

async function registerServiceWorker() {
    if (!("serviceWorker" in navigator)) {
        logEvent("system", "Service workers are not available in this browser");
        updateInstallUi();
        return;
    }

    try {
        state.serviceWorkerRegistration = await navigator.serviceWorker.register("./sw.js", { scope: "./" });
        logEvent("system", `Service worker ready for ${pwaAssets.length} shell assets`);
    } catch (error) {
        logEvent("system", `Service worker registration failed: ${error.message}`);
    }

    updateInstallUi();
}

async function installPwa() {
    if (!state.installPromptEvent) {
        updateInstallUi();
        return;
    }

    const promptEvent = state.installPromptEvent;
    state.installPromptEvent = null;
    updateInstallUi();

    await promptEvent.prompt();
    const outcome = await promptEvent.userChoice;
    logEvent("system", `Install prompt ${outcome.outcome}`);
    updateInstallUi();
}

function initializeSupportUi() {
    if (device.supported) {
        elements.supportIndicator.textContent = "Web Serial is available in this browser.";
    } else {
        elements.supportIndicator.textContent = "Web Serial is not available in this browser.";
        setPill(elements.connectionBadge, "Unsupported browser", "danger");
    }
    updateConnectionUi();
    resetTransfer();
}

device.addEventListener("log", (event) => {
    logEvent(event.detail.direction, event.detail.message);
});

device.addEventListener("connection-change", (event) => {
    if (event.detail.connected) {
        elements.portInfo.textContent = `USB ${event.detail.usbVendorId ?? "?"}:${event.detail.usbProductId ?? "?"}`;
    }
    updateConnectionUi();
});

device.addEventListener("status", () => {
    updateStatusUi();
});

device.addEventListener("transfer-start", () => {
    renderTransfer();
});

device.addEventListener("chunk", () => {
    renderTransfer();
});

device.addEventListener("complete", () => {
    renderTransfer();
});

device.addEventListener("error", (event) => {
    const { description, detail } = event.detail;
    setPill(elements.queryBadge, "Error", "danger");
    elements.transferState.textContent = "Error";
    if (device.transfer && device.transfer.requestId === event.detail.requestId) {
        elements.responseOutput.textContent = `${description}${detail ? `\n\n${detail}` : ""}`;
    }
});

window.addEventListener("beforeinstallprompt", (event) => {
    event.preventDefault();
    state.installPromptEvent = event;
    logEvent("system", "Install prompt is available");
    updateInstallUi();
});

window.addEventListener("appinstalled", () => {
    state.installPromptEvent = null;
    logEvent("system", "ButterFi Console installed");
    updateInstallUi();
});

elements.connectButton.addEventListener("click", () => {
    device.connect().catch((error) => logEvent("system", `Connect failed: ${error.message}`));
});

elements.disconnectButton.addEventListener("click", () => {
    device.disconnect().catch((error) => logEvent("system", `Disconnect failed: ${error.message}`));
});

elements.statusButton.addEventListener("click", () => {
    device.requestDeviceStatus().catch((error) => logEvent("system", `Status request failed: ${error.message}`));
});

elements.sendButton.addEventListener("click", () => {
    sendQuery().catch((error) => logEvent("system", `Query send failed: ${error.message}`));
});

elements.cancelButton.addEventListener("click", () => {
    device.cancelTransfer()
        .then(() => resetTransfer("Transfer cancelled from browser."))
        .catch((error) => logEvent("system", `Cancel failed: ${error.message}`));
});

elements.resendButton.addEventListener("click", () => {
    device.requestMissingChunk().catch((error) => logEvent("system", `Resend request failed: ${error.message}`));
});

elements.clearLogButton.addEventListener("click", () => {
    elements.logOutput.textContent = "";
});

elements.installButton.addEventListener("click", () => {
    installPwa().catch((error) => logEvent("system", `Install failed: ${error.message}`));
});

elements.queryInput.addEventListener("input", updateButtons);

initializeSupportUi();
registerServiceWorker().catch((error) => logEvent("system", `PWA bootstrap failed: ${error.message}`));
