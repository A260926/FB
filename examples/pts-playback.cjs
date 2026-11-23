'use strict';
// End-to-end demonstration of syncToPTS playback. The material (a legal
// MPEG-TS with MPEG-1 I/P video and MP2 audio, including a 33-bit PTS wrap and
// an intentional gap) is generated here and fed through the real production
// TS demuxer, JS decoders, Player and WebAudio output. A controllable clock
// and recording Canvas/AudioContext stand in for the browser; the production
// classes perform every time conversion and scheduling decision.
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const assert = require('node:assert/strict');

const ROOT = path.join(__dirname, '..');
const TICKS = 90000;
const WRAP = 0x200000000;
const START_DELAY = 0.1;

class Bits {
	constructor() { this.a = []; }
	put(v, n) { for (let i = n - 1; i >= 0; i--) this.a.push((v >>> i) & 1); return this; }
	code(s) { for (const c of s) this.a.push(Number(c)); return this; }
	align() { while (this.a.length % 8) this.a.push(0); return this; }
	start(c) { return this.align().put(1, 24).put(c, 8); }
	bytes() {
		this.align();
		const out = Buffer.alloc(this.a.length / 8);
		this.a.forEach((v, i) => { out[i >> 3] |= v << (7 - (i % 8)); });
		return out;
	}
}

function sequenceHeader() {
	return new Bits().start(0xb3).put(16, 12).put(16, 12).put(1, 4).put(3, 4)
		.put(100, 18).put(1, 1).put(20, 10).put(0, 1).put(0, 1).put(0, 1).bytes();
}
function flatIFrame() {
	const b = new Bits();
	b.start(0).put(0, 10).put(1, 3).put(65535, 16).put(0, 1)
		.start(1).put(8, 5).put(0, 1).code('1').code('1');
	for (let k = 0; k < 6; k++) b.code(k < 4 ? '100' : '00').code('10');
	return b.bytes();
}
function flatPFrame(tref) {
	const b = new Bits();
	b.start(0).put(tref, 10).put(2, 3).put(65535, 16).put(0, 1).put(1, 3).put(0, 1)
		.start(1).put(8, 5).put(0, 1).code('00011');
	for (let k = 0; k < 6; k++) b.code(k < 4 ? '100' : '00').code('10');
	return b.bytes();
}
function nonflatIFrame() {
	const b = new Bits();
	b.start(0).put(0, 10).put(1, 3).put(65535, 16).put(0, 1)
		.start(1).put(8, 5).put(0, 1).code('1').code('1');
	for (let k = 0; k < 6; k++) {
		b.code(k < 4 ? '100' : '00');
		if (k === 0) b.code('110');
		b.code('10');
	}
	return b.bytes();
}
function mp2Frame() {
	const b = new Bits();
	b.put(0xfffd8400, 32);
	for (let sb = 0; sb < 27; sb++)
		for (let ch = 0; ch < 2; ch++)
			b.put(sb === 0 ? 1 : 0, sb < 11 ? 4 : sb < 23 ? 3 : 2);
	b.put(2, 2).put(2, 2).put(0, 6).put(3, 6);
	for (let gr = 0; gr < 12; gr++) b.put(gr % 2 ? 26 : 0, 5).put(gr % 2 ? 0 : 26, 5);
	const out = Buffer.alloc(384);
	b.bytes().copy(out);
	return out;
}
function ptsTimestamp(tick) {
	return new Bits().put(2, 4)
		.put(Math.floor(tick / 1073741824), 3).put(1, 1)
		.put(Math.floor(tick / 32768) % 32768, 15).put(1, 1)
		.put(tick % 32768, 15).put(1, 1).bytes();
}
function tsPackets(pid, streamId, tick, payload, ccStart) {
	const pes = Buffer.concat([
		Buffer.from([0, 0, 1, streamId, (payload.length + 8) >> 8, (payload.length + 8) & 255, 0x80, 0x80, 5]),
		ptsTimestamp(tick), payload
	]);
	const out = [];
	let cc = ccStart || 0;
	for (let at = 0; at < pes.length;) {
		const n = Math.min(184, pes.length - at);
		const pkt = Buffer.alloc(188, 0xff);
		const start = at === 0;
		pkt[0] = 0x47;
		pkt[1] = (start ? 0x40 : 0) | (pid >> 8);
		pkt[2] = pid & 255;
		let pos = 4;
		if (n < 184) {
			pkt[3] = 0x30 | (cc++ & 15);
			pkt[4] = 183 - n;
			if (pkt[4]) pkt[5] = 0;
			pos = 188 - n;
		}
		else {
			pkt[3] = 0x10 | (cc++ & 15);
		}
		pes.copy(pkt, pos, at, at + n);
		out.push(pkt);
		at += n;
	}
	return Buffer.concat(out);
}

