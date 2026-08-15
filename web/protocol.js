// Shared ButterFi USB framing + Web Serial transport.
//
// Wraps the binary frame layer from docs/shared-protocol.md behind a small
// EventTarget-based device object so both the debug console (app.js) and the
// student browsing UI (browse.js) can drive a device without duplicating the
// serial connection, frame parsing, and chunk-assembly logic.

export const USB = {
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

export const SIDEWALK = {
    responseChunk: 0x81,
};

export const deviceStateLabels = {
    0: "Idle",
    1: "Serial ready",
    2: "Sidewalk starting",
    3: "Sidewalk ready",
    4: "Busy",
    5: "Error",
    6: "Sidewalk not registered",
};

export const linkStateLabels = {
    0: "Unknown",
    1: "BLE",
    2: "FSK",
    3: "LoRa",
};

export const errorCodeLabels = {
    0x01: "Invalid host frame",
    0x02: "Device busy",
    0x03: "Sidewalk unavailable",
    0x04: "Cloud fetch failed",
    0x05: "Transfer timed out",
    0x06: "Protocol mismatch",
    0x07: "Config save failed",
};

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

export class ButterfiFrameParser {
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

export function encodeFrame(frameType, requestId, payload = new Uint8Array(0), flags = 0) {
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

function getNextMissingChunkIndex(transfer) {
    if (!transfer || !Array.isArray(transfer.chunks)) {
        return null;
    }

    const index = transfer.chunks.findIndex((chunk) => chunk === null);
    return index === -1 ? null : index;
}

/**
 * Owns the Web Serial connection, USB frame encode/decode, and chunked
 * response assembly for a single ButterFi device. UI layers subscribe via
 * addEventListener() and read `device.transfer` / `device.deviceStatus`.
 *
 * Events: "log", "connection-change", "status", "transfer-start", "chunk",
 * "complete", "error".
 */
export class ButterfiDevice extends EventTarget {
    constructor() {
        super();
        this.port = null;
        this.reader = null;
        this.writer = null;
        this.readLoopPromise = null;
        this.parser = new ButterfiFrameParser();
        this.connected = false;
        this.requestCounter = 1;
        this.transfer = null;
        this.deviceStatus = { deviceState: 0, linkState: 0, activeRequest: 0 };
    }

    get supported() {
        return Boolean(navigator.serial);
    }

    _log(direction, message) {
        this.dispatchEvent(new CustomEvent("log", { detail: { direction, message } }));
    }

    _nextRequestId() {
        const current = this.requestCounter;
        this.requestCounter = this.requestCounter >= 255 ? 1 : this.requestCounter + 1;
        return current;
    }

    async _writeFrame(frameType, requestId, payload = new Uint8Array(0)) {
        if (!this.writer) {
            throw new Error("Serial writer is not available");
        }

        await this.writer.write(encodeFrame(frameType, requestId, payload));
    }

    getNextMissingChunkIndex() {
        return getNextMissingChunkIndex(this.transfer);
    }

    async connect() {
        if (!navigator.serial) {
            throw new Error("Web Serial is not available in this browser");
        }

        this.port = await navigator.serial.requestPort();
        await this.port.open({ baudRate: 115200, bufferSize: 4096 });

        if (typeof this.port.setSignals === "function") {
            try {
                await this.port.setSignals({ dataTerminalReady: true, requestToSend: true });
            } catch (error) {
                this._log("system", `Unable to assert serial control lines: ${error.message}`);
            }
        }

        this.writer = this.port.writable.getWriter();
        this.reader = this.port.readable.getReader();
        this.connected = true;
        this.parser.reset();

        const info = this.port.getInfo();
        this.dispatchEvent(new CustomEvent("connection-change", {
            detail: {
                connected: true,
                usbVendorId: info.usbVendorId,
                usbProductId: info.usbProductId,
            },
        }));
        this._log("system", "Serial port connected");

        this.readLoopPromise = this._readLoop();
        await this.requestDeviceStatus();
    }

    async disconnect() {
        if (!this.port) {
            return;
        }
        this._closing = true;  // intentional close; suppress the read-loop drop handler

        if (typeof this.port.setSignals === "function") {
            try {
                await this.port.setSignals({ dataTerminalReady: false, requestToSend: false });
            } catch (error) {
                this._log("system", `Unable to clear serial control lines: ${error.message}`);
            }
        }

        if (this.reader) {
            await this.reader.cancel();
            this.reader.releaseLock();
        }
        if (this.writer) {
            this.writer.releaseLock();
        }
        if (this.readLoopPromise) {
            await this.readLoopPromise.catch(() => {});
        }

        await this.port.close();

        this.port = null;
        this.reader = null;
        this.writer = null;
        this.readLoopPromise = null;
        this.connected = false;
        this.deviceStatus = { deviceState: 0, linkState: 0, activeRequest: 0 };
        this.transfer = null;
        this.dispatchEvent(new CustomEvent("connection-change", { detail: { connected: false } }));
        this._log("system", "Serial port disconnected");
        this._closing = false;
    }

    async requestDeviceStatus() {
        await this._writeFrame(USB.frameHostStatusRequest, 0);
        this._log("host", "Requested device status");
    }

    async sendQuery(query) {
        const trimmed = query.trim();
        if (!trimmed) {
            return null;
        }

        const requestId = this._nextRequestId();
        this.transfer = { requestId, totalChunks: 0, chunks: [], complete: false };
        this.dispatchEvent(new CustomEvent("transfer-start", { detail: { requestId } }));

        await this._writeFrame(USB.frameHostQuerySubmit, requestId, textEncoder.encode(trimmed));
        this._log("host", `Sent query ${requestId}: ${trimmed}`);
        return requestId;
    }

    async cancelTransfer() {
        if (!this.transfer) {
            return;
        }

        const requestId = this.transfer.requestId;
        await this._writeFrame(USB.frameHostCancelRequest, requestId);
        this._log("host", `Sent cancel for request ${requestId}`);
        this.transfer = null;
        this.dispatchEvent(new CustomEvent("transfer-start", { detail: { requestId: null } }));
    }

    async requestMissingChunk() {
        const chunkIndex = this.getNextMissingChunkIndex();
        if (chunkIndex === null || !this.transfer) {
            return;
        }

        await this._writeFrame(USB.frameHostResendRequest, this.transfer.requestId, Uint8Array.of(chunkIndex));
        this._log("host", `Requested resend from chunk ${chunkIndex} for request ${this.transfer.requestId}`);
    }

    _ensureTransfer(requestId, totalChunks) {
        if (!this.transfer || this.transfer.requestId !== requestId) {
            this.transfer = {
                requestId,
                totalChunks,
                chunks: new Array(totalChunks).fill(null),
                complete: false,
            };
        }

        if (this.transfer.totalChunks !== totalChunks) {
            this.transfer.totalChunks = totalChunks;
            if (this.transfer.chunks.length !== totalChunks) {
                const nextChunks = new Array(totalChunks).fill(null);
                this.transfer.chunks.forEach((chunk, index) => {
                    if (index < totalChunks) {
                        nextChunks[index] = chunk;
                    }
                });
                this.transfer.chunks = nextChunks;
            }
        }

        return this.transfer;
    }

    _handleStatusFrame(payload) {
        if (payload.length < 3) {
            this._log("device", "Received short status payload");
            return;
        }

        this.deviceStatus = {
            deviceState: payload[0],
            linkState: payload[1],
            activeRequest: payload[2],
        };
        this.dispatchEvent(new CustomEvent("status", { detail: this.deviceStatus }));
        this._log(
            "device",
            `Status: ${deviceStateLabels[payload[0]] ?? payload[0]}, ${linkStateLabels[payload[1]] ?? payload[1]}, request ${payload[2] || "none"}`
        );
    }

    _handleResponseChunkFrame(payload) {
        if (payload.length < 4 || payload[0] !== SIDEWALK.responseChunk) {
            this._log("device", "Received malformed response chunk payload");
            return;
        }

        const requestId = payload[1];
        const chunkIndex = payload[2];
        const totalChunks = payload[3];
        const chunkText = textDecoder.decode(payload.slice(4));
        const transfer = this._ensureTransfer(requestId, totalChunks);

        transfer.chunks[chunkIndex] = chunkText;
        this._log("device", `Chunk ${chunkIndex + 1}/${totalChunks} for request ${requestId}`);
        this.dispatchEvent(new CustomEvent("chunk", { detail: { transfer } }));
    }

    _handleTransferComplete(requestId) {
        if (this.transfer && this.transfer.requestId === requestId) {
            this.transfer.complete = true;
        }
        this.dispatchEvent(new CustomEvent("complete", { detail: { requestId, transfer: this.transfer } }));
        this._log("device", `Transfer complete for request ${requestId}`);
    }

    _handleTransferError(requestId, payload) {
        const errorCode = payload[0] ?? 0;
        const detail = payload.length > 1 ? textDecoder.decode(payload.slice(1)) : "";
        const description = errorCodeLabels[errorCode] ?? `Error ${errorCode}`;
        this._log("device", `Transfer error for request ${requestId}: ${description}${detail ? ` - ${detail}` : ""}`);
        this.dispatchEvent(new CustomEvent("error", { detail: { requestId, errorCode, description, detail } }));
    }

    _handleFrame(frame) {
        switch (frame.frameType) {
            case USB.frameDeviceStatus:
                this._handleStatusFrame(frame.payload);
                break;
            case USB.frameDeviceUplinkAccepted:
                this._log("device", `Uplink accepted for request ${frame.requestId}`);
                break;
            case USB.frameDeviceResponseChunk:
                this._handleResponseChunkFrame(frame.payload);
                break;
            case USB.frameDeviceTransferComplete:
                this._handleTransferComplete(frame.requestId);
                break;
            case USB.frameDeviceTransferError:
                this._handleTransferError(frame.requestId, frame.payload);
                break;
            default:
                this._log("device", `Unhandled frame type 0x${frame.frameType.toString(16)}`);
                break;
        }
    }

    async _readLoop() {
        try {
            while (this.reader) {
                const { value, done } = await this.reader.read();
                if (done) {
                    break;
                }
                if (!value) {
                    continue;
                }

                const frames = this.parser.push(value);
                frames.forEach((frame) => this._handleFrame(frame));
            }
        } catch (error) {
            this._log("system", `Read loop ended with error: ${error.message}`);
        } finally {
            // If the loop ended while we still thought we were connected and it
            // wasn't an intentional disconnect(), the device was unplugged / the
            // port dropped. Tear down and notify so the UI prompts a reconnect
            // instead of spinning on a frozen device state forever.
            if (this.connected && !this._closing) {
                this.connected = false;
                this.deviceStatus = { deviceState: 0, linkState: 0, activeRequest: 0 };
                this.transfer = null;
                try { this.reader?.releaseLock(); } catch (_) { /* already released */ }
                this.reader = null;
                this.dispatchEvent(new CustomEvent("connection-change", { detail: { connected: false } }));
                this._log("system", "Device connection lost (unplugged?)");
            }
        }
    }
}
