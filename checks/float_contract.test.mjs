// Contract tests for the IEEE floating-point pixel path (SampleFormat=3).
//
// The TIFF bytes come from checks/tiff_fixture.mjs, an independent classic-TIFF
// writer that never calls UTIF. The expected RGBA below is computed from the
// same linear samples with an independent implementation of the documented
// rules (clamp, sRGB transfer, alpha association), so UTIF is never used as its
// own reference. Files, linear samples and expected bytes are kept separate.
import {test} from 'node:test';
import assert from 'node:assert/strict';
import UTIF from '../UTIF.js';
import {floatTiff} from './tiff_fixture.mjs';

// ---- independent reference (mirrors the documented contract, not UTIF) ----
function refNorm(x) {
	if(Number.isNaN(x) || x === Number.NEGATIVE_INFINITY) return 0;
	if(x === Number.POSITIVE_INFINITY) return 1;
	return x < 0 ? 0 : (x > 1 ? 1 : x);
}
function refSRGB(x) {
	return x < 0.0031308 ? 12.92*x : 1.055*Math.pow(x, 1/2.4) - 0.055;
}
function refByte(x) { return Math.round(x*255); }
// values: linear samples, sample count 1/3/4; alphaMode: 1 associated, 2 non-associated
function expectedRGBA(values, samples, alphaMode) {
	const pixels = values.length / samples;
	const out = new Uint8Array(pixels*4);
	for(let p=0; p<pixels; p++) {
		const s = p*samples, q = p*4;
		if(samples === 1) {
			const g = refByte(refNorm(values[s]));
			out[q] = out[q+1] = out[q+2] = g; out[q+3] = 255;
			continue;
		}
		const hasAlpha = samples === 4;
		const premul = hasAlpha && alphaMode === 1;
		let a = 1;
		if(hasAlpha) a = refNorm(values[s+3]);
		if(premul && a === 0) { out[q] = out[q+1] = out[q+2] = out[q+3] = 0; continue; }
		for(let c=0; c<3; c++) {
			let lin = values[s+c];
			if(premul) lin = lin / a;                 // unassociate first, transfer function after
			out[q+c] = refByte(refSRGB(refNorm(lin)));
		}
		out[q+3] = hasAlpha ? refByte(a) : 255;      // alpha is linear, never gamma-corrected
	}
	return out;
}
function assertCloseRGBA(actual, expected, label) {
	assert.equal(actual.length, expected.length, label + ' length');
	for(let i=0; i<actual.length; i++) {
		assert.ok(Math.abs(actual[i] - expected[i]) <= 1,
			label + ' byte ' + i + ': got ' + actual[i] + ' want ' + expected[i]);
	}
}
function convert(buffer) {
	const ifds = UTIF.decode(buffer);
	UTIF.decodeImage(buffer, ifds[0], ifds);
	return ifds[0];
}
// Deterministic linear samples, deliberately including out-of-gamut values.
function makeValues(count, seed = 1) {
	const v = [];
	for(let i=0; i<count; i++) {
		const z = ((i*37 + seed*11) % 17) / 16 - 0.25;   // spans roughly -0.25 .. 0.8125
		v.push(Number(z.toFixed(4)));
	}
	return v;
}
const COMPRESSIONS = [1, 8, 32773];   // uncompressed, Deflate, PackBits

