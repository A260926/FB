// Contract tests for ALAC frame element decoding: element ordering,
// per-packet frame counts, real compressed packets, and error handling.
// Run directly via `node --test` (see checks/check_target.sh).
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const { AV, ALAC, cookie } = require('../checks/witness.cjs');

const ID_SCE = 0, ID_CPE = 1, ID_LFE = 3;

// Build a hand-made escape (uncompressed) packet. Each element is
// { tag, frames: [[ch0, ch1?], ...] } and may override the partial-frame
// length written into the header with `len`.
function buildPacket(elements, { partial = true } = {}) {
    const bits = [];
    const w = (v, n) => { for (let i = n - 1; i >= 0; i--) bits.push((v >>> i) & 1); };
    for (const el of elements) {
        w(el.tag, 3);              // element tag
        w(0, 4);                   // element instance tag
        w(0, 12);                  // unused header bits
        w(partial ? 1 : 0, 1);     // partial frame flag
        w(0, 2);                   // shift-off flag
        w(1, 1);                   // escape flag: samples stored verbatim
        if (partial) w(el.len != null ? el.len : el.frames.length, 32);
        for (const frame of el.frames)
            for (const x of frame) w(x & 0xffff, 16);
    }
    w(7, 3);                       // ID_END
    while (bits.length % 8) bits.push(0);
    const b = Buffer.alloc(bits.length / 8);
    bits.forEach((v, i) => b[i >> 3] |= v << (7 - (i % 8)));
    return b;
}

// Expected interleaved output: frame-major, elements in packet order.
function interleave(elements) {
    const out = [];
    for (let i = 0; i < elements[0].frames.length; i++)
        for (const el of elements)
            for (const v of el.frames[i]) out.push(v);
    return out;
}

// A decoder wired to an Aurora-style event source, recording what it publishes.
function session(channels, frames = 4) {
    const demuxer = new AV.EventEmitter();
    const decoder = new ALAC(demuxer, {});
    decoder.setCookie(new AV.Buffer(cookie(channels, frames)));
    const s = { data: [], errors: [], decoder, demuxer };
    decoder.on('data', (packet) => s.data.push(Array.from(packet)));
    decoder.on('error', (err) => s.errors.push(err));
    s.feed = (buf) => demuxer.emit('data', new AV.Buffer(buf));
    s.decodeAll = () => { while (decoder.decode()); };
    return s;
}

// Feeding one buffer and decoding must yield exactly the expected packets.
function decodesTo(channels, packets, expected, frames = 4) {
    const s = session(channels, frames);
    s.feed(Buffer.concat(packets));
    s.decodeAll();
    assert.deepEqual(s.errors, []);
    assert.deepEqual(s.data, expected);
}

// A bad packet must surface one error and publish no (partial) PCM.
function failsWith(channels, packet, frames = 4) {
    const s = session(channels, frames);
    s.feed(packet);
    s.decodeAll();
    assert.equal(s.errors.length, 1, 'expected exactly one decode error');
    assert.deepEqual(s.data, [], 'bad packet must not publish partial PCM');
    return s.errors[0];
}

