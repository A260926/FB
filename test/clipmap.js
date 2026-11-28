'use strict';

// Synchronous test groups for terrain-clipmap.js and the VoxelSpace.html
// production Render. Each group: (api, check) => number of check() calls.
// api = {TerrainSource, TerrainClipmap, runViewer}. No framework, no main.

function key(x, y) {
    return x + ',' + y;
}

// Independent window reference: grid index sets for a level, computed
// straight from the specification, with no use of the clipmap internals.
function expectedWindow(cx, cy, spacing, R, side) {
    const ox = Math.floor(cx / spacing) - R;
    const oy = Math.floor(cy / spacing) - R;
    const set = new Set();
    for (let j = 0; j < side; j++) {
        for (let i = 0; i < side; i++) {
            set.add(key((ox + i) * spacing, (oy + j) * spacing));
        }
    }
    return set;
}

// A source that logs every effective read for independent bookkeeping.
function makeLoggingSource(fn) {
    const log = [];
    const source = new TerrainSourceShim(fn, log);
    return {source, log};
}

// Local alias resolved inside each group from api.
let TerrainSourceRef = null;

class TerrainSourceShim {
    constructor(fn, log) {
        this._fn = fn;
        this._log = log;
        this._inner = new TerrainSourceRef((x, y) => {
            this._log.push(key(x, y));
            return fn(x, y);
        });
    }
    get inner() {
        return this._inner;
    }
    read(x, y) {
        return this._inner.read(x, y);
    }
    edit(batch) {
        return this._inner.edit(batch);
    }
    subscriberCount() {
        return this._inner.subscriberCount();
    }
}

// Linear analytic terrain with a uniquely decodable height per world point.
function linearHeight(x, y) {
    return 3 * x - 7 * y;
}
function linearColor(x, y) {
    const r = ((x % 256) + 256) % 256;
    const g = ((y % 256) + 256) % 256;
    const b = ((x + y) % 256 + 256) % 256;
    return (0xff000000 | (b << 16) | (g << 8) | r) >>> 0;
}

// ---------------------------------------------------------------------------
// windows: ring-slot set differences, negative-coordinate wrap, sharing,
// destroy, coordinate limits.
// ---------------------------------------------------------------------------