// --- Load the real production sources into a browser-like sandbox ----------
const J = { Decoder: {}, Demuxer: {}, AudioOutput: {}, Renderer: {}, Source: {}, Now: null, Fill: (a, v) => a.fill(v) };
let clock = 0;
const rendered = [];
const audioStarts = [];
class RecordingAudioContext {
	constructor() { this.currentTime = 0; this.destination = {}; this._connections = 0; }
	createGain() { return { gain: { value: 1 }, connect() {}, disconnect() {} }; }
	createBuffer(channels, length, sampleRate) {
		return {
			numberOfChannels: channels, length, sampleRate,
			duration: length / sampleRate,
			_data: [new Float32Array(length), new Float32Array(length)],
			getChannelData(i) { return this._data[i]; }
		};
	}
	createBufferSource() {
		const ctx = this;
		return {
			buffer: null, connect() {},
			start(when) {
				audioStarts.push({
					when, rate: this.buffer.sampleRate, length: this.buffer.length,
					left: Float32Array.from(this.buffer.getChannelData(0)),
					right: Float32Array.from(this.buffer.getChannelData(1))
				});
			}
		};
	}
	close() {}
}
const audioCtx = new RecordingAudioContext();
const scope = {
	JSMpeg: J, console,
	requestAnimationFrame() { return 1; }, cancelAnimationFrame() {},
	navigator: { userAgent: 'node-demo' },
	document: { addEventListener() {}, visibilityState: 'visible' },
	window: { AudioContext: function () { return audioCtx; }, performance: { now: () => clock * 1000 } },
	performance: { now: () => clock * 1000 }
};
vm.createContext(scope);
for (const name of ['buffer.js', 'decoder.js', 'ts.js', 'mpeg1.js', 'mp2.js', 'webaudio.js', 'player.js']) {
	vm.runInContext(fs.readFileSync(path.join(ROOT, 'src', name), 'utf8'), scope, { filename: name });
}
J.Now = () => clock;
J.Renderer.Canvas2D = class {
	resize(w, h) { this.w = w; this.h = h; }
	render(y, cr, cb) { rendered.push({ at: clock, ysum: y.reduce((a, b) => a + b, 0), ymin: Math.min(...y), ymax: Math.max(...y) }); }
	renderProgress() {} destroy() {}
};
class Source {
	constructor() { this.streaming = true; this.established = true; this.progress = 1; this.completed = false; }
	connect(d) { this.destination = d; } start() {} destroy() {}
}

// --- Build a stream that crosses the 33-bit wrap and contains a gap --------
// Video begins just before the wrap; the second picture lands after it. Audio
// starts on the far side of the wrap, and one video frame interval is skipped.
const v0 = WRAP - 2 * TICKS;            // two seconds before the wrap point
const v1 = WRAP + 2 * TICKS;            // four seconds of media time later (gap included)
const v2 = WRAP + 3 * TICKS;
const a0 = WRAP - TICKS;                // one second before the wrap
const a1 = WRAP - TICKS + 2160;         // 24ms after the first audio frame
const stream = Buffer.concat([
	tsPackets(256, 0xe0, v0, Buffer.concat([sequenceHeader(), nonflatIFrame()]), 0),
	tsPackets(257, 0xc0, a0, mp2Frame(), 0),
	tsPackets(257, 0xc0, a1, mp2Frame(), 1),
	tsPackets(256, 0xe0, v1, flatPFrame(2), 1),
	tsPackets(256, 0xe0, v2, flatPFrame(3), 2)
]);