test('float_chunky', () => {
	// RGB 32-bit, odd 3x5 image, multiple strips (RowsPerStrip=2 -> 2,2,1 rows),
	// both byte orders and all three supported compressions.
	for(const little of [true, false]) {
		for(const compression of COMPRESSIONS) {
			const width = 3, height = 5, samples = 3;
			const values = makeValues(width*height*samples, little ? 1 : 2);
			const buffer = floatTiff({compression, width, height, rows: 2, samples,
				bits: 32, little, planar: false, values});
			const ifd = convert(buffer);
			assertCloseRGBA(UTIF.toRGBA8(ifd), expectedRGBA(values, samples, 2),
				'chunky rgb little=' + little + ' cmpr=' + compression);
			// decompressed bytes keep the file's byte order; negatives must not trigger a swap
			const dv = new DataView(ifd.data.buffer, ifd.data.byteOffset, ifd.data.byteLength);
			assert.equal(dv.getFloat32(0, little), values[0]);
		}
	}

	// Chunky 16x16 tiles on a 17x13 image: right column and bottom rows are the
	// physical tile extending past the image boundary.
	{
		const width = 17, height = 13, samples = 3;
		const values = makeValues(width*height*samples, 3);
		const buffer = floatTiff({width, height, samples, bits: 32, little: true,
			tileWidth: 16, tileHeight: 16, values});
		const ifd = convert(buffer);
		assertCloseRGBA(UTIF.toRGBA8(ifd), expectedRGBA(values, samples, 2), 'chunky tile edges');
	}

	// RGBA 32-bit, associated (1) and non-associated (2) alpha, multi-strip.
	for(const alpha of [1, 2]) {
		const width = 4, height = 3, samples = 4;
		const values = makeValues(width*height*samples, 4 + alpha);
		// force one fully transparent and one half-transparent pixel per mode
		values[3] = 0; values[7] = 0.5;
		const buffer = floatTiff({width, height, rows: 2, samples, bits: 32,
			little: true, alpha, values});
		const ifd = convert(buffer);
		assertCloseRGBA(UTIF.toRGBA8(ifd), expectedRGBA(values, samples, alpha),
			'chunky rgba alpha=' + alpha);
	}

	// 32-bit and 64-bit grayscale: linear intensity, clamp then round, no gamma.
	for(const bits of [32, 64]) {
		const width = 5, height = 1, samples = 1;
		const values = [-0.5, 0, 0.25, 0.5, 1];
		const buffer = floatTiff({width, height, rows: 1, samples, bits,
			little: true, values});
		const ifd = convert(buffer);
		assertCloseRGBA(UTIF.toRGBA8(ifd), expectedRGBA(values, samples, 2),
			'chunky gray bits=' + bits);
	}
});

test('float_planar', () => {
	// PlanarConfiguration=2 strips on an odd 5x3 image, II and MM, every
	// compression. Planes must be interleaved per whole pixel sample.
	for(const little of [true, false]) {
		for(const compression of COMPRESSIONS) {
			const width = 5, height = 3, samples = 3;
			const values = makeValues(width*height*samples, 7 + (little ? 0 : 1));
			const buffer = floatTiff({compression, width, height, rows: 2, samples,
				bits: 32, little, planar: true, values});
			const ifd = convert(buffer);
			assertCloseRGBA(UTIF.toRGBA8(ifd), expectedRGBA(values, samples, 2),
				'planar strip little=' + little + ' cmpr=' + compression);
		}
	}

	// Planar 16x16 tiles on 19x17: 2x2 tiles, residual right column band and
	// bottom row band sit inside physical tiles with padding.
	for(const little of [true, false]) {
		const width = 19, height = 17, samples = 3;
		const values = makeValues(width*height*samples, 9 + (little ? 0 : 1));
		const buffer = floatTiff({width, height, samples, bits: 32, little,
			planar: true, tileWidth: 16, tileHeight: 16, values});
		const ifd = convert(buffer);
		assertCloseRGBA(UTIF.toRGBA8(ifd), expectedRGBA(values, samples, 2),
			'planar tile edges little=' + little);
	}
});