test('frame-element-contract', async (t) => {
    await t.test('elements interleave frame-by-frame in packet order', () => {
        // SCE followed by CPE
        decodesTo(3, [buildPacket([
            { tag: ID_SCE, frames: [[10], [20]] },
            { tag: ID_CPE, frames: [[30, 40], [50, 60]] },
        ])], [[10, 30, 40, 20, 50, 60]]);

        // CPE followed by SCE
        decodesTo(3, [buildPacket([
            { tag: ID_CPE, frames: [[30, 40], [50, 60]] },
            { tag: ID_SCE, frames: [[10], [20]] },
        ])], [[30, 40, 10, 50, 60, 20]]);

        // multiple SCEs
        decodesTo(3, [buildPacket([
            { tag: ID_SCE, frames: [[10], [20]] },
            { tag: ID_SCE, frames: [[30], [40]] },
            { tag: ID_SCE, frames: [[50], [60]] },
        ])], [[10, 30, 50, 20, 40, 60]]);

        // LFE occupies a single channel like an SCE, in both orders
        decodesTo(2, [buildPacket([
            { tag: ID_SCE, frames: [[10], [20]] },
            { tag: ID_LFE, frames: [[30], [40]] },
        ])], [[10, 30, 20, 40]]);
        decodesTo(2, [buildPacket([
            { tag: ID_LFE, frames: [[30], [40]] },
            { tag: ID_SCE, frames: [[10], [20]] },
        ])], [[30, 10, 40, 20]]);

        // eight channels from CPE x3 + SCE + LFE
        const surround = [
            { tag: ID_CPE, frames: [[1, 2], [11, 12]] },
            { tag: ID_CPE, frames: [[3, 4], [13, 14]] },
            { tag: ID_CPE, frames: [[5, 6], [15, 16]] },
            { tag: ID_SCE, frames: [[7], [17]] },
            { tag: ID_LFE, frames: [[8], [18]] },
        ];
        decodesTo(8, [buildPacket(surround)], [interleave(surround)]);

        // mono and stereo, full packets without a partial-frame header
        decodesTo(1, [buildPacket([{ tag: ID_SCE, frames: [[10], [20], [30], [40]] }], { partial: false })],
            [[10, 20, 30, 40]]);
        decodesTo(2, [buildPacket([{ tag: ID_CPE, frames: [[1, -1], [2, -2], [3, -3], [4, -4]] }], { partial: false })],
            [[1, -1, 2, -2, 3, -3, 4, -4]]);
    });

    await t.test('consecutive packets keep their own frame counts', () => {
        const lengths = [2, 4, 1, 3];
        const packets = lengths.map((len, p) => buildPacket([
            { tag: ID_SCE, frames: Array.from({ length: len }, (_, i) => [p * 100 + i]) },
        ]));
        decodesTo(1, packets,
            lengths.map((len, p) => Array.from({ length: len }, (_, i) => p * 100 + i)));
    });

    await t.test('real compressed packets match the reference PCM', () => {
        const dir = path.join(__dirname, '..', 'checks');
        const compressed = fs.readFileSync(path.join(dir, 'compressed.alac'));
        const pcm = fs.readFileSync(path.join(dir, 'compressed.pcm'));
        const tail = buildPacket([{ tag: ID_CPE, frames: [[1, -1], [2, -2], [3, -3], [4, -4]] }]);

        const s = session(2, 4096);
        s.feed(Buffer.concat([compressed, compressed, tail]));
        s.decodeAll();

        assert.deepEqual(s.errors, []);
        assert.equal(s.data.length, 3);
        const reference = [];
        for (let i = 0; i < 512 * 2; i++) reference.push(pcm.readInt16LE(i * 2));
        assert.deepEqual(s.data[0], reference);
        assert.deepEqual(s.data[1], reference);
        assert.deepEqual(s.data[2], [1, -1, 2, -2, 3, -3, 4, -4]);
    });

    await t.test('invalid element lengths are rejected', () => {
        // partial frame length of zero
        failsWith(1, buildPacket([{ tag: ID_SCE, len: 0, frames: [] }]));
        // partial frame length beyond the configured frameLength
        failsWith(1, buildPacket([{ tag: ID_SCE, len: 5, frames: [[1], [2], [3], [4], [5]] }]));
        // elements inside one packet disagree on the frame count
        failsWith(3, buildPacket([
            { tag: ID_SCE, frames: [[10], [20]] },
            { tag: ID_CPE, len: 3, frames: [[30, 40], [50, 60], [70, 80]] },
        ]));
        // more channels than configured
        failsWith(2, buildPacket([
            { tag: ID_CPE, frames: [[1, 2], [3, 4]] },
            { tag: ID_CPE, frames: [[5, 6], [7, 8]] },
        ]));
    });

    await t.test('ID_END with missing channels is an error, not zero padding', () => {
        const err = failsWith(2, buildPacket([{ tag: ID_SCE, frames: [[10], [20]] }]));
        assert.match(err.message, /channels/i);
    });

    await t.test('truncated packets wait for more data instead of failing', () => {
        const packet = buildPacket([{ tag: ID_SCE, frames: [[10], [20], [30], [40]] }]);
        const s = session(1);
        // a prefix long enough to start decoding, too short to finish
        s.feed(packet.subarray(0, packet.length - 4));
        s.decoder.decode();
        assert.deepEqual(s.data, []);
        assert.deepEqual(s.errors, [], 'underflow must not surface as a decode error');
        // delivering the rest lets Aurora retry and decode the packet
        s.feed(packet.subarray(packet.length - 4));
        assert.deepEqual(s.errors, []);
        assert.deepEqual(s.data, [[10, 20, 30, 40]]);
    });
});