const player = new J.Player('', {
	source: Source, streaming: true, disableWebAssembly: true, disableGl: true,
	video: true, audio: true, pauseWhenHidden: false, decodeFirstFrame: false,
	syncToPTS: true, loop: false, videoBufferSize: 16384, audioBufferSize: 16384
});

// TS bytes may be split arbitrarily; use 7-byte writes.
for (let at = 0; at < stream.length; at += 7) {
	player.source.destination.write(stream.subarray(at, at + 7));
}

// Independent expected table, derived straight from the raw tick schedule.
const originTick = Math.min(v0, a0);                 // earliest presentation time
const wallFor = (tick) => START_DELAY + (tick - originTick) / TICKS;
const expectedVideo = [v0, v1, v2].map(wallFor);
const expectedAudio = [a0, a1].map(wallFor);

// Drive the clock in fine steps across the whole media window.
for (let t = 0; t <= 6; t += 0.01) {
	clock = t;
	audioCtx.currentTime = t;
	player.update();
}

// Golden reference: decode the same elementary stream straight through the
// production MPEG-1 decoder, in arrival order, without any Player timing.
const goldenDecoder = new J.Decoder.MPEG1Video({ decodeFirstFrame: false });
const golden = [];
goldenDecoder.connect({
	resize() {},
	render(y) { golden.push({ ymin: Math.min(...y), ymax: Math.max(...y), ysum: y.reduce((a, b) => a + b, 0) }); }
});
goldenDecoder.write(0, Buffer.concat([
	sequenceHeader(), nonflatIFrame(), flatPFrame(2), flatPFrame(3),
	new Bits().start(0xb7).bytes()
]));
let guard = 0;
while (goldenDecoder.decode()) assert.ok(++guard < 100);
assert.equal(golden.length, 3);

// --- Verify picture timing and pixels went through the real MPEG-1 decoder -
assert.equal(rendered.length, 3, 'all three pictures presented');
rendered.forEach((r, i) => {
	assert.ok(Math.abs(r.at - expectedVideo[i]) < 0.011, 'picture ' + i + ' rendered at its PTS');
	assert.equal(r.ysum, golden[i].ysum, 'picture ' + i + ' pixels match the golden decoder');
	assert.equal(r.ymin, golden[i].ymin);
	assert.equal(r.ymax, golden[i].ymax);
});
assert.equal(rendered[0].ymin, 125, 'first picture is the non-flat I frame');
assert.equal(rendered[0].ymax, 131);

// --- Verify audio source.start timing, gaps and real decoded PCM ------------
assert.equal(audioStarts.length, 2, 'one source node per MP2 frame, none for the gap');
audioStarts.forEach((s, i) => {
	assert.ok(Math.abs(s.when - expectedAudio[i]) < 1e-12, 'audio ' + i + ' scheduled at its PTS');
	assert.equal(s.rate, 48000);
	assert.equal(s.length, 1152);
	assert.ok(s.left.some(x => x !== 0), 'decoded PCM is non-zero');
	assert.ok(s.left.some((x, k) => x !== s.right[k]), 'stereo channels are distinct');
});
assert.ok(Math.abs((audioStarts[1].when - audioStarts[0].when) - 2160 / TICKS) < 1e-12, 'audio cadence follows PTS, not arrival order');
assert.ok(Math.abs(player.currentTime - (clock - START_DELAY)) < 0.011, 'currentTime is the shared media coordinate');
assert.equal(player.ptsSync.anchors.video.length + player.ptsSync.anchors.audio.length, 0, 'all anchors drained');

console.log('video present times (s):', expectedVideo.map(x => +x.toFixed(3)).join(', '));
console.log('audio source.start (s): ', expectedAudio.map(x => +x.toFixed(3)).join(', '));
console.log('pictures=' + rendered.length + ' audioNodes=' + audioStarts.length + ' wrap=2^33 gap=retained');
console.log('PTS_PLAYBACK_DEMO_OK');
