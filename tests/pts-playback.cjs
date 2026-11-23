'use strict';
// PTS driven playback tests. All materials (TS packets, MPEG-1 I/P pictures
// and MP2 frames) are constructed here; production classes do the real
// demuxing, decoding, time conversion and scheduling. Fakes only record.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

const SOURCE_FILES = [
	'buffer.js', 'decoder.js', 'ts.js', 'mpeg1.js',
	'mp2.js', 'webaudio.js', 'player.js'
];
const WRAP = 0x200000000; // 2^33 ticks of the 33-bit 90kHz PTS counter
const TICKS_PER_SECOND = 90000;
const START_DELAY = 0.1;

// ---------------------------------------------------------------------------
// Bitstream / TS material construction (independent of production code)
// ---------------------------------------------------------------------------

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
	const b = new Bits();
	// 16x16, aspect 1, 25fps(code 3), bitrate 100, vbv 20
	b.start(0xb3).put(16, 12).put(16, 12).put(1, 4).put(3, 4)
		.put(100, 18).put(1, 1).put(20, 10).put(0, 1).put(0, 1).put(0, 1);
	return b.bytes();
}

// Flat grey (Y=128) intra picture; pattern proven against the production decoder.
function flatIFrame() {
	const b = new Bits();
	b.start(0).put(0, 10).put(1, 3).put(65535, 16).put(0, 1)
		.start(1).put(8, 5).put(0, 1).code('1').code('1');
	for (let k = 0; k < 6; k++) b.code(k < 4 ? '100' : '00').code('10');
	return b.bytes();
}

// Predictive picture; with zero motion vectors and no residuals it copies the
// forward reference. tref lets each picture stay a legal, distinct header.
function flatPFrame(tref) {
	const b = new Bits();
	b.start(0).put(tref, 10).put(2, 3).put(65535, 16).put(0, 1).put(1, 3).put(0, 1)
		.start(1).put(8, 5).put(0, 1).code('00011');
	for (let k = 0; k < 6; k++) b.code(k < 4 ? '100' : '00').code('10');
	return b.bytes();
}

// Non-flat intra picture (luma Y in 125..131), exact pattern validated against
// the production decoder in task_support/nonflat.cjs.
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

// MPEG-1 Layer II, 128 kb/s stereo at 48 kHz; only subband 0 allocated with
// asymmetric samples so both channels decode to non-zero, different PCM.
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
	const b = new Bits();
	b.put(2, 4)
		.put(Math.floor(tick / 1073741824), 3).put(1, 1)
		.put(Math.floor(tick / 32768) % 32768, 15).put(1, 1)
		.put(tick % 32768, 15).put(1, 1);
	return b.bytes();
}

function pesPacket(streamId, tick, payload) {
	const len = payload.length + 8;
	return Buffer.concat([
		Buffer.from([0, 0, 1, streamId, len >> 8, len & 255, 0x80, 0x80, 5]),
		ptsTimestamp(tick), payload
	]);
}

