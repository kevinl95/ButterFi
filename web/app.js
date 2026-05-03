const USB = {
    sync1: 0x42,
    sync2: 0x46,
    version: 0x01,
    frameHostQuerySubmit: 0x01,
    frameHostResendRequest: 0x02,
    frameHostCancelRequest: 0x03,
    frameHostStatusRequest: 0x05,
    frameDeviceStatus: 0x81,
    frameDeviceUplinkAccepted: 0x82,
    frameDeviceResponseChunk: 0x83,
    frameDeviceTransferComplete: 0x84,
    frameDeviceTransferError: 0x85,
    maxPayload: 512,
};

const SIDEWALK = {
    responseChunk: 0x81,
};

const deviceStateLabels = {
    0: "Idle",
    1: "Serial ready",
    2: "Sidewalk starting",
    3: "Sidewalk ready",
    4: "Busy",
    5: "Error",
    6: "Sidewalk not registered",
};

const linkStateLabels = {
    0: "Unknown",
    1: "BLE",
    2: "FSK",
    3: "LoRa",
};

const errorCodeLabels = {
    0x01: "Invalid host frame",
    0x02: "Device busy",
    0x03: "Sidewalk unavailable",
    0x04: "Cloud fetch failed",
    0x05: "Transfer timed out",
    0x06: "Protocol mismatch",
};

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();
const pwaAssets = [
    "./",
    "./index.html",
    "./styles.css",
    "./app.js",
    "./manifest.webmanifest",
    "./icons/icon-any.svg",
    "./icons/icon-maskable.svg",
];

class ButterfiFrameParser {
    constructor() {
        this.buffer = [];
        this.expectedLength = 0;
    }

    reset() {
        this.buffer = [];
        this.expectedLength = 0;
    }

    push(chunk) {
        const frames = [];

        for (const byte of chunk) {
            if (this.buffer.length === 0 && byte !== USB.sync1) {
                continue;
            }

            if (this.buffer.length === 1 && byte !== USB.sync2) {
                this.buffer = byte === USB.sync1 ? [USB.sync1] : [];
                this.expectedLength = 0;
                continue;
            }

            this.buffer.push(byte);

            if (this.buffer.length === 8) {
                const payloadLength = this.buffer[6] | (this.buffer[7] << 8);
                if (this.buffer[2] !== USB.version || payloadLength > USB.maxPayload) {
                    this.reset();
                    continue;
                }

                this.expectedLength = 8 + payloadLength + 1;
            }

            if (this.expectedLength > 0 && this.buffer.length === this.expectedLength) {
                const frameBytes = Uint8Array.from(this.buffer);
                const checksum = frameBytes.slice(2, frameBytes.length - 1)
                    .reduce((xor, current) => xor ^ current, 0);

                if (checksum === frameBytes[frameBytes.length - 1]) {
                    frames.push({
                        frameType: frameBytes[3],
                        requestId: frameBytes[4],
                        flags: frameBytes[5],
                        payload: frameBytes.slice(8, frameBytes.length - 1),
                    });
                }

                this.reset();
            }
        }

        return frames;
    }
}

function encodeFrame(frameType, requestId, payload = new Uint8Array(0), flags = 0) {
    if (payload.length > USB.maxPayload) {
        throw new Error("Payload too large for ButterFi USB frame");
    }

    const buffer = new Uint8Array(8 + payload.length + 1);
    buffer[0] = USB.sync1;
    buffer[1] = USB.sync2;
    buffer[2] = USB.version;
    buffer[3] = frameType;
    buffer[4] = requestId;
    buffer[5] = flags;
    buffer[6] = payload.length & 0xff;
    buffer[7] = (payload.length >> 8) & 0xff;
    buffer.set(payload, 8);
    buffer[buffer.length - 1] = buffer.slice(2, buffer.length - 1)
        .reduce((xor, current) => xor ^ current, 0);
    return buffer;
}

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

