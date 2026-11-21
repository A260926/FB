'use strict';
/*
 * test/clipmap.js
 *
 * Three synchronous groups: windows, updates, render.  Each receives
 * {TerrainSource, TerrainClipmap, runViewer} and a check() callback, calls
 * check for every assertion and returns the assertion count.  The render
 * group drives the real production Render through runViewer and compares its
 * frames against an independently written, slow per-sample reference loop.
 */

// Independent ring model: one Map per level, keyed by ring slot, holding the
// absolute grid-index identity.  A mismatch models an eviction (fresh read).
function RingReference(side, levels) {
    this.side = side;
    this.levels = levels;
    this.R = (side - 1) / 2;
    this.m = side + 1;
    this.rings = Array.from({length: levels}, () => new Map());
    this.reads = 0;
    this.perLevel = new Array(levels).fill(0);
}
RingReference.prototype.move = function (cx, cy) {
    const moved = new Array(this.levels).fill(0);
    for (let l = 0; l < this.levels; l++) {
        const s = 1 << l, m = this.m;
        const ox = Math.floor(cx / s) - this.R;
        const oy = Math.floor(cy / s) - this.R;
        const ring = this.rings[l];
        for (let gy = oy; gy < oy + this.side; gy++) {
            for (let gx = ox; gx < ox + this.side; gx++) {
                const sx = ((gx % m) + m) % m;
                const sy = ((gy % m) + m) % m;
                const slot = sy * m + sx;
                const id = gx + ',' + gy;
                if (ring.get(slot) !== id) {
                    ring.set(slot, id);
                    this.reads++;
                    this.perLevel[l]++;
                    moved[l]++;
                }
            }
        }
    }
    return moved;
};

// Independent bilinear / LOD oracle written straight from the specification.
function bilinearOracle(base, l, x, y) {
    const s = 1 << l;
    const gx = Math.floor(x / s);
    const gy = Math.floor(y / s);
    const fx = x / s - gx;
    const fy = y / s - gy;
    const at = (ix, iy) => base(ix * s, iy * s);
    const w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy);
    const w01 = (1 - fx) * fy, w11 = fx * fy;
    const p00 = at(gx, gy), p10 = at(gx + 1, gy);
    const p01 = at(gx, gy + 1), p11 = at(gx + 1, gy + 1);
    return {
        height: p00.height * w00 + p10.height * w10 +
                p01.height * w01 + p11.height * w11,
        color: blendColors([p00.color, p10.color, p01.color, p11.color],
                           [w00, w10, w01, w11])
    };
}
function blendColors(colors, weights) {
    const out = new Array(4);
    for (let b = 0; b < 4; b++) {
        let v = 0;
        for (let i = 0; i < 4; i++) v += ((colors[i] >>> (b * 8)) & 255) * weights[i];
        v = Math.round(v);
        out[b] = v < 0 ? 0 : (v > 255 ? 255 : v);
    }
    return ((out[3] << 24) | (out[2] << 16) | (out[1] << 8) | out[0]) >>> 0;
}
function mixSamples(a, b, w) {
    const channels = new Array(4);
    for (let k = 0; k < 4; k++) {
        channels[k] = a.channels[k] * (1 - w) + b.channels[k] * w;
    }
    return {height: a.height * (1 - w) + b.height * w, channels};
}
function sampleChannels(sample) {
    return [0, 1, 2, 3].map((b) => (sample.color >>> (b * 8)) & 255);
}
function quantizeChannels(channels) {
    let color = 0;
    for (let b = 0; b < 4; b++) {
        let v = Math.round(channels[b]);
        if (v < 0) v = 0;
        else if (v > 255) v = 255;
        color |= v << (b * 8);
    }
    return color >>> 0;
}
// Full LOD oracle for a point around camera center (cx, cy).
function lodOracle(base, R, levels, cx, cy, x, y) {
    const r = Math.max(Math.abs(x - cx), Math.abs(y - cy));
    const baseRadius = R - 2;
    const last = levels - 1;
    if (r > baseRadius * (1 << last)) return null;
    if (r <= baseRadius / 2) {
        const s = bilinearOracle(base, 0, x, y);
        return {height: s.height, color: s.color};
    }
    let l = 0;
    while (l < last && r >= baseRadius * (1 << l)) l++;
    if (l >= last) {
        const s = bilinearOracle(base, last, x, y);
        return {height: s.height, color: s.color};
    }
    const bl = baseRadius * (1 << l);
    const fine = bilinearOracle(base, l, x, y);
    const coarse = bilinearOracle(base, l + 1, x, y);
    const w = 2 * r / bl - 1;
    const mixed = mixSamples(
        {height: fine.height, channels: sampleChannels(fine)},
        {height: coarse.height, channels: sampleChannels(coarse)}, w);
    return {height: mixed.height, color: quantizeChannels(mixed.channels)};
}

