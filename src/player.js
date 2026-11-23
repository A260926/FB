JSMpeg.Player = (function(){ "use strict";

// PTS driven playback uses one shared, monotonically unwrapped 33-bit 90kHz
// time domain for all tracks. The raw counter wraps at 2^33; adjacent ticks of
// the same track differ by less than half a wrap, so the nearest cycle is always
// unambiguous.
var PTS_TICKS_PER_SECOND = 90000;
var PTS_WRAP = 0x200000000; // 2^33
var PTS_HALF_WRAP = PTS_WRAP / 2; // 2^32
var PTS_START_DELAY = 0.1; // seconds between arming and the media zero point
var PTS_AUDIO_AHEAD = 0.25; // decode/queue audio at most this far ahead

// Place a raw 33-bit tick on the cycle nearest to an already unwrapped point
// in the common time domain.
var nearestWrapCycle = function(tick, reference) {
	var delta = tick - (reference % PTS_WRAP);
	if (delta > PTS_HALF_WRAP) {
		delta -= PTS_WRAP;
	}
	else if (delta < -PTS_HALF_WRAP) {
		delta += PTS_WRAP;
	}
	return reference + delta;
};

var Player = function(url, options) {
	this.options = options || {};

	if (options.source) {
		this.source = new options.source(url, options);
		options.streaming = !!this.source.streaming;
	}
	else if (url.match(/^wss?:\/\//)) {
		this.source = new JSMpeg.Source.WebSocket(url, options);
		options.streaming = true;
	}
	else if (options.progressive !== false) {
		this.source = new JSMpeg.Source.AjaxProgressive(url, options);
		options.streaming = false;
	}
	else {
		this.source = new JSMpeg.Source.Ajax(url, options);
		options.streaming = false;
	}

	// Exact PTS playout only exists for the pure JavaScript streaming chain.
	// Any other configuration keeps the original arrival-order behavior.
	this.ptsMode = options.syncToPTS === true
		&& options.streaming === true
		&& options.disableWebAssembly === true;
	if (this.ptsMode) {
		// The first picture must appear at its PTS, not while being written.
		options.decodeFirstFrame = false;
		this.ptsSync = this.createPtsSync();
	}

	this.maxAudioLag = options.maxAudioLag || 0.25;
	this.loop = options.loop !== false;
	this.autoplay = !!options.autoplay || options.streaming;

	this.demuxer = new JSMpeg.Demuxer.TS(options);
	this.source.connect(this.demuxer);
	if (this.ptsMode) {
		this.demuxer.onFrame = this.ptsOnFrame.bind(this);
	}

	if (!options.disableWebAssembly && JSMpeg.WASMModule.IsSupported()) {
		this.wasmModule = JSMpeg.WASMModule.GetModule();
		options.wasmModule = this.wasmModule;
	}

	if (options.video !== false) {
		this.video = options.wasmModule
			? new JSMpeg.Decoder.MPEG1VideoWASM(options)
			: new JSMpeg.Decoder.MPEG1Video(options);

		this.renderer = !options.disableGl && JSMpeg.Renderer.WebGL.IsSupported()
			? new JSMpeg.Renderer.WebGL(options)
			: new JSMpeg.Renderer.Canvas2D(options);
		
		this.demuxer.connect(JSMpeg.Demuxer.TS.STREAM.VIDEO_1, this.video);
		this.video.connect(this.renderer);
	}

	if (options.audio !== false && JSMpeg.AudioOutput.WebAudio.IsSupported()) {
		this.audio = options.wasmModule
			? new JSMpeg.Decoder.MP2AudioWASM(options)
			: new JSMpeg.Decoder.MP2Audio(options);
		this.audioOut = new JSMpeg.AudioOutput.WebAudio(options);
		this.demuxer.connect(JSMpeg.Demuxer.TS.STREAM.AUDIO_1, this.audio);
		this.audio.connect(this.audioOut);
	}

	Object.defineProperty(this, 'currentTime', {
		get: this.getCurrentTime,
		set: this.setCurrentTime
	});
	Object.defineProperty(this, 'volume', {
		get: this.getVolume,
		set: this.setVolume
	});

	this.paused = true;
	this.unpauseOnShow = false;
	if (options.pauseWhenHidden !== false) {
		document.addEventListener('visibilitychange', this.showHide.bind(this));
	}

	// If we have WebAssembly support, wait until the module is compiled before
	// loading the source. Otherwise the decoders won't know what to do with 
	// the source data.
	if (this.wasmModule) {
		if (this.wasmModule.ready) {
			this.startLoading();
		}
		else if (JSMpeg.WASM_BINARY_INLINED) {
			var wasm = JSMpeg.Base64ToArrayBuffer(JSMpeg.WASM_BINARY_INLINED);
			this.wasmModule.loadFromBuffer(wasm, this.startLoading.bind(this));
		}
		else {
			this.wasmModule.loadFromFile('jsmpeg.wasm',  this.startLoading.bind(this));
		}
	}
	else {
		this.startLoading();

	}
};

// ---------------------------------------------------------------------------
// syncToPTS: exact media-time playout for the pure JavaScript streaming chain
// ---------------------------------------------------------------------------

// One anchor per track maps a frame's presentation time to the bytes of its
// PES in that track's BitBuffer. Anchors are the only per-frame state; their
// count is bounded by the unconsumed PES plus one consumed predecessor per
// track. No timestamps, PCM or pictures are cached beyond the decoders' own
// working buffers.
Player.prototype.createPtsSync = function() {
	return {
		// Per-track raw-tick unwrap state; all tracks share one 90kHz domain.
		lastTick: {video: null, audio: null},
		// Per-track anchors, kept ordered by increasing presentation tick.
		anchors: {video: [], audio: []},
		// Bytes still held in each track's BitBuffer (compressed, unplayed).
		inflight: {video: 0, audio: 0},
		// Media zero point: unwrapped tick and the wall clock it maps to.
		originTick: null,
		wallOrigin: null,
		started: false
	};
};

// Which logical track a PES stream id belongs to.
Player.prototype.ptsTrackFor = function(streamId) {
	if (streamId === JSMpeg.Demuxer.TS.STREAM.VIDEO_1) {
		return 'video';
	}
	if (streamId === JSMpeg.Demuxer.TS.STREAM.AUDIO_1) {
		return 'audio';
	}
	return null;
};

// Demuxer hook: one call per completed PES. The payload has already been
// written into the track decoder's BitBuffer, so anchors describe bytes that
// physically exist there.
Player.prototype.ptsOnFrame = function(streamId, rawTick, pts, byteLength) {
	var track = this.ptsTrackFor(streamId);
	if (!track) {
		return;
	}
	var sync = this.ptsSync;
	var tick = this.ptsUnwrap(track, rawTick);
	var anchors = sync.anchors[track];
	var anchor = {tick: tick, bytes: byteLength};
	anchors.push(anchor);
	sync.inflight[track] += byteLength;
};

// Unwrap a raw 33-bit tick into the shared, monotonic time domain. The first
// tick of a track establishes itself on the cycle nearest to any time already
// known from the other track (the common time domain); later ticks pick the
// wrap cycle nearest to the same track's previous tick. The original offset
// between the tracks is preserved either way.
Player.prototype.ptsUnwrap = function(track, rawTick) {
	var sync = this.ptsSync;
	var previous = sync.lastTick[track];
	if (previous === null) {
		var otherTrack = track === 'video' ? 'audio' : 'video';
		var reference = sync.lastTick[otherTrack];
		var unwrapped = reference === null
			? rawTick
			: nearestWrapCycle(rawTick, reference);
		sync.lastTick[track] = unwrapped;
		return unwrapped;
	}
	var next = nearestWrapCycle(rawTick, previous);
	sync.lastTick[track] = next;
	return next;
};

// Every enabled track has at least one complete frame available.
Player.prototype.ptsReadyToStart = function() {
	if (this.video && this.ptsSync.anchors.video.length === 0) {
		return false;
	}
	if (this.audio && this.ptsSync.anchors.audio.length === 0) {
		return false;
	}
	return true;
};

Player.prototype.ptsEarliestTick = function() {
	var tick = null;
	if (this.video) {
		tick = this.ptsSync.anchors.video[0].tick;
	}
	if (this.audio) {
		var audioTick = this.ptsSync.anchors.audio[0].tick;
		if (tick === null || audioTick < tick) {
			tick = audioTick;
		}
	}
	return tick;
};

Player.prototype.ptsClockNow = function() {
	// With audio, the AudioContext clock is the reference; otherwise the
	// player's generic wall clock. Each Player instance keeps its own origin.
	return this.audio
		? this.audioOut.context.currentTime
		: JSMpeg.Now();
};

Player.prototype.ptsMediaTime = function() {
	var sync = this.ptsSync;
	if (!sync.started) {
		return 0;
	}
	var elapsed = this.ptsClockNow() - sync.wallOrigin;
	// The shared media coordinate starts at zero; before the armed start
	// instant it reports zero rather than a negative value.
	return elapsed > 0 ? elapsed : 0;
};

// Arm playback once all enabled tracks hold a complete frame: the earliest
// presentation time becomes the media zero, scheduled 100ms on the clock.
Player.prototype.ptsArm = function() {
	var sync = this.ptsSync;
	var originTick = this.ptsEarliestTick();
	sync.originTick = originTick;
	sync.wallOrigin = this.ptsClockNow() + PTS_START_DELAY;
	sync.started = true;
};

// Convert an unwrapped presentation tick to a clock time.
Player.prototype.ptsToClock = function(tick) {
	return this.ptsSync.wallOrigin +
		(tick - this.ptsSync.originTick) / PTS_TICKS_PER_SECOND;
};

Player.prototype.updateForPTS = function() {
	var sync = this.ptsSync;

	if (!sync.started) {
		if (!this.ptsReadyToStart()) {
			// Missing input: nothing may be decoded and the origin must not be
			// re-bound to a new wall-clock reading.
			return;
		}
		this.ptsArm();
	}

	var now = this.ptsClockNow();

	if (this.video) {
		this.ptsPumpVideo(now);
	}
	if (this.audio) {
		this.ptsPumpAudio(now);
	}
};

// Render every complete picture whose presentation time has arrived. Pictures
// are never rendered early; gaps keep the previous picture on screen.
Player.prototype.ptsPumpVideo = function(now) {
	var anchors = this.ptsSync.anchors.video;
	while (anchors.length > 0) {
		var anchor = anchors[0];
		var due = this.ptsToClock(anchor.tick);
		if (due > now) {
			break;
		}
		// One decode() consumes exactly one PES/frame in the input contract.
		// If the compressed bytes are not actually decodable yet, leave the
		// anchor and decode position untouched and wait for a later update.
		if (!this.video.decode()) {
			break;
		}
		anchor.consumed = true;
		this.ptsSync.inflight.video -= anchor.bytes;
		anchors.shift();
	}
};

// Decode and schedule audio frames at most PTS_AUDIO_AHEAD ahead of their
// start; the final frame of a run may extend past that boundary. Gaps remain
// silent because no source node is created for them.
Player.prototype.ptsPumpAudio = function(now) {
	var anchors = this.ptsSync.anchors.audio;
	while (anchors.length > 0) {
		var anchor = anchors[0];
		var due = this.ptsToClock(anchor.tick);
		if (due - now > PTS_AUDIO_AHEAD) {
			break;
		}
		// Decode one frame; MP2's decode() hands PCM to its destination, which
		// routes it to the timed playAt() entry via a one-shot output hook.
		var decoded = this.ptsDecodeAudioAt(due);
		if (!decoded) {
			// Frame bytes not fully present yet; do not advance anything.
			break;
		}
		anchor.consumed = true;
		this.ptsSync.inflight.audio -= anchor.bytes;
		anchors.shift();
	}
};

// Decode a single MP2 frame and route its PCM through playAt() at the frame's
// exact clock time, while leaving the decoder's normal play() path untouched.
Player.prototype.ptsDecodeAudioAt = function(when) {
	var audioOut = this.audioOut;
	audioOut.timedWhen = when;
	var ok = this.audio.decode();
	// A frame either completed and consumed the hook, or nothing was decoded.
	audioOut.timedWhen = null;
	return ok;
};

Player.prototype.startLoading = function() {
	this.source.start();
	if (this.autoplay) {
		this.play();
	}
};

Player.prototype.showHide = function(ev) {
	if (document.visibilityState === 'hidden') {
		this.unpauseOnShow = this.wantsToPlay;
		this.pause();
	}
	else if (this.unpauseOnShow) {
		this.play();
	}
};

Player.prototype.play = function(ev) {
	if (this.animationId) {
		return;
	}

	this.animationId = requestAnimationFrame(this.update.bind(this));
	this.wantsToPlay = true;
	this.paused = false;
};

Player.prototype.pause = function(ev) {
	if (this.paused) {
		return;
	}

	cancelAnimationFrame(this.animationId);
	this.animationId = null;
	this.wantsToPlay = false;
	this.isPlaying = false;
	this.paused = true;

	if (this.audio && this.audio.canPlay) {
		// Seek to the currentTime again - audio may already be enqueued a bit
		// further, so we have to rewind it.
		this.audioOut.stop();
		this.seek(this.currentTime);
	}

	if (this.options.onPause) {
		this.options.onPause(this);
	}
};

Player.prototype.getVolume = function() {
	return this.audioOut ? this.audioOut.volume : 0;
};

Player.prototype.setVolume = function(volume) {
	if (this.audioOut) {
		this.audioOut.volume = volume;
	}
};

Player.prototype.stop = function(ev) {
	this.pause();
	this.seek(0);
	if (this.video && this.options.decodeFirstFrame !== false) {
		this.video.decode();
	}
};

Player.prototype.destroy = function() {
	this.pause();
	this.source.destroy();
	this.video && this.video.destroy();
	this.renderer && this.renderer.destroy();
	this.audio && this.audio.destroy();
	this.audioOut && this.audioOut.destroy();
};

Player.prototype.seek = function(time) {
	var startOffset = this.audio && this.audio.canPlay
		? this.audio.startTime
		: this.video.startTime;

	if (this.video) {
		this.video.seek(time + startOffset);
	}
	if (this.audio) {
		this.audio.seek(time + startOffset);
	}

	this.startTime = JSMpeg.Now() - time;
};

Player.prototype.getCurrentTime = function() {
	if (this.ptsMode) {
		return this.ptsMediaTime();
	}
	return this.audio && this.audio.canPlay
		? this.audio.currentTime - this.audio.startTime
		: this.video.currentTime - this.video.startTime;
};

Player.prototype.setCurrentTime = function(time) {
	this.seek(time);
};

Player.prototype.update = function() {
	this.animationId = requestAnimationFrame(this.update.bind(this));

	if (!this.source.established) {
		if (this.renderer) {
			this.renderer.renderProgress(this.source.progress);
		}
		return;
	}

	if (!this.isPlaying) {
		this.isPlaying = true;
		this.startTime = JSMpeg.Now() - this.currentTime;

		if (this.options.onPlay) {
			this.options.onPlay(this);
		}
	}

	if (this.ptsMode) {
		this.updateForPTS();
	}
	else if (this.options.streaming) {
		this.updateForStreaming();
	}
	else {
		this.updateForStaticFile();
	}
};

Player.prototype.updateForStreaming = function() {
	// When streaming, immediately decode everything we have buffered up until
	// now to minimize playback latency.

	if (this.video) {
		this.video.decode();
	}

	if (this.audio) {
		var decoded = false;
		do {
			// If there's a lot of audio enqueued already, disable output and
			// catch up with the encoding.
			if (this.audioOut.enqueuedTime > this.maxAudioLag) {
				this.audioOut.resetEnqueuedTime();
				this.audioOut.enabled = false;
			}
			decoded = this.audio.decode();		
		} while (decoded);
		this.audioOut.enabled = true;
	}
};

Player.prototype.nextFrame = function() {
	if (this.source.established && this.video) {
		return this.video.decode();
	}
	return false;
};

Player.prototype.updateForStaticFile = function() {
	var notEnoughData = false,
		headroom = 0;

	// If we have an audio track, we always try to sync the video to the audio.
	// Gaps and discontinuities are far more percetable in audio than in video.

	if (this.audio && this.audio.canPlay) {
		// Do we have to decode and enqueue some more audio data?
		while (
			!notEnoughData && 
			this.audio.decodedTime - this.audio.currentTime < 0.25
		) {
			notEnoughData = !this.audio.decode();
		}

		// Sync video to audio
		if (this.video && this.video.currentTime < this.audio.currentTime) {
			notEnoughData = !this.video.decode();
		}

		headroom = this.demuxer.currentTime - this.audio.currentTime;
	}


	else if (this.video) {
		// Video only - sync it to player's wallclock
		var targetTime = (JSMpeg.Now() - this.startTime) + this.video.startTime,
			lateTime = targetTime - this.video.currentTime,
			frameTime = 1/this.video.frameRate;

		if (this.video && lateTime > 0) {
			// If the video is too far behind (>2 frames), simply reset the
			// target time to the next frame instead of trying to catch up.
			if (lateTime > frameTime * 2) {
				this.startTime += lateTime;
			}

			notEnoughData = !this.video.decode();
		}

		headroom = this.demuxer.currentTime - targetTime;
	}

	// Notify the source of the playhead headroom, so it can decide whether to
	// continue loading further data.
	this.source.resume(headroom);

	// If we failed to decode and the source is complete, it means we reached
	// the end of our data. We may want to loop.
	if (notEnoughData && this.source.completed) {
		if (this.loop) {
			this.seek(0);
		}
		else {
			this.pause();
			if (this.options.onEnded) {
				this.options.onEnded(this);
			}
		}
	}

	// If there's not enough data and the source is not completed, we have
	// just stalled.
	else if (notEnoughData && this.options.onStalled) {
		this.options.onStalled(this);
	}
};

return Player;

})();

