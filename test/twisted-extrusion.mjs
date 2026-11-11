// Tests for twisted polygon extrusion (twist / twistSegments / twistOrigin).
// Run with: node --no-warnings --loader ./checks/loader.mjs test/twisted-extrusion.mjs
import assert from 'node:assert/strict';
import { writeFileSync } from 'node:fs';
import { extrudePolygon } from '../src/main.js';

const Z_EPS = 1e-3;
const XY_EPS = 1e-4;

// Asymmetric rectangle (extra vertex (4,0) on the right edge) with an eccentric hole.
const outer = [[4, 0], [4, 3], [-3, 3], [-3, -2], [4, -2]];
const hole = [[1, -0.5], [2.5, -0.5], [2.5, 1], [1, 1]];
const mainPoly = [outer, hole];
const HOLE_CENTER = [1.75, 0.25];
const MAIN_AREA = 35 - 2.25; // 32.75

// Outer ring with a concave corner (L shape).
const concavePoly = [[[0, 0], [6, 0], [6, 2], [2, 2], [2, 5], [0, 5]]];
const CONCAVE_AREA = 18;

function key2(x, y) {
    return `${Math.round(x / XY_EPS)},${Math.round(y / XY_EPS)}`;
}

function rotate(x, y, angle, origin) {
    const c = Math.cos(angle);
    const s = Math.sin(angle);
    const dx = x - origin[0];
    const dy = y - origin[1];
    return [origin[0] + dx * c - dy * s, origin[1] + dx * s + dy * c];
}

function levelsOf(mesh) {
    const map = new Map();
    for (let i = 2; i < mesh.position.length; i += 3) {
        const key = Math.round(mesh.position[i] / Z_EPS);
        if (!map.has(key)) {
            map.set(key, mesh.position[i]);
        }
    }
    return [...map.values()].sort((a, b) => a - b);
}

function countAtLevel(mesh, z) {
    let n = 0;
    for (let i = 2; i < mesh.position.length; i += 3) {
        if (Math.abs(mesh.position[i + 2] - z) < Z_EPS) {
            n++;
        }
    }
    return n;
}

function hasVertex(mesh, x, y, z) {
    for (let i = 0; i < mesh.position.length; i += 3) {
        if (Math.abs(mesh.position[i] - x) < XY_EPS
            && Math.abs(mesh.position[i + 1] - y) < XY_EPS
            && Math.abs(mesh.position[i + 2] - z) < Z_EPS) {
            return true;
        }
    }
    return false;
}