// Split one PES over as many 188-byte TS packets as needed, padding the last.
function tsPackets(pid, streamId, tick, payload, ccStart) {
	const pes = pesPacket(streamId, tick, payload);
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

const VIDEO_PID = 256, AUDIO_PID = 257;
const VIDEO_ID = 0xe0, AUDIO_ID = 0xc0;

// A framed piece of the stream: kind 'I' (flat), 'N' (non-flat I), 'P', 'A'.
function framePacket(spec, index) {
	if (spec.kind === 'A') {
		return tsPackets(AUDIO_PID, AUDIO_ID, spec.tick, mp2Frame(), index & 15);
	}
	const picture = spec.kind === 'N' ? nonflatIFrame()
		: spec.kind === 'I' ? flatIFrame()
		: flatPFrame(index + 1);
	// Optional 0xff padding inflates the ES payload to exercise streaming
	// eviction; it never forms a 000001 start code and is skipped by the
	// decoder's start-code scan between pictures.
	const padded = spec.pad ? Buffer.concat([picture, Buffer.alloc(spec.pad, 0xff)]) : picture;
	const payload = spec.first
		? Buffer.concat([sequenceHeader(), padded])
		: padded;
	return tsPackets(VIDEO_PID, VIDEO_ID, spec.tick, payload, index & 15);
}

function buildStream(specs) {
	return Buffer.concat(specs.map((s, i) => framePacket(s, i)));
}

function feedInChunks(player, data, chunkSize) {
	for (let at = 0; at < data.length; at += chunkSize) {
		player.source.destination.write(data.subarray(at, at + chunkSize));
	}
}

// Independent expected wall-clock table from the raw tick table.
function expectedWall(tick, originTick) {
	return START_DELAY + (tick - originTick) / TICKS_PER_SECOND;
}

function pictureSignature(y, cr, cb) {
	return {
		ymin: Math.min(...y),
		ymax: Math.max(...y),
		ysum: y.reduce((a, b) => a + b, 0),
		crFlat: cr.every(v => v === cr[0]),
		cbFlat: cb.every(v => v === cb[0]),
		y: Uint8Array.from(y),
		cr: Uint8Array.from(cr),
		cb: Uint8Array.from(cb)
	};
}

// ---------------------------------------------------------------------------
// VM harness: production sources with controllable clock and recording fakes
// ---------------------------------------------------------------------------

function loadProduction() {
	const J = {
		Decoder: {}, Demuxer: {}, AudioOutput: {}, Renderer: {}, Source: {},
		Now: null, Fill: (a, v) => a.fill(v)
	};
	const scope = {
		JSMpeg: J, console,
		requestAnimationFrame() { return 1; },
		cancelAnimationFrame() {},
		navigator: { userAgent: 'node-test' },
		document: { addEventListener() {}, visibilityState: 'visible' },
		window: null, performance: null
	};
	vm.createContext(scope);
	for (const name of SOURCE_FILES) {
		vm.runInContext(fs.readFileSync(path.join('src', name), 'utf8'), scope, { filename: name });
	}
	return { J, scope };
}

class FakeAudioContext {
	constructor() {
		this.currentTime = 0;
		this.destination = {};
		this.sampleRate = 48000;
		this.starts = [];
		this._connections = 0;
	}
	createGain() {
		return {
			gain: { value: 1 },
			connect() {}, disconnect() {}
		};
	}
	createBuffer(channels, length, sampleRate) {
		return {
			numberOfChannels: channels, length,
			duration: length / sampleRate, sampleRate,
			_data: [new Float32Array(length), new Float32Array(length)],
			getChannelData(i) { return this._data[i]; }
		};
	}
	createBufferSource() {
		const ctx = this;
		return {
			buffer: null,
			connect() {},
			start(when) {
				ctx.starts.push({
					when,
					rate: this.buffer.sampleRate,
					length: this.buffer.length,
					left: Float32Array.from(this.buffer.getChannelData(0)),
					right: Float32Array.from(this.buffer.getChannelData(1))
				});
			}
		};
	}
	close() {}
}

function createHarness(overrides) {
	const opts = Object.assign({ video: true, audio: true, videoBufferSize: 4096, audioBufferSize: 4096 }, overrides);
	const { J, scope } = loadProduction();
	let now = 0;
	const audioCtx = opts.audio ? new FakeAudioContext() : null;
	J.Now = () => now;
	scope.window = { AudioContext: opts.audio ? function () { return audioCtx; } : undefined, performance: { now: () => now * 1000 } };
	scope.performance = { now: () => now * 1000 };

	const renders = [];
	// The canvas fake only records pixels and clock times; all decoding and
	// scheduling stays in the production classes.
	J.Renderer.Canvas2D = class {
		resize(w, h) { this.w = w; this.h = h; }
		render(y, cr, cb) {
			renders.push({ at: now, sig: pictureSignature(y, cr, cb) });
		}
		renderProgress() {}
		destroy() {}
	};

	class Source {
		constructor() { this.streaming = true; this.established = true; this.progress = 1; this.completed = false; }
		connect(d) { this.destination = d; }
		start() {}
		destroy() {}
	}

	const player = new J.Player('', {
		source: Source, streaming: true, disableWebAssembly: true, disableGl: true,
		video: opts.video, audio: opts.audio, pauseWhenHidden: false,
		decodeFirstFrame: false, syncToPTS: true, loop: false,
		videoBufferSize: opts.videoBufferSize, audioBufferSize: opts.audioBufferSize
	});

	return {
		J, player, renders,
		get audioStarts() { return audioCtx ? audioCtx.starts : []; },
		audioCtx,
		now: () => now,
		setNow(t) { now = t; if (audioCtx) audioCtx.currentTime = t; },
		setAudioClock(t) { if (audioCtx) audioCtx.currentTime = t; },
		tick(t) {
			if (t !== undefined) this.setNow(t);
			player.update();
		},
		feed(data, chunkSize) { feedInChunks(player, data, chunkSize || 7); }
	};
}

// Golden references: the same elementary streams decoded by the production
// decoders straight, outside any player, in pure arrival order.
function goldenVideo(J, frameSpecs) {
	const parts = [sequenceHeader()];
	for (const s of frameSpecs) {
		parts.push(s.kind === 'N' ? nonflatIFrame()
			: s.kind === 'I' ? flatIFrame()
			: flatPFrame(parts.length));
	}
	parts.push(new Bits().start(0xb7).bytes());
	const dec = new J.Decoder.MPEG1Video({ decodeFirstFrame: false });
	const out = [];
	dec.connect({ resize() {}, render(y, cr, cb) { out.push(pictureSignature(y, cr, cb)); } });
	dec.write(0, Buffer.concat(parts));
	let guard = 0;
	while (dec.decode()) assert.ok(++guard < 100);
	return out;
}

function goldenAudio(J, count) {
	const dec = new J.Decoder.MP2Audio({ streaming: false });
	const out = [];
	dec.connect({
		play(rate, l, r) {
			out.push({ rate, length: l.length, left: Float32Array.from(l), right: Float32Array.from(r) });
		}
	});
	const frames = [];
	for (let i = 0; i < count; i++) frames.push(mp2Frame());
	dec.write(0, Buffer.concat(frames));
	let guard = 0;
	while (dec.decode()) assert.ok(++guard < 100);
	return out;
}

// ---------------------------------------------------------------------------
// Test groups
// ---------------------------------------------------------------------------

const tests = [];

// --- transport -------------------------------------------------------------

tests.push({
	group: 'transport',
	name: 'one-anchor-per-pes-with-one-byte-chunks',
	run(check) {
		const h = createHarness();
		const specs = [
			{ kind: 'I', tick: 90000, first: true }, { kind: 'P', tick: 93600 },
			{ kind: 'A', tick: 90000 }, { kind: 'A', tick: 92160 },
			{ kind: 'P', tick: 97200 }, { kind: 'A', tick: 94320 }
		];
		h.feed(buildStream(specs), 1);
		const s = h.player.ptsSync;
		check(s.anchors.video.length === 3, 'three video PES produce three anchors');
		check(s.anchors.audio.length === 3, 'three audio PES produce three anchors');
		check(JSON.stringify(s.anchors.video.map(a => a.tick)) === JSON.stringify([90000, 93600, 97200]), 'video ticks in order');
		check(JSON.stringify(s.anchors.audio.map(a => a.tick)) === JSON.stringify([90000, 92160, 94320]), 'audio ticks in order');
	}
});

tests.push({
	group: 'transport',
	name: 'chunk-shapes-1-7-187-give-identical-anchors',
	run(check) {
		const specs = [
			{ kind: 'I', tick: 90000, first: true }, { kind: 'P', tick: 93600 },
			{ kind: 'A', tick: 91080 }, { kind: 'P', tick: 97200 }, { kind: 'A', tick: 93240 }
		];
		const seen = [];
		for (const chunk of [1, 7, 187]) {
			const h = createHarness();
			h.feed(buildStream(specs), chunk);
			seen.push({
				v: h.player.ptsSync.anchors.video.map(a => a.tick),
				a: h.player.ptsSync.anchors.audio.map(a => a.tick)
			});
		}
		check(JSON.stringify(seen[0]) === JSON.stringify(seen[1]), '1 vs 7 byte chunks match');
		check(JSON.stringify(seen[1]) === JSON.stringify(seen[2]), '7 vs 187 byte chunks match');
	}
});

tests.push({
	group: 'transport',
	name: 'pes-split-over-many-ts-packets-builds-one-frame',
	run(check) {
		const h = createHarness({ audio: false });
		// One video PES deliberately larger than two TS payloads.
		const payload = Buffer.concat([sequenceHeader(), nonflatIFrame(), Buffer.alloc(400, 0x11)]);
		const data = tsPackets(VIDEO_PID, VIDEO_ID, 90000, payload, 0);
		check(data.length >= 188 * 3, 'fixture PES spans at least three TS packets');
		h.feed(data, 1);
		const anchors = h.player.ptsSync.anchors.video;
		check(anchors.length === 1, 'split PES yields exactly one playback frame');
		check(anchors[0].tick === 90000, 'anchor keeps the PES PTS');
		check(anchors[0].bytes === payload.length, 'anchor bytes equal the full PES payload');
	}
});

tests.push({
	group: 'transport',
	name: 'raw-33bit-tick-survives-demuxer-without-rescaling',
	run(check) {
		const h = createHarness();
		const raw = WRAP - 7; // needs all 33 bits
		const got = [];
		h.player.demuxer.onFrame = (streamId, tick) => got.push({ streamId, tick });
		h.feed(tsPackets(AUDIO_PID, AUDIO_ID, raw, mp2Frame(), 0), 7);
		check(got.length === 1, 'one frame callback');
		check(got[0].tick === raw, 'tick is the exact 90kHz integer, not seconds');
		check(got[0].streamId === AUDIO_ID, 'stream id reported');
	}
});

tests.push({
	group: 'transport',
	name: 'muxed-pids-route-to-the-right-track',
	run(check) {
		const h = createHarness();
		const data = Buffer.concat([
			tsPackets(VIDEO_PID, VIDEO_ID, 50000, Buffer.concat([sequenceHeader(), flatIFrame()]), 0),
			tsPackets(AUDIO_PID, AUDIO_ID, 51000, mp2Frame(), 0),
			tsPackets(VIDEO_PID, VIDEO_ID, 53600, flatPFrame(2), 1),
			tsPackets(AUDIO_PID, AUDIO_ID, 53160, mp2Frame(), 1)
		]);
		h.feed(data, 13);
		check(JSON.stringify(h.player.ptsSync.anchors.video.map(a => a.tick)) === JSON.stringify([50000, 53600]), 'video route');
		check(JSON.stringify(h.player.ptsSync.anchors.audio.map(a => a.tick)) === JSON.stringify([51000, 53160]), 'audio route');
	}
});

tests.push({
	group: 'transport',
	name: 'sync-to-pts-only-enables-on-streaming-js-chain',
	run(check) {
		// Default streaming path: option absent, no PTS controller.
		const def = createHarness();
		const defPlayer = new def.J.Player('', {
			source: def.player.source.constructor, streaming: true, disableWebAssembly: true,
			disableGl: true, video: true, audio: false, pauseWhenHidden: false, loop: false
		});
		check(defPlayer.ptsMode === false, 'default path does not enter PTS mode');
		check(defPlayer.ptsSync === undefined, 'no PTS controller on the default path');
		// Explicit opt-in engages the controller only for streaming + JS.
		const h = createHarness({ audio: false });
		check(h.player.ptsMode === true, 'streaming + disableWebAssembly engages PTS mode');
		check(typeof h.player.ptsSync === 'object' && h.player.demuxer.onFrame !== null, 'controller and demuxer hook installed');
		// Non-streaming (static file) keeps the original behavior.
		class StaticSource {
			constructor() { this.streaming = false; this.established = true; this.progress = 1; this.completed = true; }
			connect(d) { this.destination = d; } start() {} resume() {} destroy() {}
		}
		const staticPlayer = new h.J.Player('x.ts', {
			source: StaticSource, progressive: false, disableWebAssembly: true, syncToPTS: true,
			video: false, audio: false, pauseWhenHidden: false
		});
		check(staticPlayer.ptsMode === false, 'static file ignores syncToPTS');
		staticPlayer.destroy();
	}
});

tests.push({
	group: 'transport',
	name: 'inflight-bytes-track-only-unconsumed-compressed-data',
	run(check) {
		const h = createHarness({ audio: false });
		const specs = [
			{ kind: 'I', tick: 90000, first: true },
			{ kind: 'P', tick: 93600 }, { kind: 'P', tick: 97200 }, { kind: 'P', tick: 100800 }
		];
		h.feed(buildStream(specs), 7);
		const s = h.player.ptsSync;
		const total = s.anchors.video.reduce((a, x) => a + x.bytes, 0);
		check(s.inflight.video === total, 'inflight equals held PES payload bytes');
		h.setNow(0.5); h.tick(); // arm + render first frame
		const rest = s.anchors.video.reduce((a, x) => a + x.bytes, 0);
		check(s.inflight.video === rest, 'inflight drops by the consumed PES size');
		check(s.inflight.video > 0, 'later frames remain buffered');
	}
});

// --- anchors ---------------------------------------------------------------

tests.push({
	group: 'anchors',
	name: 'wrap-audio-first-then-video-preserves-offset',
	run(check) {
		const h = createHarness();
		const a0 = WRAP - 50000, v0 = 100000;
		const data = Buffer.concat([
			tsPackets(AUDIO_PID, AUDIO_ID, a0, mp2Frame(), 0),
			tsPackets(VIDEO_PID, VIDEO_ID, v0, Buffer.concat([sequenceHeader(), flatIFrame()]), 0)
		]);
		h.feed(data, 7);
		const s = h.player.ptsSync;
		check(s.anchors.audio[0].tick === WRAP - 50000, 'first track keeps its raw tick');
		check(s.anchors.video[0].tick === WRAP + 100000, 'video placed on the cycle nearest the common time');
		check(s.anchors.video[0].tick - s.anchors.audio[0].tick === 150000, 'original 150000-tick offset preserved');
	}
});

tests.push({
	group: 'anchors',
	name: 'wrap-video-first-then-audio-preserves-offset',
	run(check) {
		const h = createHarness();
		const a0 = WRAP - 50000, v0 = 100000;
		const data = Buffer.concat([
			tsPackets(VIDEO_PID, VIDEO_ID, v0, Buffer.concat([sequenceHeader(), flatIFrame()]), 0),
			tsPackets(AUDIO_PID, AUDIO_ID, a0, mp2Frame(), 0)
		]);
		h.feed(data, 7);
		const s = h.player.ptsSync;
		check(s.anchors.video[0].tick === 100000, 'first track keeps its raw tick');
		check(s.anchors.audio[0].tick === -50000, 'audio placed on the earlier cycle');
		check(s.anchors.video[0].tick - s.anchors.audio[0].tick === 150000, 'same offset regardless of arrival order');
	}
});

tests.push({
	group: 'anchors',
	name: 'consecutive-wrap-inside-one-track-unwraps-monotonically',
	run(check) {
		const h = createHarness({ audio: false });
		const data = Buffer.concat([
			tsPackets(VIDEO_PID, VIDEO_ID, WRAP - 3000, Buffer.concat([sequenceHeader(), flatIFrame()]), 0),
			tsPackets(VIDEO_PID, VIDEO_ID, 6000, flatPFrame(2), 1)
		]);
		h.feed(data, 7);
		const ticks = h.player.ptsSync.anchors.video.map(a => a.tick);
		check(ticks[0] === WRAP - 3000, 'pre-wrap tick');
		check(ticks[1] === WRAP + 6000, 'post-wrap tick continues past 2^33');
		check(ticks[1] > ticks[0], 'unwrapped time increases');
	}
});

tests.push({
	group: 'anchors',
	name: 'start-offset-and-gap-remain-in-media-coordinates',
	run(check) {
		const h = createHarness();
		const vTicks = [900000, 1080000]; // one frame interval missing between them
		const aTick = 990000; // audio starts exactly one second after video
		const data = Buffer.concat([
			tsPackets(VIDEO_PID, VIDEO_ID, vTicks[0], Buffer.concat([sequenceHeader(), flatIFrame()]), 0),
			tsPackets(AUDIO_PID, AUDIO_ID, aTick, mp2Frame(), 0),
			tsPackets(VIDEO_PID, VIDEO_ID, vTicks[1], flatPFrame(2), 1)
		]);
		h.feed(data, 7);
		h.setNow(2); h.tick();
		const s = h.player.ptsSync;
		check(s.originTick === 900000, 'earliest presentation time is the media zero');
		check(Math.abs(h.player.ptsToClock(990000) - h.player.ptsToClock(900000) - 1) < 1e-12, 'one second A/V offset');
		check(Math.abs(h.player.ptsToClock(1080000) - h.player.ptsToClock(900000) - 2) < 1e-12, 'two second gap retained');
		check(s.anchors.video.length === 2, 'no anchor invented for the missing frame');
	}
});

tests.push({
	group: 'anchors',
	name: 'repeated-eviction-keeps-anchor-to-frame-alignment',
	run(check) {
		const h = createHarness({ audio: false, videoBufferSize: 512 });
		const specs = [{ kind: 'I', tick: 90000, first: true, pad: 220 }];
		for (let i = 1; i < 24; i++) specs.push({ kind: i % 3 === 0 ? 'I' : 'P', tick: 90000 + i * 3600, pad: 220 });
		const golden = goldenVideo(h.J, specs);
		let evictions = 0;
		const origEvict = h.J.BitBuffer.prototype.evict;
		h.J.BitBuffer.prototype.evict = function (size) { evictions++; return origEvict.call(this, size); };
		const data = buildStream(specs);
		// Feed and pump in small rounds so eviction reclaims consumed bytes.
		for (let at = 0; at < data.length; at += 37) {
			h.player.source.destination.write(data.subarray(at, Math.min(at + 37, data.length)));
			h.setNow(0.1 + (at / data.length) * 3);
			h.tick();
		}
		h.setNow(10); h.tick();
		check(evictions >= 3, 'multiple eviction rounds happened (' + evictions + ')');
		check(h.renders.length === golden.length, 'every frame rendered exactly once across evictions');
		check(h.renders.every((r, i) => r.sig.ysum === golden[i].ysum && r.sig.ymin === golden[i].ymin), 'pixels stay aligned to frames');
		check(h.player.ptsSync.anchors.video.length === 0, 'all anchors released after consumption');
		check(h.player.video.bits.bytes.length <= 512, 'compressed buffer never exceeds bufferSize');
	}
});

tests.push({
	group: 'anchors',
	name: 'empty-update-advances-neither-decode-position-nor-origin',
	run(check) {
		const h = createHarness({ audio: false });
		const specs = [
			{ kind: 'I', tick: 90000, first: true },
			{ kind: 'P', tick: 180000 }, { kind: 'P', tick: 270000 }
		];
		h.feed(buildStream(specs), 7);
		h.setNow(0.0); h.tick();
		const origin = h.player.ptsSync.wallOrigin;
		h.setNow(0.11); h.tick();
		check(h.renders.length === 1, 'first picture due');
		const indexAfterFirst = h.player.video.bits.index;
		const anchorsAfterFirst = h.player.ptsSync.anchors.video.length;
		for (const t of [0.12, 0.12, 0.5, 0.9]) { h.setNow(t); h.tick(); }
		check(h.renders.length === 1, 'no new picture without a due frame');
		check(h.player.video.bits.index === indexAfterFirst, 'decode position unchanged');
		check(h.player.ptsSync.anchors.video.length === anchorsAfterFirst, 'undue anchors retained');
		check(h.player.ptsSync.wallOrigin === origin, 'media zero not re-bound to the wall clock');
		h.setNow(1.2); h.tick();
		check(h.renders.length === 2, 'gap ends: second picture appears at its own time');
	}
});

tests.push({
	group: 'anchors',
	name: 'anchor-count-stays-bounded-by-in-flight-frames',
	run(check) {
		const h = createHarness({ audio: false, videoBufferSize: 8192 });
		const specs = [{ kind: 'I', tick: 90000, first: true }];
		for (let i = 1; i < 30; i++) specs.push({ kind: 'P', tick: 90000 + i * 3600 });
		h.feed(buildStream(specs), 7);
		let maxAnchors = h.player.ptsSync.anchors.video.length;
		for (let t = 0.1; t < 2.0; t += 0.05) {
			h.setNow(t); h.tick();
			maxAnchors = Math.max(maxAnchors, h.player.ptsSync.anchors.video.length);
		}
		check(maxAnchors <= specs.length, 'anchors never outnumber the frames fed');
		check(h.player.ptsSync.anchors.video.length === 0, 'all consumed by the end');
		// No whole-stream timestamp cache exists beside the anchors.
		check(!h.player.video.timestamps || h.player.video.timestamps.length === 0, 'decoder holds no PTS table in streaming mode');
	}
});

// --- playout ---------------------------------------------------------------

tests.push({
	group: 'playout',
	name: 'video-presents-at-pts-wall-times-and-holds-through-gaps',
	run(check) {
		const h = createHarness({ audio: false });
		const vTicks = [90000, 126000, 162000, 198000]; // 400ms gaps after first
		const specs = [
			{ kind: 'I', tick: vTicks[0], first: true },
			{ kind: 'P', tick: vTicks[1] }, { kind: 'P', tick: vTicks[2] }, { kind: 'P', tick: vTicks[3] }
		];
		const golden = goldenVideo(h.J, specs);
		h.feed(buildStream(specs), 7);
		const expected = vTicks.map(t => expectedWall(t, vTicks[0]));
		h.setNow(0); h.tick(); // arm with wallOrigin exactly START_DELAY
		check(Math.abs(h.player.ptsSync.wallOrigin - 0.1) < 1e-12, 'armed 100ms after the zero clock');
		h.setNow(0.05); h.tick();
		check(h.renders.length === 0, 'nothing before the 100ms start');
		h.setNow(expected[0]); h.tick();
		h.setNow((expected[0] + expected[1]) / 2); h.tick();
		check(h.renders.length === 1, 'previous picture held through the gap');
		for (let i = 1; i < expected.length; i++) {
			h.setNow(expected[i] - 1e-9); h.tick();
			check(h.renders.length === i, 'not rendered early');
			h.setNow(expected[i]); h.tick();
		}
		check(h.renders.length === 4, 'all pictures rendered');
		check(h.renders.every((r, i) => Math.abs(r.at - expected[i]) < 1e-12), 'render clock table matches the tick table');
		check(h.renders.every((r, i) => r.sig.ysum === golden[i].ysum && r.sig.ymin === golden[i].ymin && r.sig.ymax === golden[i].ymax), 'pixels match golden decode order');
	}
});

tests.push({
	group: 'playout',
	name: 'audio-source-start-follows-pts-with-silent-gaps',
	run(check) {
		const h = createHarness({ video: false });
		// 48kHz / 1152 samples = 24ms per MP2 frame; leave a 100ms hole between frame 2 and 3.
		const aTicks = [90000, 92160, 103320, 105480];
		const data = Buffer.concat(aTicks.map((t, i) => tsPackets(AUDIO_PID, AUDIO_ID, t, mp2Frame(), i & 15)));
		const golden = goldenAudio(h.J, aTicks.length);
		h.feed(data, 7);
		h.setNow(0); h.tick(); // arm with wallOrigin exactly START_DELAY
		h.setNow(1); h.tick();
		const starts = h.audioStarts;
		check(starts.length === aTicks.length, 'one source node per frame, none for the gap');
		const expected = aTicks.map(t => expectedWall(t, aTicks[0]));
		check(starts.every((s, i) => Math.abs(s.when - expected[i]) < 1e-12), 'source.start table matches the tick table');
		check(starts[2].when - starts[1].when > (starts[1].when - starts[0].when) + 0.05, 'gap is silence, not concatenated audio');
		check(starts.every((s, i) => s.length === golden[i].length && s.rate === golden[i].rate), 'PCM shape matches golden');
		check(starts.every((s, i) => s.left.every((v, k) => v === golden[i].left[k])), 'left PCM matches golden decoder');
		check(starts.every((s, i) => s.right.every((v, k) => v === golden[i].right[k])), 'right PCM matches golden decoder');
		check(starts.some(s => s.left.some((v, i2) => v !== s.right[i2])), 'channels stay asymmetric/non-zero');
	}
});

tests.push({
	group: 'playout',
	name: 'audio-decodes-no-more-than-250ms-ahead',
	run(check) {
		// All 20 frames are fed before playback, so the buffer must hold the
		// whole in-flight set (input contract: in-flight <= bufferSize).
		const h = createHarness({ video: false, audioBufferSize: 16384 });
		const aTicks = [];
		for (let i = 0; i < 20; i++) aTicks.push(90000 + i * 2160); // 24ms cadence
		const data = Buffer.concat(aTicks.map((t, i) => tsPackets(AUDIO_PID, AUDIO_ID, t, mp2Frame(), i & 15)));
		h.feed(data, 7);
		h.setNow(0); h.tick(); // arms at wallOrigin 0.1
		const wallOrigin = h.player.ptsSync.wallOrigin;
		check(Math.abs(wallOrigin - 0.1) < 1e-12, 'start 100ms after the arming clock');
		const now = 0.11;
		h.setNow(now); h.tick();
		const starts = h.audioStarts;
		check(starts.length > 1 && starts.length < 20, 'only the near window is queued');
		starts.forEach(s => check(s.when <= now + 0.25 + 1e-12, 'scheduled start inside the 250ms window'));
		const remaining = h.player.ptsSync.anchors.audio;
		check(remaining.length > 0, 'frames remain queued for later updates');
		check(expectedWall(remaining[0].tick, aTicks[0]) > now + 0.25, 'the first unscheduled frame lies outside the 250ms window');
		// A later update drains the rest; the last frame may extend beyond 250ms.
		h.setNow(2); h.tick();
		check(h.audioStarts.length === 20, 'all frames eventually scheduled once');
	}
});

tests.push({
	group: 'playout',
	name: 'video-only-clock-is-jsmpeg-now',
	run(check) {
		const h = createHarness({ audio: false });
		const specs = [
			{ kind: 'I', tick: 3000, first: true }, { kind: 'P', tick: 6600 }, { kind: 'P', tick: 10200 }
		];
		h.feed(buildStream(specs), 187);
		h.setNow(0); h.tick();
		check(h.player.currentTime === 0, 'media time zero before start');
		h.setNow(0.1 + 0.02); h.tick();
		check(h.renders.length === 1, 'first frame at Now-derived time');
		check(Math.abs(h.player.currentTime - 0.02) < 1e-12, 'currentTime is the shared media coordinate');
		h.setNow(0.1 + 0.08); h.tick();
		check(h.renders.length === 3, 'all due frames processed in one update');
	}
});

tests.push({
	group: 'playout',
	name: 'audio-only-clock-is-the-audio-context',
	run(check) {
		const h = createHarness({ video: false });
		const aTicks = [45000, 47160, 49320];
		const data = Buffer.concat(aTicks.map((t, i) => tsPackets(AUDIO_PID, AUDIO_ID, t, mp2Frame(), i & 15)));
		h.feed(data, 7);
		// JSMpeg.Now and the AudioContext clock diverge; scheduling must follow the context.
		h.setNow(0);
		h.setAudioClock(0.5);
		h.tick();
		const wallOrigin = h.player.ptsSync.wallOrigin;
		check(Math.abs(wallOrigin - 0.6) < 1e-12, 'wall origin built from AudioContext.currentTime');
		h.setNow(9); h.setAudioClock(0.61); h.tick();
		check(h.audioStarts.length === aTicks.length, 'context clock drives playout despite different JSMpeg.Now');
		check(Math.abs(h.audioStarts[0].when - 0.6) < 1e-12, 'first source at context start instant');
	}
});

tests.push({
	group: 'playout',
	name: 'two-players-sharing-a-context-keep-independent-origins',
	run(check) {
		const { J, scope } = loadProduction();
		let now = 0;
		const sharedCtx = new FakeAudioContext();
		J.Now = () => now;
		scope.window = {
			AudioContext: function () { return sharedCtx; },
			performance: { now: () => now * 1000 }
		};
		scope.performance = { now: () => now * 1000 };

		const renders = [[], []];
		J.Renderer.Canvas2D = class {
			constructor(o) { this.which = o._which; }
			resize() {}
			render(y) { renders[this.which].push({ at: now, sum: y.reduce((a, b) => a + b, 0) }); }
			renderProgress() {} destroy() {}
		};
		class Source {
			constructor() { this.streaming = true; this.established = true; this.progress = 1; }
			connect(d) { this.destination = d; } start() {} destroy() {}
		}
		const makePlayer = (which) => new J.Player('', {
			source: Source, streaming: true, disableWebAssembly: true, disableGl: true,
			audio: true, video: true, pauseWhenHidden: false, decodeFirstFrame: false,
			syncToPTS: true, loop: false, _which: which
		});
		const p0 = makePlayer(0);
		const p1 = makePlayer(1);
		check(p0.audioOut.context === sharedCtx && p1.audioOut.context === sharedCtx, 'both players share one AudioContext');

		const mk = (base) => Buffer.concat([
			tsPackets(VIDEO_PID, VIDEO_ID, base, Buffer.concat([sequenceHeader(), flatIFrame()]), 0),
			tsPackets(AUDIO_PID, AUDIO_ID, base + 1000, mp2Frame(), 0)
		]);
		feedInChunks(p0, mk(10000), 7);
		now = 1;
		sharedCtx.currentTime = 1; // AudioContext clock drives an audio-enabled player
		feedInChunks(p1, mk(400000), 7);
		p0.update(); // p0 arms at 1.1
		check(Math.abs(p0.ptsSync.wallOrigin - 1.1) < 1e-12, 'player 0 armed at its own clock reading');
		check(p1.ptsSync.started === false, 'player 1 not armed by the other player update');
		now = 2;
		sharedCtx.currentTime = 2;
		p1.update(); // p1 arms at 2.1
		check(Math.abs(p1.ptsSync.wallOrigin - 2.1) < 1e-12, 'player 1 keeps its own start instant');
		check(p0.ptsSync.wallOrigin === 1.1, 'player 0 origin untouched when player 1 starts');

		// Audio of both players lands on the shared context at distinct times.
		const p0audio = sharedCtx.starts.find(s => Math.abs(s.when - (1.1 + 1000 / TICKS_PER_SECOND)) < 1e-12);
		const p1audio = sharedCtx.starts.find(s => Math.abs(s.when - (2.1 + 1000 / TICKS_PER_SECOND)) < 1e-12);
		check(!!p0audio && !!p1audio, 'each player schedules against its own origin on the shared clock');
	}
});

tests.push({
	group: 'playout',
	name: 'memory-stays-bounded-by-the-in-flight-window',
	run(check) {
		const h = createHarness({ videoBufferSize: 4096, audioBufferSize: 4096 });
		const N = 40;
		const vSpecs = [{ kind: 'I', tick: 90000, first: true }];
		for (let i = 1; i < N; i++) vSpecs.push({ kind: 'P', tick: 90000 + i * 3600 });
		// Interleave video and audio PES so each track's in-flight window is
		// exercised while playback consumes earlier frames.
		const parts = [];
		for (let i = 0; i < N; i++) {
			parts.push(framePacket(vSpecs[i], i));
			parts.push(tsPackets(AUDIO_PID, AUDIO_ID, 90000 + i * 2160, mp2Frame(), i & 15));
		}
		const data = Buffer.concat(parts);
		let peakAnchors = 0;
		let peakVideoBytes = 0, peakAudioBytes = 0;
		for (let at = 0; at < data.length; at += 53) {
			h.player.source.destination.write(data.subarray(at, Math.min(at + 53, data.length)));
			// Clock sweeps through the whole media window as data arrives.
			h.setNow(0.1 + (at / data.length) * 2.5);
			h.tick();
			const s = h.player.ptsSync;
			peakAnchors = Math.max(peakAnchors, s.anchors.video.length + s.anchors.audio.length);
			peakVideoBytes = Math.max(peakVideoBytes, h.player.video.bits.byteLength);
			peakAudioBytes = Math.max(peakAudioBytes, h.player.audio.bits.byteLength);
		}
		h.setNow(20); h.tick();
		check(peakAnchors < 12, 'anchor count bounded by the small in-flight window, got ' + peakAnchors);
		check(peakVideoBytes <= 4096 && peakAudioBytes <= 4096, 'compressed buffers stay at bufferSize');
		check(h.renders.length === N, 'all 40 pictures eventually presented');
		check(h.audioStarts.length === N, 'all 40 audio frames eventually scheduled');
		check(h.player.ptsSync.anchors.video.length === 0 && h.player.ptsSync.anchors.audio.length === 0, 'anchors fully drained');
		// PCM workspace only: two fixed 1152-sample work arrays plus the queue window.
		check(h.player.audio.left.length === 1152 && h.player.audio.right.length === 1152, 'no per-frame PCM accumulation in the decoder');
	}
});

module.exports = tests;