test('float_reuse', () => {
	const width = 3, height = 4, samples = 3;
	const values = makeValues(width*height*samples, 5);
	const buffer = floatTiff({compression: 8, width, height, rows: 2, samples,
		bits: 32, little: true, values});
	const ifd = convert(buffer);
	const expected = expectedRGBA(values, samples, 2);

	// Raw byte snapshot proves conversion is read-only, including NaN encodings.
	const snapshot = Uint8Array.from(ifd.data);
	const first = UTIF.toRGBA8(ifd);
	assertCloseRGBA(first, expected, 'reuse first');
	const second = UTIF.toRGBA8(ifd);
	const third = UTIF.toRGBA8(ifd);
	// Repeated conversions of the same IFD must be byte-for-byte identical.
	assert.deepEqual(second, first);
	assert.deepEqual(third, first);
	assert.deepEqual(Uint8Array.from(ifd.data), snapshot);

	// Two IFDs converted in interleaved order keep independent, stable results.
	const valuesB = makeValues(width*height*samples, 8);
	const ifdB = convert(floatTiff({compression: 32773, width, height, rows: 2,
		samples, bits: 32, little: false, values: valuesB}));
	const expectedB = expectedRGBA(valuesB, samples, 2);
	const a1 = UTIF.toRGBA8(ifd), b1 = UTIF.toRGBA8(ifdB);
	const b2 = UTIF.toRGBA8(ifdB), a2 = UTIF.toRGBA8(ifd);
	assert.deepEqual(a2, a1);
	assert.deepEqual(b2, b1);
	assertCloseRGBA(b1, expectedB, 'interleaved second IFD');

	// Reading raw linear floats in between conversions changes nothing.
	const dv = new DataView(ifd.data.buffer, ifd.data.byteOffset, ifd.data.byteLength);
	const rawBefore = dv.getFloat32(4, true);
	const mid = UTIF.toRGBA8(ifd);
	const rawAfter = dv.getFloat32(4, true);
	assert.equal(rawAfter, rawBefore);
	assert.deepEqual(mid, first);
	assert.deepEqual(Uint8Array.from(ifd.data), snapshot);

	// A data view with a non-zero byteOffset: only the view range is interpreted,
	// and the sentinel bytes outside it must stay untouched.
	const inner = ifd.data;
	const big = new Uint8Array(inner.length + 8);
	big.fill(0xA5);
	big.set(inner, 4);
	const guarded = new Uint8Array(big.buffer, 4, inner.length);
	assert.ok(guarded.byteOffset > 0);
	ifd.data = guarded;
	const fromView = UTIF.toRGBA8(ifd);
	assertCloseRGBA(fromView, expected, 'non-zero byteOffset view');
	assert.equal(big[0], 0xA5); assert.equal(big[1], 0xA5);
	assert.equal(big[2], 0xA5); assert.equal(big[3], 0xA5);
	assert.equal(big[big.length-1], 0xA5); assert.equal(big[big.length-2], 0xA5);
	// the guarded view itself is still the original bytes
	assert.deepEqual(Uint8Array.from(guarded), snapshot);
});

test('float_alpha', () => {
	const samples = 4;
	// Non-associated alpha 0 keeps the underlying color; associated alpha 0 is
	// transparent black. Alpha itself is clamped and rounded linearly.
	const unassoc = [0.2, 0.4, 0.6, 0,  1, 1, 1, 0.5];
	let ifd = convert(floatTiff({width: 2, height: 1, rows: 1, samples, bits: 32,
		little: true, alpha: 2, values: unassoc}));
	assertCloseRGBA(UTIF.toRGBA8(ifd), expectedRGBA(unassoc, samples, 2), 'non-associated');

	const assoc = [0.25, 0.25, 0.25, 0.5,  0.2, 0.4, 0.6, 0];
	ifd = convert(floatTiff({width: 2, height: 1, rows: 1, samples, bits: 32,
		little: true, alpha: 1, values: assoc}));
	const rgba = UTIF.toRGBA8(ifd);
	assertCloseRGBA(rgba, expectedRGBA(assoc, samples, 1), 'associated');
	// associated a=0 pixel is transparent black
	assert.deepEqual([...rgba.slice(4)], [0, 0, 0, 0]);
	// premultiplied 0.25 @ a=0.5 unassociated to 0.5 before the transfer function
	assert.equal(rgba[3], 128);
});

test('float_special', () => {
	// NaN and -Infinity -> 0, +Infinity -> 1; ordinary negatives clamp to 0,
	// values above 1 clamp to 1; no exposure or auto-range normalization.
	const values = [Number.NaN, Number.NEGATIVE_INFINITY, Number.POSITIVE_INFINITY,
		-2, 0.5, 3];
	const ifd = convert(floatTiff({width: 2, height: 1, rows: 1, samples: 3,
		bits: 32, little: true, values}));
	const snapshot = Uint8Array.from(ifd.data);   // NaN bit pattern must survive
	const rgba = UTIF.toRGBA8(ifd);
	assertCloseRGBA(rgba, expectedRGBA(values, 3, 2), 'special values');
	assert.deepEqual(Uint8Array.from(ifd.data), snapshot);
	assert.deepEqual([...rgba], [0, 0, 255, 255, 0, 188, 255, 255]);
});