function windows(api, check) {
    TerrainSourceRef = api.TerrainSource;
    const TerrainSource = api.TerrainSource;
    const TerrainClipmap = api.TerrainClipmap;
    let n = 0;
    function ck(cond, msg) { check(cond, msg); n++; }

    const side = 9, R = 4, levels = 2;
    const shim = makeLoggingSource((x, y) => ({
        height: linearHeight(x, y), color: linearColor(x, y)
    }));
    const source = shim.source.inner;
    const view = new TerrainClipmap(source, {side, levels});

    function readsSince(mark) {
        const keys = shim.log.slice(mark);
        return {count: keys.length, set: new Set(keys)};
    }

    // Initial moveTo: every cell of every level is filled once.
    let mark = shim.log.length;
    view.moveTo(0, 0);
    let st = view.stats();
    ck(st.reads === side * side * levels, 'initial fills every cell once');
    ck(st.resident === side * side * levels, 'resident counts all cells');
    ck(st.lastReads === side * side * levels, 'lastReads matches initial fill');
    let logged = readsSince(mark);
    let exp0 = expectedWindow(0, 0, 1, R, side);
    let exp1 = expectedWindow(0, 0, 2, R, side);
    ck(logged.count === exp0.size + exp1.size, 'logged reads match both windows');
    // Every logged point belongs to one of the expected level windows.
    let allExpected = true;
    for (const p of logged.set) {
        if (!exp0.has(p) && !exp1.has(p)) { allExpected = false; break; }
    }
    ck(allExpected, 'all initial reads lie inside declared windows');

    // One-cell x move: only one level-0 strip; level 1 window is unchanged.
    mark = shim.log.length;
    view.moveTo(1, 0);
    st = view.stats();
    ck(st.lastReads === side, 'single-axis move reads one strip only');
    ck(st.lastFillsByLevel[0] === side && st.lastFillsByLevel[1] === 0,
        'coarse level does not move for a sub-spacing shift');
    logged = readsSince(mark);
    const stripWin = expectedWindow(1, 0, 1, R, side);
    let stripOnly = true;
    for (const p of logged.set) {
        if (!stripWin.has(p)) { stripOnly = false; break; }
    }
    ck(stripOnly && logged.set.size === side, 'strip points are exactly the new column');

    // Walk back into the previous window: zero source reads.
    mark = shim.log.length;
    const readsBeforeBack = st.reads;
    view.moveTo(0, 0);
    st = view.stats();
    ck(st.lastReads === 0, 'walking back to a resident window reads nothing');
    ck(st.reads === readsBeforeBack, 'cumulative reads unchanged on walk-back');
    ck(shim.log.length === mark, 'no source calls during walk-back');

    // A fresh view (no window history) gives the exact new/old set difference
    // on a diagonal move: one new column and one new row whose single shared
    // corner is read only once.
    const exact = new TerrainClipmap(source, {side, levels});
    exact.moveTo(0, 0);
    const exactMark = shim.log.length;
    exact.moveTo(1, 1);
    st = exact.stats();
    const l0Win = expectedWindow(1, 1, 1, R, side);
    const l0Old = expectedWindow(0, 0, 1, R, side);
    let l0New = 0;
    for (const p of l0Win) if (!l0Old.has(p)) l0New++;
    const l1Win = expectedWindow(1, 1, 2, R, side);
    const l1Old = expectedWindow(0, 0, 2, R, side);
    let l1New = 0;
    for (const p of l1Win) if (!l1Old.has(p)) l1New++;
    ck(st.lastReads === l0New + l1New, 'fresh diagonal move reads exactly the set difference');
    // L0 difference for +1,+1: a column plus a row sharing one corner.
    ck(l0New === side + (side - 1), 'column plus row minus the single shared corner');
    // A sub-spacing camera shift leaves the coarse window untouched.
    ck(l1New === 0, 'coarse level window is unchanged for a one-cell shift');
    let exactLogged = shim.log.slice(exactMark);
    // The unique intersecting corner (grid point 5,5) is odd, so it cannot
    // belong to the level-1 grid; it must appear in the log exactly once.
    const cornerKey = key(5, 5);
    let cornerHits = 0;
    exactLogged.forEach((p) => { if (p === cornerKey) cornerHits++; });
    ck(cornerHits === 1, 'the shared strip corner is read a single time');
    ck(exactLogged.length === l0New + l1New &&
       exactLogged.length === new Set(exactLogged).size,
        'every difference point is read once and none twice');
    let noUnexpected = true;
    for (const p of new Set(exactLogged)) {
        if (!l0Win.has(p) && !l1Win.has(p)) { noUnexpected = false; break; }
    }
    ck(noUnexpected, 'diagonal reads stay inside both new windows');

    // On the history view a larger diagonal move may only read a subset of
    // the set difference (ghost slots already hold some difference points);
    // it must never read a point outside that difference.
    const l0Win2 = expectedWindow(2, 1, 1, R, side);
    const l0Old2 = expectedWindow(0, 0, 1, R, side);
    const l1Win2 = expectedWindow(2, 1, 2, R, side);
    const l1Old2 = expectedWindow(0, 0, 2, R, side);
    let diffSize = 0;
    for (const p of l0Win2) if (!l0Old2.has(p)) diffSize++;
    for (const p of l1Win2) if (!l1Old2.has(p)) diffSize++;
    mark = shim.log.length;
    view.moveTo(2, 1);
    logged = readsSince(mark);
    let subsetOnly = true;
    for (const p of logged.set) {
        if (!l0Win2.has(p) && !l1Win2.has(p)) { subsetOnly = false; break; }
    }
    ck(subsetOnly && logged.count <= diffSize,
        'cached ghost slots only suppress difference reads, never add others');

    // Teleport to a fully disjoint window: every cell refilled once.
    mark = shim.log.length;
    view.moveTo(40, 40);
    st = view.stats();
    ck(st.lastReads === side * side * levels, 'teleport refills every level once');
    ck(st.lastFillsByLevel[0] === side * side &&
       st.lastFillsByLevel[1] === side * side, 'per-level teleport fills equal cells');

    // Negative coordinates and ring-slot identity: values must follow the
    // absolute grid point, not the reused slot.
    view.moveTo(-12, -12);
    const atInteger = view.sample(-12, -12);
    ck(atInteger.height === linearHeight(-12, -12),
        'negative integer point keeps its identity through ring wrap');
    const atHalf = view.sample(-11.5, -12);
    const expectedHalf = (linearHeight(-11, -12) + linearHeight(-12, -12)) / 2;
    ck(Math.abs(atHalf.height - expectedHalf) < 1e-12,
        'negative-coordinate bilinear interpolation is correct');

    // A span of exactly `side` on a single-level view replaces every resident
    // slot exactly once, with no point read twice.
    const spanView = new TerrainClipmap(source, {side, levels: 1});
    spanView.moveTo(0, 0);
    const spanMark = shim.log.length;
    spanView.moveTo(side, 0);
    st = spanView.stats();
    ck(st.lastFillsByLevel[0] === side * side,
        'span == side fills each resident slot exactly once');
    const spanLogged = shim.log.slice(spanMark);
    ck(spanLogged.length === side * side &&
       spanLogged.length === new Set(spanLogged).size,
        'span == side performs side*side distinct reads');

    // Long deterministic walk across many ring sizes (positive and negative).
    // The terrain height is affine, so every bilinear/LOD reconstruction must
    // reproduce 3x-7y exactly even after slots have been reused many times:
    // a reused slot must never expose a value from an old grid point.
    const walk = new TerrainClipmap(source, {side, levels: 3});
    let seed = 1234567;
    const rnd = function () {
        seed = (seed * 1103515245 + 12345) & 0x7fffffff;
        return seed / 0x7fffffff;
    };
    let wx = 0, wy = 0;
    walk.moveTo(wx, wy);
    const walkMargin = R - 2;
    const walkB = walkMargin * (1 << (3 - 1));
    let walkProbes = 0, walkOK = true;
    for (let step = 0; step < 60; step++) {
        wx += Math.round((rnd() - 0.5) * 2 * (side + 2));
        wy += Math.round((rnd() - 0.5) * 2 * (side + 2));
        walk.moveTo(wx, wy);
        for (let gx = -walkB + 1; gx <= walkB - 1; gx += 1) {
            for (let gy = -walkB + 1; gy <= walkB - 1; gy += 2) {
                const px = wx + gx + 0.25;
                const py = wy + gy - 0.75;
                const got = walk.sample(px, py);
                walkProbes++;
                // An empty/stale slot would carry height 0 and alpha 0; every
                // genuine grid cell of this source has finite height, alpha 255.
                if (got === null ||
                    Math.abs(got.height - linearHeight(px, py)) > 1e-9 ||
                    ((got.color >>> 24) & 255) !== 255) {
                    walkOK = false;
                }
            }
        }
    }
    ck(walkOK && walkProbes > 1000,
        'ring reuse never returns a stale value over a long signed walk');

    // Constructor validation.
    let threw = false;
    for (const bad of [
        {side: 8, levels: 1}, {side: 10, levels: 1},
        {side: 9, levels: 0}, {side: 9, levels: 17}
    ]) {
        try { new TerrainClipmap(source, bad); } catch (e) {
            if (e instanceof RangeError) { threw = true; continue; }
        }
        threw = false;
        break;
    }
    ck(threw, 'bad side/levels combinations throw RangeError');

    // Sampling before any moveTo is an error.
    const fresh = new TerrainClipmap(source, {side, levels: 1});
    let sampleEarly = false;
    try { fresh.sample(0, 0); } catch (e) { sampleEarly = true; }
    ck(sampleEarly, 'sample before moveTo throws');

    // Coordinate limit: legal edge, then an out-of-range move that must not
    // mutate the view.
    const lim = 1 << 30;
    fresh.moveTo(lim - R, 0);
    const edgeSample = fresh.sample(lim - R, 0);
    ck(edgeSample.height === linearHeight(lim - R, 0), 'positive limit edge is reachable');
    const readsAtEdge = fresh.stats().reads;
    let rangeError = false;
    try { fresh.moveTo(lim - R + 1, 0); } catch (e) {
        rangeError = e instanceof RangeError;
    }
    ck(rangeError, 'window past +2^30 throws RangeError');
    ck(fresh.stats().reads === readsAtEdge, 'failed move leaves reads unchanged');
    ck(fresh.sample(lim - R, 0).height === edgeSample.height,
        'failed move leaves the resident window intact');
    fresh.moveTo(-lim + R, 0);
    rangeError = false;
    try { fresh.moveTo(-lim + R - 1, 0); } catch (e) {
        rangeError = e instanceof RangeError;
    }
    ck(rangeError, 'window past -2^30 throws RangeError');
    let nanError = false;
    try { fresh.moveTo(NaN, 0); } catch (e) { nanError = e instanceof RangeError; }
    ck(nanError, 'non-finite camera throws RangeError');

    // Two cameras share one source; each cache is independent.
    const shimA = makeLoggingSource((x, y) => ({
        height: linearHeight(x, y), color: linearColor(x, y)
    }));
    const shared = shimA.source.inner;
    const viewA = new TerrainClipmap(shared, {side, levels: 1});
    const viewB = new TerrainClipmap(shared, {side, levels: 2});
    viewA.moveTo(0, 0);
    viewB.moveTo(0, 0);
    ck(shared.subscriberCount() === 2, 'two live subscribers reported');
    const bReads = viewB.stats().reads;
    viewA.moveTo(5, 5);
    ck(viewB.stats().reads === bReads, 'moving one view does not touch the other cache');
    viewB.destroy();
    ck(shared.subscriberCount() === 1, 'destroy removes one subscription');
    let deadThrows = false;
    try { viewB.sample(0, 0); } catch (e) { deadThrows = true; }
    ck(deadThrows, 'destroyed view rejects sample');
    deadThrows = false;
    try { viewB.moveTo(1, 1); } catch (e) { deadThrows = true; }
    ck(deadThrows, 'destroyed view rejects moveTo');
    deadThrows = false;
    try { viewB.stats(); } catch (e) { deadThrows = true; }
    ck(deadThrows, 'destroyed view rejects stats');
    ck(viewA.sample(5, 5) !== null &&
       viewA.sample(5, 5).height === linearHeight(5, 5),
        'surviving view still works after sibling destroy');
    viewA.destroy();
    ck(shared.subscriberCount() === 0, 'last destroy reaches zero subscribers');

    return n;
}