function signedVolume(mesh) {
    let volume = 0;
    for (let i = 0; i < mesh.indices.length; i += 3) {
        const p = Array.from(mesh.indices.slice(i, i + 3), v => Array.from(mesh.position.slice(v * 3, v * 3 + 3)));
        const [a, b, c] = p;
        volume += (a[0] * (b[1] * c[2] - b[2] * c[1])
            + a[1] * (b[2] * c[0] - b[0] * c[2])
            + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6;
    }
    return volume;
}

function checkFinite(mesh) {
    for (const arr of [mesh.position, mesh.normal, mesh.uv]) {
        for (const v of arr) {
            assert.ok(Number.isFinite(v), 'all output values finite');
        }
    }
}

function checkUnitNormals(mesh) {
    const referenced = new Uint8Array(mesh.position.length / 3);
    for (const i of mesh.indices) {
        referenced[i] = 1;
    }
    for (let i = 0; i < referenced.length; i++) {
        if (!referenced[i]) {
            continue;
        }
        const l = Math.hypot(mesh.normal[i * 3], mesh.normal[i * 3 + 1], mesh.normal[i * 3 + 2]);
        assert.ok(Math.abs(l - 1) < 1e-3, `referenced vertex ${i} has unit normal, got ${l}`);
    }
}

// Cluster z values into levels, tolerating the per-vertex z spread of bevel rings.
function clusterLevels(mesh, gap) {
    const zs = [];
    for (let i = 2; i < mesh.position.length; i += 3) {
        zs.push(mesh.position[i]);
    }
    zs.sort((a, b) => a - b);
    const levels = [];
    let sum = 0, count = 0, prev = -Infinity;
    for (const z of zs) {
        if (z - prev > gap && count > 0) {
            levels.push(sum / count);
            sum = 0;
            count = 0;
        }
        sum += z;
        count++;
        prev = z;
    }
    if (count > 0) {
        levels.push(sum / count);
    }
    return levels;
}

// Every consecutive pair of z levels must be connected by real triangles.
function checkRingsConnected(mesh, levels) {
    const lvl = new Array(mesh.position.length / 3);
    for (let i = 0; i < lvl.length; i++) {
        const z = mesh.position[i * 3 + 2];
        let best = 0;
        let bestDist = Infinity;
        for (let L = 0; L < levels.length; L++) {
            const d = Math.abs(levels[L] - z);
            if (d < bestDist) {
                bestDist = d;
                best = L;
            }
        }
        lvl[i] = best;
    }
    const connected = new Set();
    for (let f = 0; f < mesh.indices.length; f += 3) {
        const a = lvl[mesh.indices[f]];
        const b = lvl[mesh.indices[f + 1]];
        const c = lvl[mesh.indices[f + 2]];
        const mn = Math.min(a, b, c);
        const mx = Math.max(a, b, c);
        if (mx - mn === 1) {
            connected.add(`${mn}-${mx}`);
        }
    }
    for (let i = 0; i < levels.length - 1; i++) {
        assert.ok(connected.has(`${i}-${i + 1}`), `rings at level ${i} and ${i + 1} are connected by indices`);
    }
}

// Rotate every vertex back by -twist*z/depth and verify it lands on an original
// section point (exterior or hole), at every layer.
function checkUntwist(mesh, expectedPoints, twist, depth, origin) {
    const expected = new Set();
    for (const [x, y] of expectedPoints) {
        expected.add(key2(x, y));
    }
    const perLevel = new Map();
    for (let i = 0; i < mesh.position.length; i += 3) {
        const z = mesh.position[i + 2];
        const [x, y] = rotate(mesh.position[i], mesh.position[i + 1], -twist * z / depth, origin);
        const k = key2(x, y);
        assert.ok(expected.has(k), `untwisted point (${x.toFixed(4)},${y.toFixed(4)}) at z=${z.toFixed(4)} is an original section point`);
        const lk = Math.round(z / Z_EPS);
        if (!perLevel.has(lk)) {
            perLevel.set(lk, new Set());
        }
        perLevel.get(lk).add(k);
    }
    for (const [lk, set] of perLevel) {
        for (const k of expected) {
            assert.ok(set.has(k), `layer z~${(lk * Z_EPS).toFixed(3)} contains full section incl. hole (${k})`);
        }
    }
}

function pointInTri2D(px, py, ax, ay, bx, by, cx, cy) {
    const d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
    const d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
    const d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
    const hasNeg = (d1 < -1e-9) || (d2 < -1e-9) || (d3 < -1e-9);
    const hasPos = (d1 > 1e-9) || (d2 > 1e-9) || (d3 > 1e-9);
    return !(hasNeg && hasPos);
}

// The cap at zCap must keep the hole open: no cap triangle covers the (rotated) hole center.
function checkHoleOpen(mesh, holeCenter, twist, depth, origin, zCap) {
    const [px, py] = rotate(holeCenter[0], holeCenter[1], twist * zCap / depth, origin);
    for (let f = 0; f < mesh.indices.length; f += 3) {
        const i1 = mesh.indices[f] * 3;
        const i2 = mesh.indices[f + 1] * 3;
        const i3 = mesh.indices[f + 2] * 3;
        if (Math.abs(mesh.position[i1 + 2] - zCap) > Z_EPS
            || Math.abs(mesh.position[i2 + 2] - zCap) > Z_EPS
            || Math.abs(mesh.position[i3 + 2] - zCap) > Z_EPS) {
            continue;
        }
        assert.ok(!pointInTri2D(
            px, py,
            mesh.position[i1], mesh.position[i1 + 1],
            mesh.position[i2], mesh.position[i2 + 1],
            mesh.position[i3], mesh.position[i3 + 1]
        ), `cap at z=${zCap} keeps the hole open`);
    }
}

function xyRange(mesh) {
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    for (let i = 0; i < mesh.position.length; i += 3) {
        minX = Math.min(minX, mesh.position[i]);
        minY = Math.min(minY, mesh.position[i + 1]);
        maxX = Math.max(maxX, mesh.position[i]);
        maxY = Math.max(maxY, mesh.position[i + 1]);
    }
    return {minX, minY, maxX, maxY};
}

function fmtRange(r) {
    return `[${r.minX.toFixed(3)},${r.minY.toFixed(3)}]~[${r.maxX.toFixed(3)},${r.maxY.toFixed(3)}]`;
}

const mainPoints = outer.concat(hole);

// ---------- 1. Core twist: depth 4, origin (0,0), total angle PI/2 ----------
{
    const polySnapshot = JSON.stringify(mainPoly);
    const opts = {depth: 4, twist: Math.PI / 2, twistSegments: 4, twistOrigin: [0, 0], smoothSide: false};
    const optsSnapshot = JSON.stringify(opts);
    const mesh = extrudePolygon([mainPoly], opts);
    // Inputs must not be mutated.
    assert.equal(JSON.stringify(mainPoly), polySnapshot);
    assert.equal(JSON.stringify(opts), optsSnapshot);

    const levels = levelsOf(mesh);
    assert.deepEqual(levels, [0, 1, 2, 3, 4], 'side passes through z=depth*k/twistSegments');

    // The reference numbers: (4,0) -> top (0,4), middle layer -> (2*sqrt2, 2*sqrt2).
    assert.ok(hasVertex(mesh, 0, 4, 4), 'point (4,0) twists to (0,4) at top');
    assert.ok(hasVertex(mesh, 2 * Math.SQRT2, 2 * Math.SQRT2, 2), 'point (4,0) twists to (2√2,2√2) at middle');
    assert.ok(hasVertex(mesh, 4, 0, 0), 'bottom is not rotated');

    // Untwist every layer and compare with the original section incl. hole.
    checkUntwist(mesh, mainPoints, Math.PI / 2, 4, [0, 0]);
    checkRingsConnected(mesh, levels);
    checkFinite(mesh);
    checkUnitNormals(mesh);

    // Exact discrete structure: 9 section points, smoothSide:false doubles ring vertices.
    assert.equal(mesh.position.length / 3, 9 * 2 + (10 + 8) * 5, 'vertex count');
    assert.equal(mesh.indices.length / 3, 9 * 2 + (5 + 4) * 2 * 4, 'face count');

    // Outward winding of walls and hole walls: positive signed volume, bounded by
    // area * depth and converging to it as twistSegments grows (the discrete
    // solid interpolates linearly between rings, slightly shrinking the volume).
    const volume = signedVolume(mesh);
    assert.ok(volume > 0.8 * MAIN_AREA * 4 && volume <= MAIN_AREA * 4 + 1e-3,
        `signed volume ${volume} within (0.8, 1] * ${MAIN_AREA * 4}`);
    const fine = extrudePolygon([mainPoly], {
        depth: 4, twist: Math.PI / 2, twistSegments: 256, twistOrigin: [0, 0], smoothSide: false
    });
    const fineVolume = signedVolume(fine);
    assert.ok(fineVolume > volume && Math.abs(fineVolume - MAIN_AREA * 4) < 0.5,
        `volume converges to ${MAIN_AREA * 4} with more segments, got ${fineVolume}`);

    // Caps keep the hole open.
    checkHoleOpen(mesh, HOLE_CENTER, Math.PI / 2, 4, [0, 0], 4);
    checkHoleOpen(mesh, HOLE_CENTER, Math.PI / 2, 4, [0, 0], 0);

    // UV follows the untwisted section: side vertices (after the 18 cap vertices)
    // share u across layers and have v == z / size.
    const size = 7; // max(rect.width, rect.height, depth)
    const uByPoint = new Map();
    for (let i = 18; i < mesh.position.length / 3; i++) {
        const z = mesh.position[i * 3 + 2];
        const [x, y] = rotate(mesh.position[i * 3], mesh.position[i * 3 + 1], -Math.PI / 2 * z / 4, [0, 0]);
        const k = key2(x, y);
        const u = mesh.uv[i * 2];
        const v = mesh.uv[i * 2 + 1];
        assert.ok(u >= 0 && u <= 1 && v >= 0 && v <= 1, 'uv wrapped into [0,1]');
        assert.ok(Math.abs(v - z / size) < 1e-4, `v follows untwisted side parameterization (${v} == ${z / size})`);
        if (uByPoint.has(k)) {
            assert.ok(Math.abs(uByPoint.get(k) - u) < 1e-4, 'u identical across twisted layers');
        }
        else {
            uByPoint.set(k, u);
        }
    }

    // boundingRect reflects the actual twisted XY extent.
    const range = xyRange(mesh);
    assert.ok(Math.abs(mesh.boundingRect.x - range.minX) < 1e-4);
    assert.ok(Math.abs(mesh.boundingRect.y - range.minY) < 1e-4);
    assert.ok(Math.abs(mesh.boundingRect.width - (range.maxX - range.minX)) < 1e-4);
    assert.ok(Math.abs(mesh.boundingRect.height - (range.maxY - range.minY)) < 1e-4);
    assert.ok(range.minX < -3 && range.maxX > 4 && range.minY < -2 && range.maxY > 4, 'twisted extent exceeds original rect');

    console.log(`TWIST_CORE layers=${levels.length} faces=${mesh.indices.length / 3} xy=${fmtRange(range)} volume=${volume.toFixed(3)}`);
}

// ---------- 2. Default twistSegments (8) and default twistOrigin ----------
{
    const mesh = extrudePolygon([mainPoly], {depth: 4, twist: Math.PI / 2, smoothSide: false});
    const levels = levelsOf(mesh);
    assert.equal(levels.length, 9, 'default twistSegments is 8');
    for (let k = 0; k <= 8; k++) {
        assert.ok(levels.some(z => Math.abs(z - k * 0.5) < Z_EPS), `level ${k * 0.5} present`);
    }
    // Default origin = center of transformed bounding rect = ((4-3)/2, (3-2)/2) = (0.5, 0.5).
    checkUntwist(mesh, mainPoints, Math.PI / 2, 4, [0.5, 0.5]);
    console.log('TWIST_DEFAULTS segments=8 origin=bounds-center');
}

// ---------- 3. twistOrigin lives in the transformed (translate/scale) coordinate ----------
{
    const translate = [10, 20];
    const scale = [1, 1];
    const mesh = extrudePolygon([mainPoly], {
        depth: 4, twist: Math.PI / 2, twistSegments: 4, translate, scale, smoothSide: false
    });
    // Transformed bounding rect: x 7..14, y 18..23 -> center (10.5, 20.5).
    const center = [10.5, 20.5];
    checkUntwist(mesh, mainPoints.map(([x, y]) => [x + 10, y + 20]), Math.PI / 2, 4, center);
    // (4,0) -> (14,20) -> rotated 90° around (10.5,20.5) -> (11,24).
    assert.ok(hasVertex(mesh, 11, 24, 4), 'translated point twists around transformed bounds center');
    console.log('TWIST_TRANSFORM_ORIGIN translate-applied');
}

// ---------- 4. Negative twist ----------
{
    const mesh = extrudePolygon([mainPoly], {
        depth: 4, twist: -Math.PI / 2, twistSegments: 4, twistOrigin: [0, 0], smoothSide: false
    });
    assert.ok(hasVertex(mesh, 0, -4, 4), 'point (4,0) twists to (0,-4) at top for negative angle');
    assert.ok(hasVertex(mesh, 2 * Math.SQRT2, -2 * Math.SQRT2, 2), 'middle layer at -45°');
    checkUntwist(mesh, mainPoints, -Math.PI / 2, 4, [0, 0]);
    checkRingsConnected(mesh, levelsOf(mesh));
    checkFinite(mesh);
    checkUnitNormals(mesh);
    const volume = signedVolume(mesh);
    // Fixed quad diagonal makes the discrete solid bulge outward for negative
    // twist; it still converges to area * depth with more segments.
    assert.ok(volume >= MAIN_AREA * 4 - 1e-3 && volume < 1.2 * MAIN_AREA * 4,
        'negative twist keeps outward winding');
    const fine = extrudePolygon([mainPoly], {
        depth: 4, twist: -Math.PI / 2, twistSegments: 256, twistOrigin: [0, 0], smoothSide: false
    });
    assert.ok(Math.abs(signedVolume(fine) - MAIN_AREA * 4) < 0.5,
        'negative twist volume converges to area * depth');
    console.log('TWIST_NEGATIVE angle=-π/2 ok');
}

// ---------- 5. Bevel with hard and soft edges ----------
{
    const common = {
        depth: 4, bevelSize: 0.5, bevelSegments: 2,
        twist: Math.PI / 2, twistSegments: 4, twistOrigin: [0, 0]
    };
    const expectedLevels = [0, 0.5 * (1 - Math.sin(Math.PI / 4)), 0.5, 1, 2, 3, 3.5, 3.5 + 0.5 * Math.cos(Math.PI / 4), 4];
    for (const [name, extra] of [['soft', {smoothSide: true, smoothBevel: true}], ['hard', {smoothSide: false, smoothBevel: false}]]) {
        const mesh = extrudePolygon([mainPoly], Object.assign({}, common, extra));
        checkFinite(mesh);
        checkUnitNormals(mesh);
        const levels = levelsOf(mesh);
        for (const z of expectedLevels) {
            assert.ok(levels.some(L => Math.abs(L - z) < Z_EPS), `${name}: bevel and twist heights both present (z=${z.toFixed(4)})`);
        }
        checkRingsConnected(mesh, clusterLevels(mesh, 0.08));
        checkHoleOpen(mesh, HOLE_CENTER, Math.PI / 2, 4, [0, 0], 4);
        checkHoleOpen(mesh, HOLE_CENTER, Math.PI / 2, 4, [0, 0], 0);
        const volume = signedVolume(mesh);
        assert.ok(volume > 100 && volume < MAIN_AREA * 4, `${name}: bevel volume ${volume.toFixed(3)} within (100, ${MAIN_AREA * 4})`);
        if (name === 'hard') {
            // Non-smooth bevel duplicates rings at the same height for split normals.
            assert.equal(countAtLevel(mesh, 3.5 + 0.5 * Math.cos(Math.PI / 4)), 2 * countAtLevel(mesh, 1),
                'hard bevel duplicates rings for split normals');
        }
        console.log(`TWIST_BEVEL_${name.toUpperCase()} layers=${levels.length} faces=${mesh.indices.length / 3} finite-normals`);
    }
    const soft = extrudePolygon([mainPoly], Object.assign({}, common, {smoothSide: true, smoothBevel: true}));
    const hard = extrudePolygon([mainPoly], Object.assign({}, common, {smoothSide: false, smoothBevel: false}));
    assert.ok(hard.position.length > soft.position.length, 'hard edges split more vertices');
}

// ---------- 6. Two polygons with different depths via depth callback ----------
{
    const mesh = extrudePolygon([mainPoly, concavePoly], {
        depth: idx => idx === 0 ? 4 : 2,
        twist: Math.PI / 3, twistSegments: 3, twistOrigin: [0, 0], smoothSide: false
    });
    checkFinite(mesh);
    checkUnitNormals(mesh);
    const levels = levelsOf(mesh);
    for (const z of [0, 2 / 3, 4 / 3, 2, 8 / 3, 4]) {
        assert.ok(levels.some(L => Math.abs(L - z) < Z_EPS), `level ${z.toFixed(3)} present`);
    }
    const volume = signedVolume(mesh);
    const expectedVolume = MAIN_AREA * 4 + CONCAVE_AREA * 2;
    assert.ok(volume > 0.8 * expectedVolume && volume <= expectedVolume + 1e-3,
        `volume ${volume.toFixed(3)} within (0.8, 1] * ${expectedVolume}`);
    console.log(`TWIST_TWO_DEPTHS layers=${levels.length} faces=${mesh.indices.length / 3} volume=${volume.toFixed(3)}`);
}

// ---------- 7. excludeBottom ----------
{
    const mesh = extrudePolygon([mainPoly], {
        depth: 4, twist: Math.PI / 2, twistSegments: 2, twistOrigin: [0, 0],
        excludeBottom: true, smoothSide: false
    });
    // Side bottom ring at z=0 still exists, but no bottom cap triangles.
    assert.ok(countAtLevel(mesh, 0) > 0, 'bottom side ring exists');
    for (let f = 0; f < mesh.indices.length; f += 3) {
        const zs = [0, 1, 2].map(k => mesh.position[mesh.indices[f + k] * 3 + 2]);
        assert.ok(!(Math.abs(zs[0]) < Z_EPS && Math.abs(zs[1]) < Z_EPS && Math.abs(zs[2]) < Z_EPS),
            'no bottom cap triangle');
    }
    assert.equal(mesh.position.length / 3, 9 + (10 + 8) * 3, 'vertex count without bottom cap');
    checkHoleOpen(mesh, HOLE_CENTER, Math.PI / 2, 4, [0, 0], 4);
    checkUntwist(mesh, mainPoints, Math.PI / 2, 4, [0, 0]);
    console.log('TWIST_EXCLUDE_BOTTOM ok');
}

// ---------- 8. Zero twist is byte-identical to the old behavior ----------
{
    const baseOpts = {depth: 4, bevelSize: 0.3, bevelSegments: 3, smoothSide: 'auto', smoothBevel: true};
    const a = extrudePolygon([mainPoly], baseOpts);
    const b = extrudePolygon([mainPoly], Object.assign({}, baseOpts, {twist: 0}));
    const c = extrudePolygon([mainPoly], Object.assign({}, baseOpts, {twist: 0, twistSegments: 5, twistOrigin: [1, 2]}));
    for (const m of [b, c]) {
        assert.deepEqual(m.position, a.position);
        assert.deepEqual(m.indices, a.indices);
        assert.deepEqual(m.uv, a.uv);
        assert.deepEqual(m.normal, a.normal);
        assert.deepEqual(m.boundingRect, a.boundingRect);
    }
    console.log('TWIST_ZERO_BYTE_IDENTICAL position/indices/uv/normal/boundingRect');
}

// ---------- 9. Uint32 indices with many segments ----------
{
    const mesh = extrudePolygon([mainPoly], {
        depth: 4, twist: Math.PI / 2, twistSegments: 3640, twistOrigin: [0, 0], smoothSide: false
    });
    assert.ok(mesh.position.length / 3 > 0xffff, 'vertex count exceeds Uint16 range');
    assert.ok(mesh.indices instanceof Uint32Array, 'Uint32 indices triggered');
    const levels = levelsOf(mesh);
    assert.equal(levels.length, 3641, 'all twist layers present');
    checkRingsConnected(mesh, levels);
    checkFinite(mesh);
    checkUntwist(mesh, mainPoints, Math.PI / 2, 4, [0, 0]);
    for (const i of mesh.indices) {
        assert.ok(i < mesh.position.length / 3, 'indices in range');
    }
    console.log(`TWIST_UINT32 layers=${levels.length} vertices=${mesh.position.length / 3} faces=${mesh.indices.length / 3}`);
}

// ---------- 10. RangeError on invalid twist parameters ----------
{
    for (const opts of [
        {twist: NaN}, {twist: Infinity}, {twist: -Infinity},
        {twist: 1, twistSegments: 0}, {twist: 1, twistSegments: -2},
        {twist: 1, twistSegments: 2.5}, {twist: 1, twistSegments: NaN},
        {twist: 1, twistOrigin: [NaN, 0]}, {twist: 1, twistOrigin: [0, Infinity]}
    ]) {
        assert.throws(() => extrudePolygon([mainPoly], Object.assign({depth: 4}, opts)), RangeError,
            `RangeError for ${JSON.stringify(opts)}`);
    }
    console.log('TWIST_RANGE_ERRORS ok');
}

// ---------- 11. OBJ preview of a twisted concave outline ----------
{
    const mesh = extrudePolygon([concavePoly], {
        depth: 3, twist: Math.PI / 2, twistSegments: 6, smoothSide: 'auto'
    });
    checkFinite(mesh);
    checkUnitNormals(mesh);
    const levels = levelsOf(mesh);
    assert.equal(levels.length, 7);
    const lines = ['# geometry-extrude twisted extrusion preview (concave L, twist=PI/2)'];
    for (let i = 0; i < mesh.position.length; i += 3) {
        lines.push(`v ${mesh.position[i]} ${mesh.position[i + 1]} ${mesh.position[i + 2]}`);
    }
    for (let i = 0; i < mesh.indices.length; i += 3) {
        lines.push(`f ${mesh.indices[i] + 1} ${mesh.indices[i + 1] + 1} ${mesh.indices[i + 2] + 1}`);
    }
    const objUrl = new URL('./twisted-extrusion-preview.obj', import.meta.url);
    writeFileSync(objUrl, lines.join('\n') + '\n');
    const range = xyRange(mesh);
    console.log(`TWIST_OBJ_PREVIEW layers=${levels.length} faces=${mesh.indices.length / 3} xy=${fmtRange(range)} file=test/twisted-extrusion-preview.obj`);
}

console.log('TWISTED_EXTRUSION_OK');