const state = {
    port: null,
    reader: null,
    writer: null,
    readLoopPromise: null,
    parser: new ButterfiFrameParser(),
    connected: false,
    requestCounter: 1,
    transfer: null,
    deviceStatus: {
        deviceState: 0,
        linkState: 0,
        activeRequest: 0,
    },
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

function nextRequestId() {
    const current = state.requestCounter;
    state.requestCounter = state.requestCounter >= 255 ? 1 : state.requestCounter + 1;
    return current;
}

function resetTransfer(message = "No response yet.") {
    state.transfer = null;
    elements.responseRequest.textContent = "None";
    elements.chunkProgress.textContent = "0 / 0 chunks";
    elements.missingCount.textContent = "0";
    elements.transferState.textContent = "Idle";
    elements.responseOutput.textContent = message;
    setPill(elements.queryBadge, "Idle", "muted");
    elements.queryMeta.textContent = state.connected ? "No request in flight." : "Connect a device to send a query.";
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
    const connected = state.connected;
    const hasTransfer = Boolean(state.transfer);
    const missingChunk = hasTransfer ? getNextMissingChunkIndex(state.transfer) !== null : false;

    elements.connectButton.disabled = connected || !navigator.serial;
    elements.disconnectButton.disabled = !connected;
    elements.statusButton.disabled = !connected;
    elements.sendButton.disabled = !connected || !elements.queryInput.value.trim();
    elements.cancelButton.disabled = !connected || !hasTransfer;
    elements.resendButton.disabled = !connected || !hasTransfer || !missingChunk;
}

function updateConnectionUi() {
    if (state.connected) {
        setPill(elements.connectionBadge, "Connected", "good");
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
    const { deviceState, linkState, activeRequest } = state.deviceStatus;
    elements.deviceState.textContent = deviceStateLabels[deviceState] ?? `State ${deviceState}`;
    elements.linkState.textContent = linkStateLabels[linkState] ?? `Link ${linkState}`;
    elements.activeRequest.textContent = activeRequest ? String(activeRequest) : "None";
}

function getNextMissingChunkIndex(transfer) {
    if (!transfer || !Array.isArray(transfer.chunks)) {
        return null;
    }

    const index = transfer.chunks.findIndex((chunk) => chunk === null);
    return index === -1 ? null : index;
}

function renderTransfer() {
    if (!state.transfer) {
        resetTransfer();
        updateButtons();
        return;
    }

    const received = state.transfer.chunks.filter(Boolean).length;
    const missing = state.transfer.chunks.length - received;
    const partialText = state.transfer.chunks
        .map((chunk, index) => (chunk === null ? `\n[missing chunk ${index}]\n` : chunk))
        .join("");

    elements.responseRequest.textContent = String(state.transfer.requestId);
    elements.chunkProgress.textContent = `${received} / ${state.transfer.totalChunks} chunks`;
    elements.missingCount.textContent = String(missing);
    elements.transferState.textContent = state.transfer.complete ? "Complete" : "Receiving";
    elements.responseOutput.textContent = partialText || "Waiting for first chunk…";
    elements.queryMeta.textContent = state.transfer.complete
        ? `Request ${state.transfer.requestId} finished.`
        : `Request ${state.transfer.requestId} is in flight.`;
    setPill(elements.queryBadge, state.transfer.complete ? "Complete" : "Waiting", state.transfer.complete ? "good" : "warn");
    updateButtons();
}

function startTransfer(requestId) {
    state.transfer = {
        requestId,
        totalChunks: 0,
        chunks: [],
        complete: false,
    };
    renderTransfer();
}

function ensureTransfer(requestId, totalChunks) {
    if (!state.transfer || state.transfer.requestId !== requestId) {
        state.transfer = {
            requestId,
            totalChunks,
            chunks: new Array(totalChunks).fill(null),
            complete: false,
        };
    }

    if (state.transfer.totalChunks !== totalChunks) {
        state.transfer.totalChunks = totalChunks;
        if (state.transfer.chunks.length !== totalChunks) {
            const nextChunks = new Array(totalChunks).fill(null);
            state.transfer.chunks.forEach((chunk, index) => {
                if (index < totalChunks) {
                    nextChunks[index] = chunk;
                }
            });
            state.transfer.chunks = nextChunks;
        }
    }

    return state.transfer;
}

async function writeFrame(frameType, requestId, payload = new Uint8Array(0)) {
    if (!state.writer) {
        throw new Error("Serial writer is not available");
    }

    const frame = encodeFrame(frameType, requestId, payload);
    await state.writer.write(frame);
}

async function requestDeviceStatus() {
    await writeFrame(USB.frameHostStatusRequest, 0);
    logEvent("host", "Requested device status");
}

async function sendQuery() {
    const query = elements.queryInput.value.trim();
    if (!query) {
        return;
    }

    const requestId = nextRequestId();
    startTransfer(requestId);
    await writeFrame(USB.frameHostQuerySubmit, requestId, textEncoder.encode(query));
    logEvent("host", `Sent query ${requestId}: ${query}`);
    updateButtons();
}

async function cancelTransfer() {
    if (!state.transfer) {
        return;
    }

    const requestId = state.transfer.requestId;
    await writeFrame(USB.frameHostCancelRequest, requestId);
    logEvent("host", `Sent cancel for request ${requestId}`);
    resetTransfer("Transfer cancelled from browser.");
}

async function requestMissingChunk() {
    if (!state.transfer) {
        return;
    }

    const chunkIndex = getNextMissingChunkIndex(state.transfer);
    if (chunkIndex === null) {
        return;
    }

    await writeFrame(USB.frameHostResendRequest, state.transfer.requestId, Uint8Array.of(chunkIndex));
    logEvent("host", `Requested resend from chunk ${chunkIndex} for request ${state.transfer.requestId}`);
}

function handleStatusFrame(payload) {
    if (payload.length < 3) {
        logEvent("device", "Received short status payload");
        return;
    }

    state.deviceStatus = {
        deviceState: payload[0],
        linkState: payload[1],
        activeRequest: payload[2],
    };
    updateStatusUi();
    logEvent(
        "device",
        `Status: ${deviceStateLabels[payload[0]] ?? payload[0]}, ${linkStateLabels[payload[1]] ?? payload[1]}, request ${payload[2] || "none"}`
    );
}

function handleResponseChunkFrame(payload) {
    if (payload.length < 4 || payload[0] !== SIDEWALK.responseChunk) {
        logEvent("device", "Received malformed response chunk payload");
        return;
    }

    const requestId = payload[1];
    const chunkIndex = payload[2];
    const totalChunks = payload[3];
    const chunkText = textDecoder.decode(payload.slice(4));
    const transfer = ensureTransfer(requestId, totalChunks);

    transfer.chunks[chunkIndex] = chunkText;
    logEvent("device", `Chunk ${chunkIndex + 1}/${totalChunks} for request ${requestId}`);
    renderTransfer();
}

function handleTransferComplete(requestId) {
    if (state.transfer && state.transfer.requestId === requestId) {
        state.transfer.complete = true;
        renderTransfer();
    }
    logEvent("device", `Transfer complete for request ${requestId}`);
}

function handleTransferError(requestId, payload) {
    const errorCode = payload[0] ?? 0;
    const detail = payload.length > 1 ? textDecoder.decode(payload.slice(1)) : "";
    const description = errorCodeLabels[errorCode] ?? `Error ${errorCode}`;
    logEvent("device", `Transfer error for request ${requestId}: ${description}${detail ? ` - ${detail}` : ""}`);
    setPill(elements.queryBadge, "Error", "danger");
    elements.transferState.textContent = "Error";
    if (state.transfer && state.transfer.requestId === requestId) {
        elements.responseOutput.textContent = `${description}${detail ? `\n\n${detail}` : ""}`;
    }
}

function handleFrame(frame) {
    switch (frame.frameType) {
        case USB.frameDeviceStatus:
            handleStatusFrame(frame.payload);
            break;
        case USB.frameDeviceUplinkAccepted:
            logEvent("device", `Uplink accepted for request ${frame.requestId}`);
            break;
        case USB.frameDeviceResponseChunk:
            handleResponseChunkFrame(frame.payload);
            break;
        case USB.frameDeviceTransferComplete:
            handleTransferComplete(frame.requestId);
            break;
        case USB.frameDeviceTransferError:
            handleTransferError(frame.requestId, frame.payload);
            break;
        default:
            logEvent("device", `Unhandled frame type 0x${frame.frameType.toString(16)}`);
            break;
    }
}

async function readLoop() {
    try {
        while (state.reader) {
            const { value, done } = await state.reader.read();
            if (done) {
                break;
            }
            if (!value) {
                continue;
            }

            const frames = state.parser.push(value);
            frames.forEach(handleFrame);
        }
    } catch (error) {
        logEvent("system", `Read loop ended with error: ${error.message}`);
    }
}

async function connect() {
    if (!navigator.serial) {
        return;
    }

    state.port = await navigator.serial.requestPort();
    await state.port.open({ baudRate: 115200, bufferSize: 4096 });

    if (typeof state.port.setSignals === "function") {
        try {
            await state.port.setSignals({ dataTerminalReady: true, requestToSend: true });
        } catch (error) {
            logEvent("system", `Unable to assert serial control lines: ${error.message}`);
        }
    }

    state.writer = state.port.writable.getWriter();
    state.reader = state.port.readable.getReader();
    state.connected = true;
    state.parser.reset();

    const info = state.port.getInfo();
    elements.portInfo.textContent = `USB ${info.usbVendorId ?? "?"}:${info.usbProductId ?? "?"}`;
    updateConnectionUi();
    logEvent("system", "Serial port connected");

    state.readLoopPromise = readLoop();
    await requestDeviceStatus();
}

async function disconnect() {
    if (!state.port) {
        return;
    }

    if (typeof state.port.setSignals === "function") {
        try {
            await state.port.setSignals({ dataTerminalReady: false, requestToSend: false });
        } catch (error) {
            logEvent("system", `Unable to clear serial control lines: ${error.message}`);
        }
    }

    if (state.reader) {
        await state.reader.cancel();
        state.reader.releaseLock();
    }
    if (state.writer) {
        state.writer.releaseLock();
    }
    if (state.readLoopPromise) {
        await state.readLoopPromise.catch(() => {});
    }

    await state.port.close();

    state.port = null;
    state.reader = null;
    state.writer = null;
    state.readLoopPromise = null;
    state.connected = false;
    state.deviceStatus = { deviceState: 0, linkState: 0, activeRequest: 0 };
    resetTransfer();
    updateConnectionUi();
    logEvent("system", "Serial port disconnected");
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
    if (navigator.serial) {
        elements.supportIndicator.textContent = "Web Serial is available in this browser.";
    } else {
        elements.supportIndicator.textContent = "Web Serial is not available in this browser.";
        setPill(elements.connectionBadge, "Unsupported browser", "danger");
    }
    updateConnectionUi();
    resetTransfer();
}

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
    connect().catch((error) => logEvent("system", `Connect failed: ${error.message}`));
});

elements.disconnectButton.addEventListener("click", () => {
    disconnect().catch((error) => logEvent("system", `Disconnect failed: ${error.message}`));
});

elements.statusButton.addEventListener("click", () => {
    requestDeviceStatus().catch((error) => logEvent("system", `Status request failed: ${error.message}`));
});

elements.sendButton.addEventListener("click", () => {
    sendQuery().catch((error) => logEvent("system", `Query send failed: ${error.message}`));
});

elements.cancelButton.addEventListener("click", () => {
    cancelTransfer().catch((error) => logEvent("system", `Cancel failed: ${error.message}`));
});

elements.resendButton.addEventListener("click", () => {
    requestMissingChunk().catch((error) => logEvent("system", `Resend request failed: ${error.message}`));
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