// ---------------------------------------------------------------------------
// updates: edit validation/coverage, subscription refresh and hand-computed
// LOD blending.
// ---------------------------------------------------------------------------

function updates(api, check) {
    TerrainSourceRef = api.TerrainSource;
    const TerrainSource = api.TerrainSource;
    const TerrainClipmap = api.TerrainClipmap;
    let n = 0;
    function ck(cond, msg) { check(cond, msg); n++; }

    const side = 9;
    const source = new TerrainSource((x, y) => ({
        height: 0, color: 0xff102030
    }));
    const view = new TerrainClipmap(source, {side, levels: 1});
    view.moveTo(0, 0);

    // Empty batch is a no-op: no notification, even the "last" counters are
    // left at the values of the previous operation.
    const reads0 = view.stats().reads;
    const lastReads0 = view.stats().lastReads;
    const lastFills0 = view.stats().lastFillsByLevel.slice();
    source.edit([]);
    ck(view.stats().reads === reads0 &&
       view.stats().lastReads === lastReads0 &&
       view.stats().lastFillsByLevel.join() === lastFills0.join(),
        'empty edit changes nothing, including the last-event counters');
    ck(view.sample(0, 0).height === 0, 'empty edit leaves values intact');

    // Illegal batches: RangeError, source and every view unchanged.
    const goodColor = 0xffaabbcc;
    const badBatches = [
        null,
        [{x: 0, y: 0, height: NaN, color: goodColor}],
        [{x: 0, y: 0, height: 1, color: 0x100000000}],
        [{x: 0, y: 0, height: 1, color: -1}],
        [{x: 1.5, y: 0, height: 1, color: goodColor}],
        [{x: (1 << 30) + 1, y: 0, height: 1, color: goodColor}],
        [{x: 0, y: 0, height: '7', color: goodColor}],
        [{x: 0, y: 0, height: 1}]
    ];
    for (const bad of badBatches) {
        let err = false;
        try { source.edit(bad); } catch (e) { err = e instanceof RangeError; }
        ck(err, 'illegal batch throws RangeError: ' + JSON.stringify(bad));
        ck(view.stats().reads === reads0, 'failed edit leaves cache reads untouched');
    }
    ck(view.sample(0, 0).height === 0, 'failed edits leave resident values intact');
    ck(source.read(0, 0).height === 0, 'failed edits leave the source intact');

    // Duplicate coordinate: last item wins, one notification point.
    source.edit([
        {x: 1, y: 1, height: 5, color: 0xff111111},
        {x: 1, y: 1, height: 9, color: 0xff222222}
    ]);
    ck(source.read(1, 1).height === 9 && source.read(1, 1).color === 0xff222222,
        'duplicate coordinate keeps the last item');
    ck(view.stats().lastReads === 1, 'duplicate collapses to one resident refresh');
    ck(view.sample(1, 1).height === 9, 'resident duplicate edit is visible');

    // Non-resident edit: no view reads until the point enters a window.
    const readsBeforeFar = view.stats().reads;
    source.edit([{x: 100, y: 100, height: 42, color: 0xff424242}]);
    ck(view.stats().lastReads === 0, 'non-resident edit triggers no source read');
    ck(view.stats().reads === readsBeforeFar, 'non-resident edit does not touch counters');
    view.moveTo(100, 100);
    ck(view.sample(100, 100).height === 42, 'override is read when the point becomes resident');
    ck(view.stats().reads > readsBeforeFar, 'entering an override point counts a read');
    // Leave and come back: the sparse cover is still there.
    view.moveTo(0, 0);
    view.moveTo(100, 100);
    ck(view.sample(100, 100).height === 42, 'edit survives leaving and re-entering a window');

    // Multi-level notification: a point refreshes every level whose grid it
    // lies on, only while resident there.
    const multi = new TerrainSource((x, y) => ({height: 0, color: 0xff000000}));
    const mv = new TerrainClipmap(multi, {side, levels: 3});
    mv.moveTo(0, 0);
    multi.edit([
        {x: 4, y: 4, height: 1, color: 0xff010101},   // divisible by 1,2,4 -> 3 levels
        {x: 2, y: 0, height: 2, color: 0xff020202},   // divisible by 1,2   -> 2 levels
        {x: 3, y: 0, height: 3, color: 0xff030303},   // divisible by 1     -> 1 level
        {x: 100, y: 0, height: 4, color: 0xff040404}  // not resident       -> 0
    ]);
    ck(mv.stats().lastReads === 3 + 2 + 1, 'resident edits refresh exactly the matching levels');
    // With the camera centered on each edited point, r = 0 selects level 0,
    // whose exact grid cell must carry the edited height.
    const editedPoints = [[4, 4, 1], [2, 0, 2], [3, 0, 3]];
    for (const [px, py, h] of editedPoints) {
        mv.moveTo(px, py);
        const got = mv.sample(px, py);
        ck(got.height === h, 'level-0 cell at edited point (' + px + ',' + py + ') holds height ' + h);
    }
    // Isolated check: an edit landing only on the level-0 grid (odd x) must
    // not leak into coarser reconstruction. At r = 3, sampling uses the
    // L1/L2 blend, neither of whose grids contain x = 3.
    const leakSrc = new TerrainSource((x, y) => ({height: 0, color: 0xff000000}));
    const leakView = new TerrainClipmap(leakSrc, {side, levels: 3});
    leakView.moveTo(0, 0);
    leakSrc.edit([{x: 3, y: 0, height: 9, color: 0xff090909}]);
    ck(leakView.stats().lastReads === 1, 'odd-coordinate edit refreshes level 0 only');
    ck(Math.abs(leakView.sample(3, 0).height - 0) < 1e-12,
        'a level-0-only edit does not alter coarser level reconstruction');
    mv.moveTo(0, 0);

    // Two views at different positions: only the resident one refreshes.
    const v1 = new TerrainClipmap(multi, {side, levels: 1});
    const v2 = new TerrainClipmap(multi, {side, levels: 1});
    v1.moveTo(0, 0);
    v2.moveTo(200, 200);
    multi.edit([{x: 0, y: 0, height: 7, color: 0xff070707}]);
    ck(v1.stats().lastReads === 1, 'resident view receives the edit');
    ck(v2.stats().lastReads === 0, 'distant view gets no refresh for a non-resident point');
    v2.destroy();
    let notifyAfterDestroy = true;
    try { multi.edit([{x: 0, y: 0, height: 8, color: 0xff080808}]); }
    catch (e) { notifyAfterDestroy = false; }
    ck(notifyAfterDestroy, 'editing after a destroy does not throw');
    ck(v1.sample(0, 0).height === 8, 'live view still receives notifications');
    ck(multi.subscriberCount() === 2, 'destroyed view is unsubscribed, the other two remain');

    // ----- Hand-computed LOD blending -------------------------------------
    // h(x,y) = x + 2y; color r = x^2, g = y^2 (small coords, no wrap),
    // b = 0, a = 255. side 9 -> R=4, margin=2, b = [2, 4, 8].
    const lod = new TerrainSource((x, y) => {
        const r = x * x, g = y * y;
        return {height: x + 2 * y,
                color: ((255 << 24) | (0 << 16) | (g << 8) | r) >>> 0};
    });
    const lv = new TerrainClipmap(lod, {side, levels: 3});
    lv.moveTo(0, 0);
    function redOf(s) { return s.color & 255; }

    let s = lv.sample(0.5, 0);                 // r = 0.5 <= b0/2 -> level 0
    ck(Math.abs(s.height - 0.5) < 1e-12, 'inner zone uses level 0 height');
    ck(redOf(s) === 1, 'inner zone level 0 color rounded (0.5 -> 1)');

    s = lv.sample(1, 0);                        // r = b0/2 boundary, still level 0
    ck(Math.abs(s.height - 1) < 1e-12 && redOf(s) === 1,
        'r = b0/2 stays on the fine level');

    s = lv.sample(1.5, 0);                      // blend l0/l1, w = 0.5
    ck(Math.abs(s.height - 1.5) < 1e-12, 'linear slope matches through the blend');
    // L0 red: 0.5*1 + 0.5*4 = 2.5; L1 red: 0.75*4 = 3; blend = 2.75 -> 3.
    ck(redOf(s) === 3, 'nonlinear red channel blended in float then rounded');

    s = lv.sample(2, 0);                        // r = b0 -> pure coarse level
    ck(Math.abs(s.height - 2) < 1e-12 && redOf(s) === 4,
        'r = b_l is assigned to the coarser level');

    s = lv.sample(3, 0);                        // blend l1/l2, w = 0.5
    ck(Math.abs(s.height - 3) < 1e-12, 'second blend zone height');
    // L1 red: 0.5*4 + 0.5*16 = 10; L2 red: 0.75*16 = 12; blend = 11.
    ck(redOf(s) === 11, 'second blend zone color hand check');

    s = lv.sample(4, 0);                        // r = b1 -> pure level 2
    ck(Math.abs(s.height - 4) < 1e-12 && redOf(s) === 16,
        'second boundary assigns the coarser level');

    s = lv.sample(8, 0);                        // r = b_last -> last level direct
    ck(Math.abs(s.height - 8) < 1e-12 && redOf(s) === 64,
        'far boundary uses the last level directly');

    ck(lv.sample(8.0001, 0) === null, 'past b_last returns null');
    ck(lv.sample(0, 9) === null, 'Chebyshev radius applies on the y axis too');
    ck(lv.sample(100, -100) === null, 'far corner returns null');

    // Negative-coordinate bilinear, all four neighbours weighted equally.
    s = lv.sample(-0.5, -0.5);
    ck(Math.abs(s.height - (-1.5)) < 1e-12,
        'negative bilinear height: (-3-2-1+0)/4 = -1.5');

    // Chebyshev (not Euclidean) radius picks the zone.
    s = lv.sample(1.5, 1.5);
    ck(Math.abs(s.height - 4.5) < 1e-12, 'Chebyshev radius drives the blend zone');

    // Continuity at r = b0: the blended endpoint equals the standalone coarse
    // bilinear value (level 1 at integer point (2,0)).
    const coarseOnly = lv.sample(2, 0);
    ck(coarseOnly.height === 2 && redOf(coarseOnly) === 4,
        'level boundary is continuous at w = 1');

    // Editing a shared grid point changes the analytic blend result.
    lod.edit([{x: 2, y: 0, height: 10, color: 0xff000000}]);
    s = lv.sample(1.5, 0);
    // L0: 0.5*1 + 0.5*10 = 5.5; L1: 0.75*10 = 7.5; w = 0.5 -> 6.5.
    ck(Math.abs(s.height - 6.5) < 1e-12, 'edits propagate through blended sampling');

    // Sampling never reads the source and never moves the counters, even
    // across the inner, blend and out-of-range zones.
    const readsAfterEdit = lv.stats().reads;
    lv.sample(0, 0); lv.sample(1.5, 0); lv.sample(4, 0);
    lv.sample(8, 0); lv.sample(100, 100); lv.sample(-0.5, -0.5);
    ck(lv.stats().reads === readsAfterEdit, 'sample does not increment reads');

    // Float interpolation with rounding and channel clamping.
    const q = new TerrainSource((x, y) => ({height: 0, color: 0xff000000}));
    const qv = new TerrainClipmap(q, {side, levels: 1});
    qv.moveTo(0, 0);
    q.edit([
        {x: 0, y: 0, height: 0, color: 0xff000001},
        {x: 1, y: 0, height: 0, color: 0xff000003}
    ]);
    const qs = qv.sample(0.5, 0);
    ck((qs.color & 255) === 2, 'interpolated channel rounds 2.0 to 2');
    ck(((qs.color >>> 24) & 255) === 255, 'alpha channel interpolated and retained');

    return n;
}