// Slow per-sample render oracle: same projection and occlusion as the
// production Render, but each terrain fetch goes explicitly through the
// clipmap view and every pixel is computed independently.
function referenceFrame(view, camera, width, height, background, distance) {
    const buffer = new Uint32Array(width * height).fill(background);
    const hiddeny = new Int32Array(width).fill(height);
    const sinang = Math.sin(camera.angle);
    const cosang = Math.cos(camera.angle);
    let deltaz = 1;
    for (let z = 1; z < distance; z += deltaz) {
        let plx = -cosang * z - sinang * z;
        let ply = sinang * z - cosang * z;
        const prx = cosang * z - sinang * z;
        const pry = -sinang * z - cosang * z;
        const dx = (prx - plx) / width;
        const dy = (pry - ply) / width;
        plx += camera.x;
        ply += camera.y;
        const invz = 1 / z * 240;
        for (let i = 0; i < width; i++) {
            const sampled = view.sample(plx, ply);
            if (sampled !== null) {
                let ytop = ((camera.height - sampled.height) * invz + camera.horizon) | 0;
                const ybottom = hiddeny[i];
                if (ytop < 0) ytop = 0;
                if (ytop <= ybottom) {
                    for (let k = ytop; k < ybottom; k++) buffer[k * width + i] = sampled.color | 0;
                    const heightonscreen = ((camera.height - sampled.height) * invz + camera.horizon) | 0;
                    if (heightonscreen < hiddeny[i]) hiddeny[i] = heightonscreen;
                }
            }
            plx += dx;
            ply += dy;
        }
        deltaz += 0.005;
    }
    return buffer;
}

function smallMap(overrides) {
    const map = {
        width: 8, height: 8, shift: 3,
        altitude: new Uint8Array(64),
        color: new Uint32Array(64).fill(0xff3050a0)
    };
    if (overrides) overrides(map);
    return map;
}
function viewerOptions(map, extra) {
    return Object.assign({
        map, width: 16, height: 12,
        camera: {x: 4, y: 6, height: 1, angle: 0, horizon: 2, distance: 64},
        background: 0xffe09090
    }, extra || {});
}

