import test from 'node:test';
import assert from 'node:assert/strict';

import Delaunator from '../index.js';

// ---------------------------------------------------------------------------
// geometric predicates defined independently of the library implementation
// ---------------------------------------------------------------------------

// standard signed area (positive for counterclockwise points in math convention)
function orient(ax, ay, bx, by, cx, cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

// standard incircle determinant; positive when P is strictly inside the
// counterclockwise circumcircle of A, B, C
function incircle(ax, ay, bx, by, cx, cy, px, py) {
    const dx = ax - px;
    const dy = ay - py;
    const ex = bx - px;
    const ey = by - py;
    const fx = cx - px;
    const fy = cy - py;

    const ap = dx * dx + dy * dy;
    const bp = ex * ex + ey * ey;
    const cp = fx * fx + fy * fy;

    return dx * (ey * cp - bp * fy) -
           dy * (ex * cp - bp * fx) +
           ap * (ex * fy - ey * fx);
}

function triangleArea(points, tri, t) {
    const [a, b, c] = [tri[t], tri[t + 1], tri[t + 2]];
    return Math.abs(orient(points[a][0], points[a][1], points[b][0], points[b][1], points[c][0], points[c][1]));
}

function edgeKey(u, v) {
    return u < v ? `${u}:${v}` : `${v}:${u}`;
}

function triKey(tri, t) {
    return [tri[t], tri[t + 1], tri[t + 2]].sort((a, b) => a - b).join(':');
}

function strictPointInTriangle(points, p, a, b, c) {
    const o1 = orient(points[p][0], points[p][1], points[a][0], points[a][1], points[b][0], points[b][1]);
    const o2 = orient(points[p][0], points[p][1], points[b][0], points[b][1], points[c][0], points[c][1]);
    const o3 = orient(points[p][0], points[p][1], points[c][0], points[c][1], points[a][0], points[a][1]);
    return o1 > 0 && o2 > 0 && o3 > 0 || o1 < 0 && o2 < 0 && o3 < 0;
}

function properCross(points, a, b, c, d) {
    return orient(...points[a], ...points[b], ...points[c]) * orient(...points[a], ...points[b], ...points[d]) < 0 &&
           orient(...points[c], ...points[d], ...points[a]) * orient(...points[c], ...points[d], ...points[b]) < 0;
}

function triangleEdges(tri, t) {
    return [[tri[t], tri[t + 1]], [tri[t + 1], tri[t + 2]], [tri[t + 2], tri[t]]];
}

// walk the star of `index` exactly the way the library documents its halfedges
function starRing(d, index) {
    const tri = d.triangles;
    const half = d.halfedges;
    let edge = -1;
    for (let i = 0; i < tri.length; i++) {
        if (tri[i] === index) { edge = i; break; }
    }
    assert.notEqual(edge, -1);
    const ring = [];
    let e = edge;
    do {
        const t = e - e % 3;
        ring.push(tri[t + (e + 1) % 3]);
        e = half[t + (e + 2) % 3];
    } while (e !== edge);
    return ring;
}

// independent full validation of a patched triangulation
function validateMesh(d, points, label) {
    const tri = d.triangles;
    const half = d.halfedges;

    assert.equal(tri.length, half.length, `${label}: triangles/halfedges length mismatch`);
    assert.equal(tri.length % 3, 0, `${label}: triangle slots must be triples`);

    // half-edge reciprocity, boundary count and opposite winding
    let boundaryEdges = 0;
    for (let i = 0; i < half.length; i++) {
        if (half[i] === -1) {
            boundaryEdges++;
        } else {
            assert.equal(half[half[i]], i, `${label}: half-edge reciprocity at ${i}`);
            const a0 = i - i % 3;
            const b0 = half[i] - half[i] % 3;
            // the two directed half-edges must traverse the shared edge in reverse
            assert.equal(tri[b0 + (half[i] + 1) % 3], tri[i], `${label}: twin origin`);
            assert.equal(tri[half[i]], tri[a0 + (i + 1) % 3], `${label}: twin destination`);
        }
    }
    assert.equal(boundaryEdges, d.hull.length, `${label}: boundary half-edges match hull`);

    // uniform nonzero winding, no degenerate or duplicated triangles
    let winding = 0;
    const seenTris = new Set();
    for (let t = 0; t < tri.length; t += 3) {
        const [a, b, c] = [tri[t], tri[t + 1], tri[t + 2]];
        assert.notEqual(a, b, `${label}: repeated vertex`);
        assert.notEqual(b, c, `${label}: repeated vertex`);
        assert.notEqual(a, c, `${label}: repeated vertex`);
        const o = orient(...points[a], ...points[b], ...points[c]);
        assert.notEqual(o, 0, `${label}: zero-area triangle`);
        if (winding === 0) winding = Math.sign(o);
        else assert.equal(Math.sign(o), winding, `${label}: consistent winding`);
        const key = triKey(tri, t);
        assert.ok(!seenTris.has(key), `${label}: duplicated triangle ${key}`);
        seenTris.add(key);
    }

    // independent edge statistics: interior edges twice, hull edges once
    const edgeCount = new Map();
    for (let t = 0; t < tri.length; t += 3) {
        for (const [u, v] of triangleEdges(tri, t)) {
            const k = edgeKey(u, v);
            edgeCount.set(k, (edgeCount.get(k) || 0) + 1);
        }
    }
    const hullEdges = new Set();
    for (let i = 0; i < d.hull.length; i++) {
        hullEdges.add(edgeKey(d.hull[i], d.hull[(i + 1) % d.hull.length]));
    }
    for (const [k, count] of edgeCount) {
        if (hullEdges.has(k)) assert.equal(count, 1, `${label}: hull edge ${k} used once`);
        else assert.equal(count, 2, `${label}: interior edge ${k} shared by two triangles`);
    }

    // triangle intersection check: no proper edge crossings and no vertex inside another triangle
    const tris = [];
    for (let t = 0; t < tri.length; t += 3) tris.push([tri[t], tri[t + 1], tri[t + 2]]);
    for (let i = 0; i < tris.length; i++) {
        for (let j = i + 1; j < tris.length; j++) {
            const A = tris[i];
            const B = tris[j];
            const ea = [[A[0], A[1]], [A[1], A[2]], [A[2], A[0]]];
            const eb = [[B[0], B[1]], [B[1], B[2]], [B[2], B[0]]];
            for (const [a, b] of ea) {
                for (const [c, q] of eb) {
                    assert.ok(!properCross(points, a, b, c, q), `${label}: crossing triangle edges`);
                }
            }
            for (const p of A) {
                if (!B.includes(p)) assert.ok(!strictPointInTriangle(points, p, ...B), `${label}: vertex ${p} inside another triangle`);
            }
            for (const p of B) {
                if (!A.includes(p)) assert.ok(!strictPointInTriangle(points, p, ...A), `${label}: vertex ${p} inside another triangle`);
            }
        }
    }

    // local Delaunay legality using the test's own predicates
    for (let e = 0; e < half.length; e++) {
        const f = half[e];
        if (f === -1 || e > f) continue;
        const a0 = e - e % 3;
        const b0 = f - f % 3;
        const A = [tri[a0], tri[a0 + 1], tri[a0 + 2]];
        const B = [tri[b0], tri[b0 + 1], tri[b0 + 2]];
        const opposite = B.find(v => !A.includes(v));
        const o = orient(...points[A[0]], ...points[A[1]], ...points[A[2]]);
        const ic = incircle(...points[A[0]], ...points[A[1]], ...points[A[2]], ...points[opposite]);
        // the library's triangles are clockwise under the standard signed-area convention; for a
        // clockwise triangle an outside-circumcircle opposite vertex makes incircle positive, so a
        // legal edge gives a non-positive product and cocircular gives zero. Only floating-point
        // noise may push it slightly positive.
        const eps = 1e-9 * Math.max(1, Math.abs(o * ic));
        assert.ok(o * ic <= eps, `${label}: edge ${e} violates local Delaunay`);
    }

    // area check: triangles tile the hull polygon
    let meshArea = 0;
    for (let t = 0; t < tri.length; t += 3) meshArea += triangleArea(points, tri, t);
    let hullArea = 0;
    for (let i = 0; i < d.hull.length; i++) {
        const a = d.hull[i];
        const b = d.hull[(i + 1) % d.hull.length];
        hullArea += points[a][0] * points[b][1] - points[b][0] * points[a][1];
    }
    hullArea = Math.abs(hullArea);
    assert.ok(Math.abs(meshArea - hullArea) <= 1e-9 * Math.max(1, hullArea),
        `${label}: triangle area ${meshArea} does not tile hull area ${hullArea}`);

    return meshArea;
}

// one-shot reference triangulation of the surviving points for geometric comparison
function referenceMesh(points, d) {
    const kept = points.map((p, i) => [p, i]).filter(([, i]) => !d._removed.has(i));
    const ref = Delaunator.from(kept.map(([p]) => p));
    const indexOf = kept.map(([, i]) => i);
    const edges = new Set();
    for (let t = 0; t < ref.triangles.length; t += 3) {
        const ids = [indexOf[ref.triangles[t]], indexOf[ref.triangles[t + 1]], indexOf[ref.triangles[t + 2]]];
        for (let j = 0; j < 3; j++) edges.add(edgeKey(ids[j], ids[(j + 1) % 3]));
    }
    let area = 0;
    const refPoints = kept.map(([p]) => p);
    for (let t = 0; t < ref.triangles.length; t += 3) area += triangleArea(refPoints, ref.triangles, t);
    return {ref, edges, area};
}

function assertSameEdgeSet(d, refEdges, label) {
    const mine = new Set();
    const tri = d.triangles;
    for (let t = 0; t < tri.length; t += 3) {
        for (let j = 0; j < 3; j++) mine.add(edgeKey(tri[t + j], tri[t + (j + 1) % 3]));
    }
    assert.equal(mine.size, refEdges.size, `${label}: edge count`);
    for (const k of refEdges) assert.ok(mine.has(k), `${label}: missing reference edge ${k}`);
    for (const k of mine) assert.ok(refEdges.has(k), `${label}: unexpected edge ${k}`);
}

// generic (non-cocircular) fixture: the inner ring is asymmetrically jittered so that no four
// points are cocircular, which makes the Delaunay edge set unique
const plainPoints = [
    [0, 0], [10, 0], [10, 10], [0, 10],
    [3.0, 3.0], [7.1, 2.9], [6.8, 7.2], [2.9, 6.9],
    [5, 5], [5.1, 2.0], [2.0, 5.1]
];

test('removes a plain interior point with a local cavity patch', () => {
    const coords = Float64Array.from(plainPoints.flat());
    const d = new Delaunator(coords);
    const removedIndex = 8;

    const before = {
        coordsObject: d.coords,
        coords: d.coords.slice(),
        hull: Array.from(d.hull),
        triangles: d.triangles.slice(),
        halfedges: d.halfedges.slice()
    };

    // monitor that the local patch never falls back to update()
    let updateCalls = 0;
    const originalUpdate = d.update;
    d.update = function () {
        updateCalls++;
        return originalUpdate.apply(this, arguments);
    };

    assert.equal(d.removePoint(removedIndex), true);
    assert.equal(updateCalls, 0, 'removePoint must not call update()');

    // coords: same object, same length, identical values
    assert.equal(d.coords, before.coordsObject, 'coords object must be preserved');
    assert.equal(d.coords.length, before.coords.length, 'coords length must be preserved');
    for (let i = 0; i < before.coords.length; i++) assert.equal(d.coords[i], before.coords[i]);

    // hull order and geometry are untouched
    assert.deepEqual(Array.from(d.hull), before.hull, 'hull point order must not change');

    // the removed index must not appear anywhere and no slot was filled with a duplicate
    for (let i = 0; i < d.triangles.length; i++) assert.notEqual(d.triangles[i], removedIndex);

    const area = validateMesh(d, plainPoints, 'plain');

    // geometric comparison against a one-shot triangulation of the surviving points
    const {ref, edges, area: refArea} = referenceMesh(plainPoints, d);
    assert.equal(d.triangles.length, ref.triangles.length, 'triangle count matches reference');
    assert.ok(Math.abs(area - refArea) <= 1e-9 * Math.max(1, refArea), 'area matches reference');
    // generic (non-cocircular) input: the Delaunay edge set is unique
    assertSameEdgeSet(d, edges, 'plain');

    // anti-cheat: every triangle outside the removed star must survive verbatim,
    // proving the patch was local rather than a full rebuild
    const exterior = new Set();
    for (let t = 0; t < before.triangles.length; t += 3) {
        const ids = [before.triangles[t], before.triangles[t + 1], before.triangles[t + 2]];
        if (!ids.includes(removedIndex)) exterior.add(ids.sort((a, b) => a - b).join(':'));
    }
    const afterTris = new Set();
    for (let t = 0; t < d.triangles.length; t += 3) afterTris.add(triKey(d.triangles, t));
    for (const k of exterior) assert.ok(afterTris.has(k), `exterior triangle ${k} must be preserved`);

    // repeating the same removal reports false and changes nothing
    const frozenTriangles = d.triangles.slice();
    const frozenHalfedges = d.halfedges.slice();
    assert.equal(d.removePoint(removedIndex), false);
    assert.deepEqual(Array.from(d.triangles), Array.from(frozenTriangles));
    assert.deepEqual(Array.from(d.halfedges), Array.from(frozenHalfedges));
});

test('retriangulates a genuinely non-convex cavity without fan or flips over the boundary', () => {
    // integer coordinates: the test's plain predicates are exact on this fixture;
    // point 4's star ring has a reflex corner, so the cavity polygon is non-convex
    const points = [
        [0, 0], [40, 0], [40, 40], [0, 40],
        [28, 31], [14, 36], [16, 15], [33, 25], [31, 25]
    ];
    const d = Delaunator.from(points);
    const removedIndex = 4;

    // prove the star cavity really is non-convex: at least one reflex ring corner
    const ring = starRing(d, removedIndex);
    let reflex = 0;
    for (let k = 0; k < ring.length; k++) {
        const a = ring[k];
        const b = ring[(k + 1) % ring.length];
        const c = ring[(k + 2) % ring.length];
        // the ring is clockwise under the standard signed-area convention, so a reflex
        // corner turns counterclockwise and yields a positive signed area
        if (orient(...points[a], ...points[b], ...points[c]) > 0) reflex++;
    }
    assert.ok(reflex > 0 && reflex < ring.length, 'cavity polygon must mix convex and reflex corners');

    assert.equal(d.removePoint(removedIndex), true);
    validateMesh(d, points, 'non-convex');

    const {ref, area: refArea} = referenceMesh(points, d);
    assert.equal(d.triangles.length, ref.triangles.length);
    let area = 0;
    for (let t = 0; t < d.triangles.length; t += 3) area += triangleArea(points, d.triangles, t);
    assert.ok(Math.abs(area - refArea) <= 1e-9 * Math.max(1, refArea));
});

test('removes adjacent interior points consecutively using current topology', () => {
    const points = [];
    for (let x = 0; x <= 4; x++) {
        for (let y = 0; y <= 4; y++) points.push([x * 2, y * 2]);
    }
    // deterministically jitter every interior grid point so the mesh stays generic
    // (no cocircular ambiguities) while index adjacency still follows the grid
    const jitter = {
        6: [2.10, 2.12], 7: [4.05, 2.08], 8: [6.15, 2.03],
        11: [2.07, 4.14], 12: [4.11, 4.06], 13: [6.02, 4.18],
        16: [2.16, 6.09], 17: [4.08, 6.04], 18: [6.13, 6.11]
    };
    for (const [i, p] of Object.entries(jitter)) points[+i] = p;

    const d = Delaunator.from(points);
    const hull = Array.from(d.hull);
    // point 12 and its four grid neighbors, all strictly interior; after 12 is gone the
    // following removals must be resolved from the current topology, not construction-time links
    const sequence = [12, 7, 13, 17, 11];
    for (const index of sequence) assert.ok(!hull.includes(index), `fixture: ${index} must be interior`);

    for (const index of sequence) {
        assert.equal(d.removePoint(index), true, `removal of ${index}`);
        assert.equal(d.removePoint(index), false, `repeat removal of ${index}`);
        validateMesh(d, points, `after removing ${index}`);
        assert.deepEqual(Array.from(d.hull), hull, 'hull stays fixed across removals');
        for (let t = 0; t < d.triangles.length; t++) assert.notEqual(d.triangles[t], index);
    }

    const {ref, edges} = referenceMesh(points, d);
    assert.equal(d.triangles.length, ref.triangles.length);
    assertSameEdgeSet(d, edges, 'consecutive');

    // planar triangulation identity: E = 3V - 3 - h for the surviving vertices
    const remaining = points.length - sequence.length;
    const expectedEdges = 3 * remaining - 3 - hull.length;
    const actualEdges = new Set();
    for (let t = 0; t < d.triangles.length; t += 3) {
        for (let j = 0; j < 3; j++) actualEdges.add(edgeKey(d.triangles[t + j], d.triangles[t + (j + 1) % 3]));
    }
    assert.equal(actualEdges.size, expectedEdges);
});

test('handles cocircular configurations and extreme coordinate scales', () => {
    // integer circle centered at (5, 5) with radius 5: 3^2 + 4^2 = 5^2, so the four
    // interior points lie exactly on one circle while the square corners form the hull
    const base = [
        [0, 0], [10, 0], [10, 10], [0, 10],
        [8, 9], [2, 9], [8, 1], [2, 1],
        [5, 5]
    ];

    for (const scale of [1, 1e-7, 1e7]) {
        const points = base.map(([x, y]) => [x * scale, y * scale]);
        const d = Delaunator.from(points);

        // fixture sanity: the four ring points are cocircular. The determinant scales with
        // the fourth power of the coordinate scale, so normalize before tolerancing
        const sanity = incircle(...points[4], ...points[5], ...points[6], ...points[7]);
        assert.ok(Math.abs(sanity) <= 1e-7 * Math.pow(scale, 4),
            `fixture must be cocircular at scale ${scale} (got ${sanity})`);
        assert.ok(!Array.from(d.hull).includes(4), 'removed point must be interior');

        assert.equal(d.removePoint(4), true);
        const area = validateMesh(d, points, `cocircular scale ${scale}`);

        // geometric comparison only: cocircular meshes allow any legal diagonal,
        // so exact array (or edge set) equality is deliberately not required
        const {ref, area: refArea} = referenceMesh(points, d);
        assert.equal(d.triangles.length, ref.triangles.length);
        assert.ok(Math.abs(area - refArea) <= 1e-7 * Math.max(1, refArea),
            `area matches reference at scale ${scale}`);
        assert.ok(Number.isFinite(area) && area > 0, 'finite positive area at extreme scale');
    }

    // generic slender (non-degenerate) neighborhood must survive without a magnitude threshold
    const slender = [[0, 0], [10, 0], [10, 10], [0, 10]];
    for (let i = 0; i < 8; i++) slender.push([1 + i * 1.05, 5 + (i % 2) * 1e-9]);
    slender.push([5, 5.0000000005]);
    const d = Delaunator.from(slender);
    const target = slender.length - 1;
    assert.equal(d.removePoint(target), true);
    validateMesh(d, slender, 'slender');
});

test('rejects invalid removals and restores points through update()', () => {
    const d = Delaunator.from(plainPoints);
    const snapshot = {
        triangles: d.triangles.slice(),
        halfedges: d.halfedges.slice(),
        hull: d.hull.slice()
    };

    // non-integer and out-of-bounds indices throw RangeError without touching the mesh
    for (const bad of [1.5, -1, plainPoints.length, NaN, Infinity]) {
        assert.throws(() => d.removePoint(bad), RangeError, `index ${bad}`);
    }

    // current convex hull vertices throw RangeError
    for (const hullIndex of d.hull) {
        assert.throws(() => d.removePoint(hullIndex), RangeError, `hull vertex ${hullIndex}`);
    }

    // the rejected calls left the mesh fully usable and byte-identical
    assert.deepEqual(Array.from(d.triangles), Array.from(snapshot.triangles));
    assert.deepEqual(Array.from(d.halfedges), Array.from(snapshot.halfedges));
    assert.deepEqual(Array.from(d.hull), Array.from(snapshot.hull));
    validateMesh(d, plainPoints, 'after rejections');

    // a duplicate point skipped at construction is not in the mesh: false, no error
    const dup = Delaunator.from(plainPoints.concat([[5, 5]]));
    assert.equal(dup.removePoint(plainPoints.length), false);

    // remove several points, then rebuild: update() must restore every removed point
    assert.equal(d.removePoint(8), true);
    assert.equal(d.removePoint(4), true);
    validateMesh(d, plainPoints, 'before restore');

    d.update();
    assert.equal(d._removed.size, 0);
    const fresh = Delaunator.from(plainPoints);
    assert.deepEqual(Array.from(d.triangles), Array.from(fresh.triangles), 'update restores the full mesh');
    validateMesh(d, plainPoints, 'restored');

    // restored points can be removed again
    assert.equal(d.removePoint(8), true);
    validateMesh(d, plainPoints, 're-removed after restore');
    assert.equal(d.removePoint(8), false);
});
