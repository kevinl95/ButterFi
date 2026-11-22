import { test } from "node:test";
import assert from "node:assert/strict";
import { USB, SIDEWALK, ButterfiFrameParser, ButterfiDevice, encodeFrame } from "../../web/protocol.js";

const bytes = (s) => new TextEncoder().encode(s);

test("encodeFrame layout and checksum", () => {
    const frame = encodeFrame(USB.frameHostQuerySubmit, 7, bytes("hi"), 0x02);
    assert.deepEqual([...frame.slice(0, 8)], [0x42, 0x46, 0x01, 0x01, 7, 0x02, 2, 0]);
    assert.deepEqual([...frame.slice(8, 10)], [0x68, 0x69]);
    const xor = frame.slice(2, -1).reduce((a, b) => a ^ b, 0);
    assert.equal(frame.at(-1), xor);
});

test("encodeFrame rejects an oversized payload", () => {
    assert.throws(() => encodeFrame(1, 1, new Uint8Array(USB.maxPayload + 1)));
});

test("parser round-trips frames split across reads, after garbage", () => {
    const a = encodeFrame(USB.frameDeviceStatus, 0, Uint8Array.of(3, 1, 0));
    const b = encodeFrame(USB.frameDeviceResponseChunk, 9, new Uint8Array(300).fill(0xab));
    const stream = Uint8Array.from([0x00, 0x42, 0x13, ...a, 0xff, ...b]);
    const parser = new ButterfiFrameParser();
    const frames = [];
    for (let i = 0; i < stream.length; i += 5) {
        frames.push(...parser.push(stream.slice(i, i + 5)));
    }
    assert.equal(frames.length, 2);
    assert.equal(frames[0].frameType, USB.frameDeviceStatus);
    assert.deepEqual([...frames[0].payload], [3, 1, 0]);
    assert.equal(frames[1].requestId, 9);
    assert.equal(frames[1].payload.length, 300);
});

test("parser drops a bad checksum and resyncs on the next frame", () => {
    const bad = encodeFrame(USB.frameDeviceStatus, 0, Uint8Array.of(1, 2, 3));
    bad[bad.length - 1] ^= 0xff;
    const good = encodeFrame(USB.frameDeviceStatus, 0, Uint8Array.of(4, 5, 6));
    const frames = new ButterfiFrameParser().push(Uint8Array.from([...bad, ...good]));
    assert.equal(frames.length, 1);
    assert.deepEqual([...frames[0].payload], [4, 5, 6]);
});

test("parser rejects a wrong version or an over-long length", () => {
    const wrongVersion = encodeFrame(USB.frameDeviceStatus, 0, Uint8Array.of(1));
    wrongVersion[2] = 0x02;
    const tooLong = Uint8Array.of(0x42, 0x46, 0x01, 0x81, 0, 0, 0x01, 0x04); // 1025 bytes
    const good = encodeFrame(USB.frameDeviceStatus, 0, Uint8Array.of(9, 9, 9));
    const frames = new ButterfiFrameParser().push(Uint8Array.from([...wrongVersion, ...tooLong, ...good]));
    assert.equal(frames.length, 1);
    assert.deepEqual([...frames[0].payload], [9, 9, 9]);
});

function chunkFrame(requestId, index, total, data) {
    return {
        frameType: USB.frameDeviceResponseChunk,
        requestId,
        flags: 0,
        payload: Uint8Array.from([SIDEWALK.responseChunk, requestId, index, total, ...data]),
    };
}

test("device assembles out-of-order chunks and completes once", () => {
    const device = new ButterfiDevice();
    const completes = [];
    device.addEventListener("complete", (e) => completes.push(e.detail.requestId));
    device._handleFrame(chunkFrame(4, 2, 3, [3]));
    assert.equal(device.getNextMissingChunkIndex(), 0);
    device._handleFrame(chunkFrame(4, 0, 3, [1]));
    assert.equal(device.getNextMissingChunkIndex(), 1);
    assert.equal(device.transfer.complete, false);
    device._handleFrame(chunkFrame(4, 1, 3, [2]));
    assert.equal(device.transfer.complete, true);
    assert.equal(device.getNextMissingChunkIndex(), null);
    assert.deepEqual(device.transfer.chunks.map((c) => [...c]), [[1], [2], [3]]);
    device._handleFrame({ frameType: USB.frameDeviceTransferComplete, requestId: 4, flags: 0, payload: new Uint8Array(0) });
    assert.deepEqual(completes, [4]);
});

test("a chunk for a new request ID replaces the old transfer", () => {
    const device = new ButterfiDevice();
    device._handleFrame(chunkFrame(1, 0, 2, [1]));
    device._handleFrame(chunkFrame(2, 0, 2, [9]));
    assert.equal(device.transfer.requestId, 2);
    assert.deepEqual([...device.transfer.chunks[0]], [9]);
    assert.equal(device.transfer.chunks[1], null);
});

test("malformed chunk payloads are ignored", () => {
    const device = new ButterfiDevice();
    device._handleFrame({ frameType: USB.frameDeviceResponseChunk, requestId: 1, flags: 0, payload: Uint8Array.of(0x81, 1) });
    device._handleFrame({ frameType: USB.frameDeviceResponseChunk, requestId: 1, flags: 0, payload: Uint8Array.of(0x00, 1, 0, 1, 5) });
    assert.equal(device.transfer, null);
});

test("request IDs wrap from 255 back to 1", () => {
    const device = new ButterfiDevice();
    device.requestCounter = 255;
    assert.equal(device._nextRequestId(), 255);
    assert.equal(device._nextRequestId(), 1);
});

test("resend asks from the first missing chunk", async () => {
    const device = new ButterfiDevice();
    const written = [];
    device.writer = { write: async (frame) => written.push(frame) };
    device._handleFrame(chunkFrame(6, 0, 4, [1]));
    device._handleFrame(chunkFrame(6, 1, 4, [1]));
    device._handleFrame(chunkFrame(6, 3, 4, [1]));
    await device.requestMissingChunk();
    const [frame] = new ButterfiFrameParser().push(written[0]);
    assert.equal(frame.frameType, USB.frameHostResendRequest);
    assert.equal(frame.requestId, 6);
    assert.deepEqual([...frame.payload], [2]);
});