module.exports = {
    // ---------------------------------------------------------------------
    windows: function (api, check) {
        const {TerrainSource, TerrainClipmap} = api;
        let n = 0;
        const side = 9, levels = 3, R = 4;
        const reads = [];
        const source = new TerrainSource((x, y) => {
            reads.push([x, y]);
            return {height: x * 3 - y * 2, color: 0xff102030};
        });
        const view = new TerrainClipmap(source, {side, levels});
        const ref = new RingReference(side, levels);
        n += check(source.subscriberCount() === 1, 'construction subscribes the view');

        const moves = [[0, 0], [1, 1], [2, 2], [1, 1], [0, 0],
                       [-3, 2], [-4, -5], [-4, -5], [7, -11], [100, 100], [102, 100]];
        for (const [cx, cy] of moves) {
            view.moveTo(cx, cy);
            const perLevel = ref.move(cx, cy);
            const st = view.stats();
            n += check(st.lastReads === perLevel.reduce((a, b) => a + b, 0),
                `move ${cx},${cy} read count matches independent ring`);
            for (let l = 0; l < levels; l++) {
                n += check(st.lastFillsByLevel[l] === perLevel[l],
                    `move ${cx},${cy} level ${l} fills match reference`);
            }
            n += check(st.reads === ref.reads,
                `move ${cx},${cy} cumulative reads match reference`);
            n += check(st.resident === side * side * levels,
                `move ${cx},${cy} keeps exactly side^2*levels resident nodes`);
        }

        // A one-step diagonal move brings 2*side+1 nodes per level and the
        // shared corner is read exactly once.
        const fresh = new TerrainClipmap(
            new TerrainSource((x, y) => ({height: 0, color: 0xff000000})),
            {side, levels});
        fresh.moveTo(0, 0);
        fresh.moveTo(1, 1);
        // Only level 0 shifts for a one-unit move; its incoming L-shape holds
        // side vertical + (side-1) horizontal nodes, the corner read once.
        n += check(fresh.stats().lastReads === 2 * side - 1,
            'diagonal one-step move reads the L-shape minus its corner once');

        // Stepping straight back into the previous window reads nothing.
        fresh.moveTo(0, 0);
        n += check(fresh.stats().lastReads === 0, 'walk back to the previous window is zero reads');

        // A span of exactly side on one axis overwrites the level once.
        const span = new TerrainClipmap(
            new TerrainSource((x, y) => ({height: 0, color: 0xff000000})),
            {side, levels: 1});
        span.moveTo(0, 0);
        span.moveTo(side, 0);
        n += check(span.stats().lastFillsByLevel[0] === side * side,
            'span == side fills the level exactly once');

        // The ring storage itself never grows or gets recopied.
        const slotCount = (side + 1) * (side + 1);
        n += check(fresh.levelsData[0].heights.length === slotCount,
            'ring has fixed (side+1)^2 slots per level');
        const heightsRef = fresh.levelsData[0].heights;
        fresh.moveTo(50, 50);
        n += check(fresh.levelsData[0].heights === heightsRef,
            'moves reuse the same ring array, never reallocate it');

        // Negative coordinates wrap through the ring without stale values.
        const negSource = new TerrainSource((x, y) => ({
            height: x * 1000 + y, color: 0xffaabbcc
        }));
        const neg = new TerrainClipmap(negSource, {side, levels: 2});
        neg.moveTo(-50, -50);
        neg.moveTo(-51, -49);
        neg.moveTo(-53, -52);
        const w = neg.levelWindow(0);
        for (let gy = w.originY; gy < w.originY + side - 1; gy++) {
            for (let gx = w.originX; gx < w.originX + side - 1; gx++) {
                const got = neg.sample(gx + 0.5, gy + 0.5);
                const h = (x, y) => x * 1000 + y;
                const expected = (h(gx, gy) + h(gx + 1, gy) +
                                  h(gx, gy + 1) + h(gx + 1, gy + 1)) / 4;
                n += check(got !== null && Math.abs(got.height - expected) < 1e-9,
                    `negative-coordinate ring node ${gx},${gy} keeps its identity`);
            }
        }

        // Range validation happens before any mutation.
        const guarded = new TerrainClipmap(
            new TerrainSource((x, y) => ({height: 0, color: 0})), {side, levels: 2});
        let threw = false;
        try { guarded.moveTo(2 ** 30 + R, 0); }
        catch (e) { threw = e instanceof RangeError; }
        n += check(threw, 'window crossing 2^30 is refused with RangeError');
        n += check(guarded.hasCamera === false, 'refused moveTo leaves the view untouched');
        try { guarded.sample(0, 0); threw = false; }
        catch (e) { threw = true; }
        n += check(threw, 'sampling before any moveTo throws');

        // Constructor option validation.
        for (const [badSide, badLevels] of [[7, 2], [10, 2], [9, 0], [9, 17]]) {
            let bad = false;
            try { new TerrainClipmap(source, {side: badSide, levels: badLevels}); }
            catch (e) { bad = e instanceof RangeError; }
            n += check(bad, `reject side=${badSide} levels=${badLevels}`);
        }
        return n;
    },

    // ---------------------------------------------------------------------
    updates: function (api, check) {
        const {TerrainSource, TerrainClipmap} = api;
        let n = 0;
        const side = 9, levels = 3;
        const source = new TerrainSource((x, y) => ({height: 0, color: 0xff000000}));
        const viewA = new TerrainClipmap(source, {side, levels});
        const viewB = new TerrainClipmap(source, {side, levels});
        n += check(source.subscriberCount() === 2, 'two views subscribe to one source');
        viewA.moveTo(0, 0);
        viewB.moveTo(1000, 1000);

        // Resident edit refreshes only the level whose grid contains the point.
        source.edit([{x: 1, y: 0, height: 77, color: 0xff112233}]);
        n += check(viewA.stats().lastReads === 1, 'odd point refreshes level 0 only');
        n += check(viewB.stats().lastReads === 0, 'distant view performs no reads for a foreign edit');
        n += check(Math.abs(viewA.sample(1, 0).height - 77) < 1e-12,
            'edited value is sampled immediately');

        // A point on a coarser grid refreshes every level whose grid contains it.
        source.edit([{x: 4, y: 0, height: 5, color: 0xff000000}]);
        n += check(viewA.stats().lastReads === 3,
            'multiple-of-2^l point refreshes all three resident levels');

        // Last entry for a duplicated coordinate wins.
        source.edit([
            {x: 1, y: 1, height: 1, color: 0xff000001},
            {x: 1, y: 1, height: 2, color: 0xff000002},
            {x: 1, y: 1, height: 3, color: 0xff000003}
        ]);
        const dup = viewA.sample(1, 1);
        n += check(dup.height === 3 && dup.color === 0xff000003, 'duplicate coordinate: last entry wins');

        // A non-resident edit costs nothing now but is visible after traveling.
        const cumulative = (v) => {
            const st = v.stats();
            return [st.reads, st.fillsByLevel.join(','), st.resident].join('|');
        };
        const before = cumulative(viewA);
        source.edit([{x: 500, y: 500, height: 9, color: 0xffffffff}]);
        n += check(cumulative(viewA) === before,
            'non-resident edit triggers no read and changes no cumulative counters');
        n += check(viewA.stats().lastReads === 0, 'non-resident edit reports zero lastReads');
        viewA.moveTo(500, 500);
        n += check(Math.abs(viewA.sample(500, 500).height - 9) < 1e-12,
            'sparse edit is visible when its point becomes resident later');
        n += check(viewA.sample(500, 500).color === 0xffffffff, 'edited color survives the revisit');

        // Invalid batches are checked fully and commit nothing.
        const statsBeforeReject = JSON.stringify(viewA.stats());
        const invalid = [
            [{x: 2 ** 31, y: 0, height: 1, color: 0}],
            [{x: 0, y: 0, height: Infinity, color: 0}],
            [{x: 0, y: 0, height: 1, color: 0x100000000}],
            [{x: 0.5, y: 0, height: 1, color: 0}],
            [{x: 0, y: 0, height: 1}]
        ];
        for (const batch of invalid) {
            let bad = false;
            try { source.edit(batch); } catch (e) { bad = e instanceof RangeError; }
            n += check(bad, 'illegal edit batch raises RangeError: ' + JSON.stringify(batch));
        }
        n += check(JSON.stringify(viewA.stats()) === statsBeforeReject,
            'all caches and counters survive the rejected batches');
        n += check(Math.abs(viewA.sample(500, 500).height - 9) < 1e-12,
            'rejected batches leave committed edits intact');

        // An empty batch changes nothing at all.
        source.edit([]);
        n += check(JSON.stringify(viewA.stats()) === statsBeforeReject, 'empty edit batch is a no-op');

        // A read served by the sparse coverage still counts as a source read.
        const covered = new TerrainClipmap(source, {side, levels: 1});
        const coveredReads = covered.stats().reads;
        covered.moveTo(500, 500);
        n += check(covered.stats().reads - coveredReads === side * side,
            'override-covered points are read through the source and counted');

        // sample() never moves the counters.
        const sampleReads = viewA.stats().reads;
        viewA.sample(500.3, 500.7);
        viewA.sample(499, 499);
        n += check(viewA.stats().reads === sampleReads, 'sample changes no statistics');

        // destroy unsubscribes, isolates the dead view and leaves the others.
        viewB.destroy();
        n += check(source.subscriberCount() === 2, 'destroyed view removed from subscribers');
        let deadCalls = 0;
        try { viewB.moveTo(0, 0); } catch (e) { deadCalls++; }
        try { viewB.sample(0, 0); } catch (e) { deadCalls++; }
        try { viewB.stats(); } catch (e) { deadCalls++; }
        n += check(deadCalls === 3, 'every method on a destroyed view throws');
        viewB.destroy();
        source.edit([{x: 500, y: 500, height: 11, color: 0xffffffff}]);
        n += check(Math.abs(viewA.sample(500, 500).height - 11) < 1e-12,
            'the surviving view still receives edits');
        return n;
    },

    // ---------------------------------------------------------------------
    render: function (api, check) {
        const {TerrainSource, TerrainClipmap, runViewer} = api;
        let n = 0;
        const R = 4, side = 9, levels = 4;

        // Analytic nonlinear terrain, evaluated by an independent oracle.
        const base = (x, y) => ({
            height: x * x + y * y,
            color: (0xff000000 | (((x + 16) * 7) & 255) |
                    ((((y + 16) * 11) & 255) << 8) |
                    ((((x - y) & 31) * 8) << 16)) >>> 0
        });
        const source = new TerrainSource(base);
        const view = new TerrainClipmap(source, {side, levels});
        view.moveTo(0, 0);
        const probe = [[0, 0], [0.25, 0.5], [-0.75, 0.25], [0.9, -0.4]];
        // Every transition endpoint: b0/2, b0, b1, b2, b3 and the far limit.
        for (const r of [1, 2, 4, 8, 16]) probe.push([r, 0], [0, -r]);
        // Mid-band points exercise the blend weight on each band.
        for (const r of [3, 6, 12]) probe.push([r, 0]);
        for (const [x, y] of probe) {
            const got = view.sample(x, y);
            const expected = lodOracle(base, R, levels, 0, 0, x, y);
            n += check(got !== null && expected !== null, 'point inside far boundary is sampled: ' + x + ',' + y);
            n += check(Math.abs(got.height - expected.height) < 1e-9,
                `LOD height matches hand calculation at ${x},${y}: ${got.height} vs ${expected.height}`);
            n += check(got.color === expected.color,
                `LOD color matches hand calculation at ${x},${y}: ${got.color.toString(16)} vs ${expected.color.toString(16)}`);
        }
        n += check(view.sample(16.001, 0) === null, 'points beyond b_last return null');
        // A linear field must survive every bilinear/blend path exactly.
        const plane = new TerrainClipmap(
            new TerrainSource((x, y) => ({height: 3 * x - 2 * y + 5, color: 0xff010203})),
            {side, levels});
        plane.moveTo(0, 0);
        for (const [x, y] of probe) {
            const got = plane.sample(x, y);
            n += check(Math.abs(got.height - (3 * x - 2 * y + 5)) < 1e-8,
                'linear field is exact through every LOD transition at ' + x + ',' + y);
        }

        // Real production frames through the HTML Render, headlessly.
        const map = smallMap();
        const options = viewerOptions(map);
        const arrayFrame = runViewer(options).color;
        const clipResult = runViewer(viewerOptions(map, {
            configure: (g) => g.SetTerrainMode('clipmap')
        }));
        let equal = 0;
        for (let i = 0; i < arrayFrame.length; i++) if (arrayFrame[i] === clipResult.color[i]) equal++;
        n += check(equal === arrayFrame.length,
            'flat terrain renders identically in array and clipmap modes');
        const sky = (buf) => buf.filter((c) => c === options.background).length;
        n += check(sky(clipResult.color) > 0, 'clipmap frame keeps background sky pixels');

        // Slow per-sample reference vs the production frame.
        const globals = clipResult.session;
        const prodView = globals.GetTerrainClipmap();
        let oracle = referenceFrame(prodView, clipResult.camera,
            options.width, options.height, options.background, options.camera.distance);
        let frameMismatch = 0;
        for (let i = 0; i < oracle.length; i++) if (oracle[i] !== clipResult.color[i]) frameMismatch++;
        n += check(frameMismatch === 0, 'production frame equals the slow per-sample reference');

        // Same comparison after moving the camera.
        const moved = runViewer(viewerOptions(map, {
            session: globals,
            camera: {x: 5, y: 7, height: 1, angle: 0.6, horizon: 2, distance: 64}
        }));
        oracle = referenceFrame(globals.GetTerrainClipmap(), moved.camera,
            options.width, options.height, options.background, options.camera.distance);
        frameMismatch = 0;
        for (let i = 0; i < oracle.length; i++) if (oracle[i] !== moved.color[i]) frameMismatch++;
        n += check(frameMismatch === 0, 'reference still matches after the camera moves');

        // Raising terrain changes occlusion while the sky remains intact.
        // The ridge is far enough away that its projection stays inside the
        // already-ground region, so no background sky pixel is overwritten.
        const flat = clipResult.color;
        const raised = runViewer(viewerOptions(map, {
            session: globals,
            configure: (g) => {
                const batch = [];
                for (let x = -40; x <= 40; x++)
                    batch.push({x, y: -20, height: 5, color: 0xff202020});
                g.GetTerrainSource().edit(batch);
            }
        }));
        const changed = raised.color.filter((c, i) => c !== flat[i]).length;
        n += check(changed > 0, 'raising the ground changes the occluded frame');
        n += check(sky(raised.color) === sky(flat), 'the sky pixels are preserved by the raise');
        oracle = referenceFrame(globals.GetTerrainClipmap(), raised.camera,
            options.width, options.height, options.background, options.camera.distance);
        frameMismatch = 0;
        for (let i = 0; i < oracle.length; i++) if (oracle[i] !== raised.color[i]) frameMismatch++;
        n += check(frameMismatch === 0, 'reference matches the edited frame');

        // Mode switching tears down the old source and restores array output.
        const oldSource = globals.GetTerrainSource();
        n += check(oldSource.subscriberCount() === 1, 'one live view before switching back');
        const restored = runViewer(viewerOptions(map, {
            session: globals, configure: (g) => g.SetTerrainMode('array')
        }));
        let restoreMismatch = 0;
        for (let i = 0; i < arrayFrame.length; i++)
            if (restored.color[i] !== arrayFrame[i]) restoreMismatch++;
        n += check(restoreMismatch === 0, 'switching back reproduces the array frame');
        n += check(oldSource.subscriberCount() === 0, 'the old source has no subscribers after teardown');
        n += check(globals.GetTerrainSource() === null, 'the old clipmap source is dropped');

        // Map switching: production OnLoadedImages rebuilds source and view.
        const switched = runViewer(viewerOptions(map, {
            session: globals,
            configure: (g) => {
                g.SetTerrainMode('clipmap');
                g.screendata.context = {
                    putImageData() {},
                    createImageData: () => ({data: new Uint8ClampedArray(options.width * options.height * 4)})
                };
                g.screendata.imagedata = {
                    data: new Uint8ClampedArray(options.width * options.height * 4)
                };
                const colorRGBA = new Uint8ClampedArray(map.width * map.height * 4);
                const heightRGBA = new Uint8ClampedArray(map.width * map.height * 4);
                for (let i = 0; i < map.width * map.height; i++) {
                    colorRGBA[i * 4] = 0xa0; colorRGBA[i * 4 + 1] = 0x50;
                    colorRGBA[i * 4 + 2] = 0x30; colorRGBA[i * 4 + 3] = 0xff;
                }
                const mapSourceBefore = g.GetTerrainSource();
                g.OnLoadedImages([colorRGBA, heightRGBA]);
                n += check(g.GetTerrainSource() !== mapSourceBefore,
                    'loading a map builds a fresh terrain source');
                n += check(mapSourceBefore.subscriberCount() === 0,
                    'the previous source is unsubscribed on map switch');
                n += check(g.GetTerrainSource().subscriberCount() === 1,
                    'the new source has exactly one subscribed view');
            }
        }));
        n += check(switched.color.length === arrayFrame.length, 'frame after map switch is rendered');

        // Camera collision uses the same clipmap view.
        const collide = runViewer(viewerOptions(map, {
            collide: true,
            camera: {x: 4, y: 6, height: -50, angle: 0, horizon: 2, distance: 64},
            configure: (g) => g.SetTerrainMode('clipmap')
        }));
        n += check(collide.camera.height >= 10, 'UpdateCamera lifts the camera via the clipmap view');
        return n;
    }
};