// ---------------------------------------------------------------------------
// render: the real production Render in VoxelSpace.html driven through
// runViewer, compared against a slow per-sample reference, plus occlusion,
// out-of-range sky, collision and source lifecycle on mode/map switches.
// ---------------------------------------------------------------------------

function render(api, check) {
    const TerrainSource = api.TerrainSource;
    const TerrainClipmap = api.TerrainClipmap;
    const runViewer = api.runViewer;
    let n = 0;
    function ck(cond, msg) { check(cond, msg); n++; }

    const W = 24, H = 18;
    const bg = 0xffe09090;
    const groundColor = 0xff3050a0;
    function makeMap(altFill, colorFill) {
        return {
            width: 8, height: 8, shift: 3,
            altitude: new Uint8Array(64).fill(altFill),
            color: new Uint32Array(64).fill(colorFill)
        };
    }
    const baseCamera = {x: 4, y: 6, height: 2, angle: 0, horizon: 2, distance: 64};
    const options = (extra) => Object.assign({
        width: W, height: H, map: makeMap(0, groundColor),
        camera: Object.assign({}, baseCamera), background: bg
    }, extra || {});

    // Original array mode still renders exactly as before.
    const arrayFrame = runViewer(options()).color;
    const arrayGround = arrayFrame.filter((x) => x === groundColor).length;
    const arraySky = arrayFrame.filter((x) => x === bg).length;
    ck(arrayGround > 0 && arraySky > 0 &&
       arrayGround + arraySky === arrayFrame.length, 'array mode still renders ground and sky only');

    // Clipmap mode over a flat uniform map must match the array frame exactly
    // while the whole view frustum is resident.
    let clipContext = null;
    const clipOptions = options();
    const clipFrame = runViewer(Object.assign(clipOptions, {
        configure(c) {
            c.setTerrainMode('clipmap');
            clipContext = c;
        }
    })).color;
    ck(clipContext.terrainMode === 'clipmap', 'production mode switch enters clipmap');
    ck(clipContext.terrain.source.subscriberCount() === 1, 'clipmap view subscribes once');
    let identical = true;
    for (let i = 0; i < clipFrame.length; i++) {
        if (clipFrame[i] !== arrayFrame[i]) { identical = false; break; }
    }
    ck(identical, 'flat clipmap frame matches the array frame pixel for pixel');

    // Slow per-sample reference: the same projection math, but an independent
    // column loop that asks the production view for every sample and applies
    // the DrawVerticalLine/hiddeny rules itself.
    function slowReference(c, fineStep) {
        const width = c.screendata.canvas.width;
        const height = c.screendata.canvas.height;
        c.DrawBackground();
        const buf32 = c.screendata.buf32;
        const hiddeny = new Int32Array(width).fill(height);
        const cam = c.camera;
        const sinang = Math.sin(cam.angle);
        const cosang = Math.cos(cam.angle);
        for (let z = 1; z < cam.distance; z += fineStep) {
            let plx = -cosang * z - sinang * z + cam.x;
            let ply = sinang * z - cosang * z + cam.y;
            const prx = cosang * z - sinang * z + cam.x;
            const pry = -sinang * z - cosang * z + cam.y;
            const dx = (prx - plx) / width;
            const dy = (pry - ply) / width;
            const invz = 1 / z * 240;
            for (let i = 0; i < width; i++) {
                const smp = c.terrain.view.sample(plx, ply);
                if (smp !== null) {
                    const ytop = ((cam.height - smp.height) * invz + cam.horizon) | 0;
                    let yt = ytop < 0 ? 0 : ytop;
                    const yb = hiddeny[i];
                    if (yt <= yb) {
                        for (let k = yt; k < yb; k++) buf32[k * width + i] = smp.color;
                        if (ytop < hiddeny[i]) hiddeny[i] = ytop;
                    }
                }
                plx += dx;
                ply += dy;
            }
        }
        return Uint32Array.from(buf32);
    }

    let exactRef = null;
    runViewer(options({
        session: clipContext,
        configure(c) {
            c.setTerrainMode('clipmap');
            exactRef = slowReference(c, 1); // production z schedule starts at step 1
        }
    }));
    let matchExact = 0;
    for (let i = 0; i < clipFrame.length; i++) {
        if (exactRef[i] === clipFrame[i]) matchExact++;
    }
    ck(matchExact === clipFrame.length, 'independent per-sample loop reproduces the production frame');

    let fineRef = null;
    runViewer(options({
        session: clipContext,
        configure(c) {
            c.setTerrainMode('clipmap');
            fineRef = slowReference(c, 0.25);
        }
    }));
    let matchFine = 0;
    for (let i = 0; i < clipFrame.length; i++) {
        if (fineRef[i] === clipFrame[i]) matchFine++;
    }
    ck(matchFine / clipFrame.length >= 0.95, 'fine-stepped slow reference stays within 5% of the frame');
    const fineSky = fineRef.filter((x) => x === bg).length;
    ck(Math.abs(fineSky - arraySky) <= W, 'slow reference preserves the sky region');

    // Raise a partial ridge through a sparse edit: occlusion changes, sky is
    // retained on the columns the ridge does not cover.
    const ridgeColor = 0xff00ff00;
    const raised = runViewer(options({
        session: clipContext,
        configure(c) {
            c.setTerrainMode('clipmap');
            const batch = [];
            for (let x = 3; x < 6; x++) {
                batch.push({x: x, y: 3, height: 40, color: ridgeColor});
            }
            c.terrain.source.edit(batch);
        }
    })).color;
    let changed = 0;
    for (let i = 0; i < raised.length; i++) {
        if (raised[i] !== clipFrame[i]) changed++;
    }
    ck(changed > 0, 'raising terrain changes the rendered frame');
    let skyKept = true;
    for (let i = 0; i < raised.length; i++) {
        // Raising ground can only cover more sky, never create sky pixels.
        if (raised[i] === bg && clipFrame[i] !== bg) { skyKept = false; break; }
    }
    ck(skyKept, 'no new sky appears after terrain is raised');
    ck(raised.filter((x) => x === bg).length > 0, 'a sky region still remains on uncovered columns');
    // The ground is red-dominant (R 0xa0 > G 0x50); the green ridge flips that
    // relation wherever its bilinear-blended color reaches the frame.
    let greenDominant = 0;
    for (const x of raised) {
        const r = x & 255, g = (x >>> 8) & 255;
        if (g > r) greenDominant++;
    }
    ck(greenDominant > 0, 'the edited ridge color reaches the frame');
    // The production view itself still returns the exact edited grid value.
    const ridgeSample = clipContext.terrain.view.sample(4, 3);
    ck(ridgeSample.height === 40 && (ridgeSample.color & 255) === 0,
        'the exact edited point is returned by the shared view');
    ck(raised.filter((x) => x === bg).length <= clipFrame.filter((x) => x === bg).length,
        'raising terrain never increases the sky count');

    // A tiny clipmap: samples beyond b_last are skipped, leaving more sky,
    // while nearby ground still draws. A downward-looking camera keeps the
    // near (r <= 2) resident band on a small screen.
    const tinyCamera = {x: 4, y: 6, height: 2, angle: 0, horizon: -470, distance: 64};
    const tinyArray = runViewer(options({camera: Object.assign({}, tinyCamera)})).color;
    const tinyArraySky = tinyArray.filter((x) => x === bg).length;
    const tiny = runViewer(options({
        camera: Object.assign({}, tinyCamera),
        configure(c) {
            c.terrain.side = 9;
            c.terrain.levels = 1;
            c.setTerrainMode('clipmap');
        }
    })).color;
    const tinySky = tiny.filter((x) => x === bg).length;
    ck(tinySky > tinyArraySky, 'out-of-resident samples are not drawn (more sky than array mode)');
    ck(tiny.filter((x) => x !== bg).length > 0, 'nearby resident ground still draws');

    // Collision height goes through the same view.
    const collideMap = makeMap(0, groundColor);
    const collide = runViewer({
        width: W, height: H, map: collideMap,
        camera: {x: 4, y: 4, height: 5, angle: 0, horizon: 2, distance: 20},
        background: bg, collide: true,
        configure(c) {
            c.setTerrainMode('clipmap');
            c.terrain.source.edit([{x: 4, y: 4, height: 30, color: ridgeColor}]);
        }
    });
    ck(collide.camera.height === 40, 'UpdateCamera lifts the camera onto edited ground');

    // Mode switching never reuses a source.
    const lifecycle = runViewer(options({
        configure(c) {
            c.setTerrainMode('clipmap');
            c._oldSource = c.terrain.source;
            c.setTerrainMode('array');
        }
    }));
    const c = lifecycle.session;
    ck(c.terrainMode === 'array', 'switching back restores array mode');
    ck(c.terrain.source === null, 'array mode drops the clipmap source');
    ck(c._oldSource.subscriberCount() === 0, 'abandoned source has no subscribers');
    runViewer(options({
        session: c,
        configure(c2) {
            c2.setTerrainMode('clipmap');
            ck(c2.terrain.source !== c2._oldSource, 're-entering clipmap builds a new source');
            ck(c2._oldSource.subscriberCount() === 0, 'the previous source stays unsubscribed');
        }
    }));

    // Map switch through the production OnLoadedImages path: old view/source
    // are destroyed before the new ones are built.
    const mapSwitch = runViewer(options({
        configure(c2) {
            c2.setTerrainMode('clipmap');
            c2._beforeSwitch = c2.terrain.source;
            const size = c2.map.width * c2.map.height;
            const dc = new Uint8Array(size * 4).fill(200);
            const dh = new Uint8Array(size * 4).fill(7);
            c2.screendata.context = {putImageData() {}};
            c2.screendata.imagedata = {data: {set() {}}};
            c2.OnLoadedImages([dc, dh]);
            ck(c2.terrain.source !== c2._beforeSwitch, 'map switch builds a new source');
            ck(c2._beforeSwitch.subscriberCount() === 0, 'map switch unsubscribes the old source');
            ck(c2.terrain.source.subscriberCount() === 1, 'new source has exactly one view');
            const sampled = c2.terrain.view.sample(c2.camera.x, c2.camera.y);
            ck(sampled.height === 7, 'new map altitude is sampled through the new view');
        }
    }));
    ck(mapSwitch !== null, 'map switch frame produced');

    return n;
}

module.exports = {windows, updates, render};
