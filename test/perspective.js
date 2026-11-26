'use strict';

/*
 * test/perspective.js
 *
 * Three regression groups for terrain-renderer.js:
 *
 *   projection(api, check)  - camera basis / pixel rays / fov convention
 *   traversal(api, check)   - traceTerrain analytic cases and degeneracies
 *   integration(api, check) - every pixel of small frames checked against a
 *                             slow brute-force per-column geometry reference
 *
 * Every group receives the renderer module as `api` and a runner supplied
 * `check(condition, message)` function. Each returns the number of check
 * calls it made (which must equal the runner's own count). A failed check
 * throws, so counts only come back on full success.
 */

function near(a, b, eps) {
    return Math.abs(a - b) <= (eps == null ? 1e-7 : eps);
}

function mod(n, m) {
    return ((n % m) + m) % m;
}

function vNear(a, b, eps) {
    eps = eps == null ? 1e-7 : eps;
    return a && b && near(a[0], b[0], eps) && near(a[1], b[1], eps) && near(a[2], b[2], eps);
}

// Wrap the runner check with a local invocation counter.
function counter(check) {
    let n = 0;
    return {
        n: function () { return n; },
        ok: function (condition, message) {
            check(!!condition, message);
            n++;
        }
    };
}

function makeMap(w, h, altFill, colorFill) {
    const altitude = new Array(w * h);
    const color = new Array(w * h);
    for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
            const i = x + w * y;
            altitude[i] = typeof altFill === 'function' ? altFill(x, y) : altFill;
            color[i] = typeof colorFill === 'function' ? colorFill(x, y, i) : colorFill;
        }
    }
    return { width: w, height: h, altitude: altitude, color: color };
}

// ---------------------------------------------------------------------------
// projection
// ---------------------------------------------------------------------------

function projection(api, check) {
    const c = counter(check);
    const ok = c.ok;

    ok(typeof api.traceTerrain === 'function', 'traceTerrain is exported');
    ok(typeof api.renderPerspective === 'function', 'renderPerspective is exported');
    ok(typeof api.rayDirection === 'function', 'rayDirection helper is exported');
    ok(typeof api.cameraBasis === 'function', 'cameraBasis helper is exported');

    // Zero angles: forward (0,-1,0), right (1,0,0), up (0,0,1).
    let b = api.cameraBasis({ yaw: 0, pitch: 0, roll: 0 });
    ok(vNear(b.forward, [0, -1, 0]), 'zero-angle forward is (0,-1,0)');
    ok(vNear(b.right, [1, 0, 0]), 'zero-angle right is (1,0,0)');
    ok(vNear(b.up, [0, 0, 1]), 'zero-angle up is (0,0,1)');

    // Yaw +90 degrees around world Z, right-hand positive:
    // (0,-1,0) rotates to (+1,0,0).
    b = api.cameraBasis({ yaw: Math.PI / 2 });
    ok(vNear(b.forward, [1, 0, 0]), 'positive yaw +90 faces +X');
    ok(vNear(b.right, [0, 1, 0]), 'positive yaw +90 right is +Y');

    // Pitch +90 around the (yawed) right axis looks straight down;
    // negative pitch looks straight up.
    b = api.cameraBasis({ pitch: Math.PI / 2 });
    ok(vNear(b.forward, [0, 0, -1]), 'positive pitch +90 looks down (-Z)');
    b = api.cameraBasis({ pitch: -Math.PI / 2 });
    ok(vNear(b.forward, [0, 0, 1]), 'negative pitch -90 looks up (+Z)');

    // Roll +90 about forward (-Y): right -> +Z, up -> -X.
    b = api.cameraBasis({ roll: Math.PI / 2 });
    ok(vNear(b.right, [0, 0, 1]), 'positive roll turns right toward +Z');
    ok(vNear(b.up, [-1, 0, 0]), 'positive roll turns up toward -X');
    // Roll must not change the centre forward ray.
    ok(vNear(b.forward, [0, -1, 0]), 'roll leaves forward axis in place');

    // A 1x1 image has its only pixel centre exactly on the forward axis.
    let rd = api.rayDirection({ fovY: Math.PI / 3 }, 0, 0, 1, 1);
    ok(vNear(rd, [0, -1, 0], 1e-12), 'single centre pixel equals forward');
    ok(near(Math.hypot(rd[0], rd[1], rd[2]), 1, 1e-12), 'centre ray is unit length');

    // Every pixel-centre ray is unit length, including the four corners,
    // and corner rays actually move with roll.
    const W = 16, H = 10;
    const cam0 = { fovY: Math.PI / 2, roll: 0 };
    const camR = { fovY: Math.PI / 2, roll: 0.7 };
    let allUnit = true;
    for (const [px, py] of [[0, 0], [W - 1, 0], [0, H - 1], [W - 1, H - 1], [7, 4]]) {
        const r = api.rayDirection({}, px, py, W, H);
        if (!near(Math.hypot(r[0], r[1], r[2]), 1, 1e-12)) allUnit = false;
    }
    ok(allUnit, 'corner and centre rays are unit vectors');

    const tl0 = api.rayDirection(cam0, 0, 0, W, H);
    const tlR = api.rayDirection(camR, 0, 0, W, H);
    ok(!vNear(tl0, tlR, 1e-4), 'corner ray changes when rolling');

    // Vertical fov convention: top-centre and bottom-centre are symmetric
    // around horizontal forward with z = +/- tan(fovY/2) after normalising.
    const top = api.rayDirection({ fovY: Math.PI / 2 }, 7.5 - 0.5 + 0.5, 0, 15, 10);
    // Use exact centre column of a 15-wide image at top/bottom pixel centres.
    const tc = api.rayDirection({ fovY: Math.PI / 2 }, 7, 0, 15, 10);
    const bc = api.rayDirection({ fovY: Math.PI / 2 }, 7, 9, 15, 10);
    ok(tc[2] > 0 && bc[2] < 0 && near(tc[2], -bc[2], 1e-12),
        'top ray points up and bottom ray points down symmetrically');
    ok(top && Array.isArray(top), 'pixel helper accepts integer pixel coordinates');

    return c.n();
}

// ---------------------------------------------------------------------------
// traversal
// ---------------------------------------------------------------------------

function traversal(api, check) {
    const c = counter(check);
    const ok = c.ok;
    const trace = api.traceTerrain;

    // ---- flat ground ------------------------------------------------------
    const flat = makeMap(4, 4, 2, (x, y) => 0xff000000 + x + 4 * y);
    let h = trace(flat, [1.3, 2.7, 5], [0, 0, -1], 0, 40);
    ok(h !== null, 'flat: downward ray hits');
    ok(h && near(h.distance, 3), 'flat: distance is 5-2=3');
    ok(h && vNear(h.position, [1.3, 2.7, 2]), 'flat: position on top plane');
    ok(h && vNear(h.normal, [0, 0, 1]), 'flat: top normal +Z');
    ok(h && h.cell[0] === 1 && h.cell[1] === 2, 'flat: cell [1,2]');
    ok(h && h.color === flat.color[1 + 4 * 2], 'flat: color is struck column color');

    ok(trace(flat, [1.3, 2.7, 5], [0, 0, 1], 0, 40) === null, 'flat: upward sky is null');
    ok(trace(flat, [1.3, 2.7, 5], [0, 0, -1], 0, 2.9) === null, 'flat: far before hit is null');
    h = trace(flat, [1.3, 2.7, 5], [0, 0, -1], 3, 40);
    ok(h !== null && near(h.distance, 3), 'flat: near exactly on surface is closed-interval hit');

    // ---- isolated tall pillar --------------------------------------------
    const pillar = makeMap(5, 5, 0, 1);
    pillar.altitude[2 + 5 * 2] = 4;
    pillar.color[2 + 5 * 2] = 0xffabcdef;

    h = trace(pillar, [2.5, 0.5, 2], [0, 1, 0], 0, 20);
    ok(h && near(h.distance, 1.5) && vNear(h.normal, [0, -1, 0]), 'pillar: south wall distance/normal');
    ok(h && h.cell[0] === 2 && h.cell[1] === 2 && h.color === 0xffabcdef, 'pillar: south wall cell/color');

    h = trace(pillar, [2.5, 4.5, 2], [0, -1, 0], 0, 20);
    ok(h && near(h.distance, 1.5) && vNear(h.normal, [0, 1, 0]), 'pillar: north wall');

    h = trace(pillar, [0.5, 2.5, 2], [1, 0, 0], 0, 20);
    ok(h && near(h.distance, 1.5) && vNear(h.normal, [-1, 0, 0]), 'pillar: west wall');

    h = trace(pillar, [4.5, 2.5, 2], [-1, 0, 0], 0, 20);
    ok(h && near(h.distance, 1.5) && vNear(h.normal, [1, 0, 0]), 'pillar: east wall');

    h = trace(pillar, [2.5, 2.5, 9], [0, 0, -1], 0, 20);
    ok(h && near(h.distance, 5) && vNear(h.normal, [0, 0, 1]), 'pillar: top from above');

    // A horizontal ray above the pillar misses everything.
    ok(trace(pillar, [2.5, 0.5, 6], [0, 1, 0], 0, 20) === null, 'pillar: ray above all columns is sky');

    // ---- steps ------------------------------------------------------------
    const steps = makeMap(4, 1, (x) => x, 1); // heights 0,1,2,3 along +X
    h = trace(steps, [0.5, 0.5, 0.5], [1, 0, 0], 0, 20);
    ok(h && near(h.distance, 0.5) && vNear(h.normal, [-1, 0, 0]) && h.cell[0] === 1,
        'steps: z=.5 hits first riser at x=1');
    h = trace(steps, [0.5, 0.5, 1.5], [1, 0, 0], 0, 20);
    ok(h && near(h.distance, 1.5) && vNear(h.normal, [-1, 0, 0]) && h.cell[0] === 2,
        'steps: z=1.5 hits second riser at x=2');
    h = trace(steps, [0.5, 0.5, 2.5], [1, 0, 0], 0, 20);
    ok(h && near(h.distance, 2.5) && h.cell[0] === 3, 'steps: z=2.5 hits third riser at x=3');

    // Downward onto each tread gives the analytic vertical distance.
    h = trace(steps, [2.5, 0.5, 5], [0, 0, -1], 0, 20);
    ok(h && near(h.distance, 3) && h.cell[0] === 2, 'steps: tread of cell 2 at height 2');

    // Grid line between equal-height columns is not a surface.
    const equal = makeMap(3, 3, 3, 7);
    h = trace(equal, [-2.5, 1.5, 1.5], [1, 0, 0], 0, 20);
    // z=1.5 is below the common top 3: the ray stays inside the solid and
    // periodic equal-height seams are internal; no boundary leaves sideways.
    ok(h === null, 'equal-height seams are not walls (inside, no side exit)');
    h = trace(equal, [-2.5, 1.5, 5], [1, 0, 0], 0, 20);
    ok(h === null, 'horizontal ray above flat terrain never hits');

    // ---- 3x5 periodic terrain + negative coordinates ----------------------
    const h35 = [
        1, 0, 2,
        0, 3, 1,
        2, 1, 0,
        1, 2, 3,
        0, 1, 2
    ];
    const p35 = { width: 3, height: 5, altitude: h35, color: h35.map((_, i) => 0xff000000 + 100 + i) };

    h = trace(p35, [-0.5, -0.5, 6], [0, 0, -1], 0, 40);
    ok(h && h.cell[0] === 2 && h.cell[1] === 4, 'periodic: (-1,-1) wraps to [2,4]');
    ok(h && near(h.distance, 6 - h35[2 + 3 * 4]), 'periodic: wrapped cell top distance');
    ok(h && h.color === p35.color[2 + 3 * 4], 'periodic: wrapped cell color');

    h = trace(p35, [-1.5, -2.5, 5], [0, 0, -1], 0, 40);
    ok(h && h.cell[0] === 1 && h.cell[1] === 2, 'periodic: (-2,-3) wraps to [1,2]');
    ok(h && near(h.distance, 4), 'periodic: cell [1,2] height 1 gives distance 4');

    // Wall across the periodic seam: raw cell -1 (== column 2, height 3 in
    // row 3) versus column 0 (height 1).
    h = trace(p35, [-0.5, 3.5, 2], [1, 0, 0], 0, 40);
    ok(h && near(h.distance, 0.5) && vNear(h.normal, [1, 0, 0]),
        'periodic: seam wall hit with +X outward normal');
    ok(h && h.cell[0] === 2 && h.cell[1] === 3, 'periodic: seam wall owns wrapped cell [2,3]');

    // ---- near / origin embedded in solid ----------------------------------
    const tower = { width: 3, height: 1, altitude: [10, 0, 0], color: [111, 222, 333] };
    h = trace(tower, [0.5, 0.5, 5], [0, 0, 1], 0, 40);
    ok(h && near(h.distance, 5) && vNear(h.normal, [0, 0, 1]),
        'embedded origin: first boundary leaving through top');

    // near=0.1 is strictly inside cell 0; do not return near, find x=1 exit.
    h = trace(tower, [0.2, 0.5, 5], [1, 0, 0], 0.1, 40);
    ok(h && near(h.distance, 0.8) && vNear(h.normal, [1, 0, 0]),
        'near inside solid: leave through side at absolute t=0.8, not near');

    // near exactly beyond the top plane while descending inside an infinite
    // column: no later boundary exists, so the closed interval has no hit.
    const one = { width: 1, height: 1, altitude: [10], color: [1] };
    ok(trace(one, [0.5, 0.5, 20], [0, 0, -1], 12, 60) === null,
        'near cuts into an infinite column with no exit: null');

    // ---- simultaneous events and degenerate directions --------------------
    // Z beats X at an exact top edge.
    const edge = { width: 2, height: 1, altitude: [1, 0], color: [1, 2] };
    const invSqrt2 = 1 / Math.SQRT2;
    h = trace(edge, [-1, 0.5, 2], [invSqrt2, 0, -invSqrt2], 0, 20);
    ok(h && near(h.distance, Math.SQRT2) && h.normal[2] === 1,
        'tie priority: Z face wins over X at top edge');

    // X beats Y at an exact grid corner (pillar [1,1]).
    const corner = makeMap(3, 3, 0, 1);
    corner.altitude[1 + 3] = 4;
    h = trace(corner, [0, 0, 2], [invSqrt2, invSqrt2, 0], 0, 20);
    ok(h && near(h.distance, Math.SQRT2) && h.normal[0] === -1 && h.normal[1] === 0,
        'tie priority: X face wins over Y at grid corner');

    // Zero X component still hits a side wall.
    h = trace(pillar, [2.5, 0.5, 2], [0, 1, 0], 0, 20);
    ok(h && h.normal[1] === -1, 'zero component: Y wall still found');

    // Ray travelling exactly along an XY grid line must terminate and give
    // consistent results under a tiny nudge to either side of the line.
    const a1 = trace(corner, [1, 0.2, 2], [0, 1, 0], 0, 20);
    const a2 = trace(corner, [1 + 1e-12, 0.2, 2], [0, 1, 0], 0, 20);
    ok(a1 !== null && a2 !== null, 'along grid line: terminates with a hit');
    ok(a1 && a2 && near(a1.distance, a2.distance, 1e-6) &&
       a1.normal[0] === a2.normal[0] && a1.normal[1] === a2.normal[1],
       'along grid line: consistent distance/normal under tiny nudge');

    // Vertical ray landing exactly on a grid corner still terminates on Z.
    h = trace(flat, [1, 1, 9], [0, 0, -1], 0, 40);
    ok(h && near(h.distance, 7) && h.normal[2] === 1, 'vertical ray on grid corner hits Z');

    return c.n();
}

// ---------------------------------------------------------------------------
// integration: slow brute-force per-column geometry reference
// ---------------------------------------------------------------------------

// Enumerate every raw integer column the finite ray segment can touch and
// intersect its top plane and four sides directly. Heights are looked up
// through the periodic modulo, so negative raw indices are exercised too.
function bruteTrace(map, o, d, near, far) {
    const mw = map.width, mh = map.height;
    const H = function (i, j) {
        return +map.altitude[mod(i, mw) + mw * mod(j, mh)];
    };
    const ox = o[0], oy = o[1], oz = o[2];
    const dx = d[0], dy = d[1], dz = d[2];
    const EPS = 1e-7;
    let best = null;

    function consider(t, rank, cx, cy) {
        if (!Number.isFinite(t)) return;
        if (t < near - EPS || t > far + EPS) return;
        if (best === null || t < best.t - 1e-8 ||
            (Math.abs(t - best.t) <= 1e-7 && rank < best.rank)) {
            best = { t: t, rank: rank, cx: cx, cy: cy };
        }
    }

    const ax = ox + dx * near, ay = oy + dy * near;
    const bx = ox + dx * far, by = oy + dy * far;
    const i0 = Math.floor(Math.min(ax, bx)) - 1;
    const i1 = Math.floor(Math.max(ax, bx)) + 1;
    const j0 = Math.floor(Math.min(ay, by)) - 1;
    const j1 = Math.floor(Math.max(ay, by)) + 1;

    for (let i = i0; i <= i1; i++) {
        for (let j = j0; j <= j1; j++) {
            const h = H(i, j);

            // Top plane z = h, bounded to the column square.
            if (dz !== 0) {
                const t = (h - oz) / dz;
                const x = ox + dx * t;
                const y = oy + dy * t;
                if (x >= i - EPS && x <= i + 1 + EPS &&
                    y >= j - EPS && y <= j + 1 + EPS) {
                    consider(t, 0, i, j);
                }
            }

            // X sides. A side is exposed only when the neighbouring column
            // does not also contain the point.
            if (dx !== 0) {
                for (let edge = 0; edge < 2; edge++) {
                    const bxLine = i + edge; // i or i+1
                    const t = (bxLine - ox) / dx;
                    const y = oy + dy * t;
                    const z = oz + dz * t;
                    if (y < j - EPS || y > j + 1 + EPS || z > h + EPS) continue;
                    const hNeighbor = edge === 0 ? H(i - 1, j) : H(i + 1, j);
                    if (z > hNeighbor - EPS) consider(t, 1, i, j);
                }
            }

            // Y sides.
            if (dy !== 0) {
                for (let edge = 0; edge < 2; edge++) {
                    const byLine = j + edge;
                    const t = (byLine - oy) / dy;
                    const x = ox + dx * t;
                    const z = oz + dz * t;
                    if (x < i - EPS || x > i + 1 + EPS || z > h + EPS) continue;
                    const hNeighbor = edge === 0 ? H(i, j - 1) : H(i, j + 1);
                    if (z > hNeighbor - EPS) consider(t, 2, i, j);
                }
            }
        }
    }
    return best;
}

// Independent camera ray construction (written from the convention prose,
// not via the module's helper).
function referenceRay(cam, px, py, W, Hh) {
    const yaw = +cam.yaw || 0;
    const pitch = +cam.pitch || 0;
    const roll = +cam.roll || 0;
    const cy = Math.cos(yaw), sy = Math.sin(yaw);

    let f = [sy, -cy, 0];
    let r = [cy, sy, 0];

    // Rodrigues around the yawed right axis for pitch.
    function rod(v, k, a) {
        const c = Math.cos(a), s = Math.sin(a);
        const kv = [
            k[1] * v[2] - k[2] * v[1],
            k[2] * v[0] - k[0] * v[2],
            k[0] * v[1] - k[1] * v[0]
        ];
        const kdv = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
        return [0, 1, 2].map(function (i) {
            return v[i] * c + kv[i] * s + k[i] * kdv * (1 - c);
        });
    }

    f = rod(f, r, pitch);
    let u = [
        f[1] * r[2] - f[2] * r[1],
        f[2] * r[0] - f[0] * r[2],
        f[0] * r[1] - f[1] * r[0]
    ];
    r = rod(r, f, roll);
    u = rod(u, f, roll);

    const fovY = cam.fovY == null ? Math.PI / 3 : +cam.fovY;
    const tanY = Math.tan(fovY / 2);
    const tanX = tanY * (W / Hh);
    const sx = (2 * (px + 0.5)) / W - 1;
    const syy = 1 - (2 * (py + 0.5)) / Hh;

    let d = [
        f[0] + sx * tanX * r[0] + syy * tanY * u[0],
        f[1] + sx * tanX * r[1] + syy * tanY * u[1],
        f[2] + sx * tanX * r[2] + syy * tanY * u[2]
    ];
    const l = Math.hypot(d[0], d[1], d[2]) || 1;
    return [d[0] / l, d[1] / l, d[2] / l];
}

function integration(api, check) {
    const c = counter(check);
    const ok = c.ok;

    ok(typeof api.renderPerspective === 'function', 'renderPerspective entry exists');

    // Build the regression scenes: flat, pillar occlusion, steps and a 3x5
    // periodic map viewed from negative coordinates.
    const flat = makeMap(6, 2, 1, () => 0xff3060a0);

    const pillar = makeMap(7, 7, 0, (x, y) => 0xff202030 + ((x * 7 + y) % 30) * 0x010101);
    pillar.altitude[3 + 7 * 3] = 7;
    pillar.color[3 + 7 * 3] = 0xffff4040;

    const steps = makeMap(8, 8, function (x, y) {
        const bx = Math.floor(x / 2), by = Math.floor(y / 2);
        return ((bx + by * 2) % 6);
    }, function (x, y) {
        const h = ((Math.floor(x / 2) + Math.floor(y / 2) * 2) % 6);
        return [0xff202020, 0xff4060a0, 0xff40a060, 0xffa0a040, 0xffa06040, 0xffc0c0c0][h];
    });
    steps.altitude[5 + 8 * 2] = 12;
    steps.color[5 + 8 * 2] = 0xffffffff;

    const small35 = {
        width: 3, height: 5,
        altitude: [1, 0, 2, 0, 3, 1, 2, 1, 0, 1, 2, 3, 0, 1, 2],
        color: [0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff, 0xff000000,
            0xffffff00, 0xffff00ff, 0xff00ffff, 0xff808080, 0xff800000,
            0xff008000, 0xff000080, 0xff808000, 0xff800080, 0xff008080]
    };

    const scenes = [
        {
            name: 'flat-roll', map: flat, W: 14, H: 10,
            camera: { position: [3.37, 1.2, 6.4], yaw: 0.35, pitch: 0.85, roll: 0.55, fovY: 1.1, near: 0, far: 60, background: 0xff112233 }
        },
        {
            name: 'pillar-occlusion-roll', map: pillar, W: 16, H: 11,
            camera: { position: [3.4, -2.6, 3.2], yaw: -0.18, pitch: 0.12, roll: 0.62, fovY: 1.05, near: 0, far: 60, background: 0xff203040 }
        },
        {
            name: 'steps', map: steps, W: 15, H: 10,
            camera: { position: [0.6, 6.7, 5.2], yaw: 0.08, pitch: 0.42, roll: -0.35, fovY: 1.15, near: 0, far: 80, background: 0xff332211 }
        },
        {
            name: 'periodic-3x5-negative', map: small35, W: 13, H: 9,
            camera: { position: [-2.2, -3.4, 6.0], yaw: 0.6, pitch: 0.7, roll: -0.9, fovY: 1.2, near: 0, far: 60, background: 0xff445566 }
        }
    ];

    let totalPixels = 0;
    let depthChecks = 0;
    let colorChecks = 0;

    scenes.forEach(function (scene) {
        const W = scene.W, Hh = scene.H;
        const out = api.renderPerspective(scene.map, scene.camera, W, Hh);

        ok(out && out.color instanceof Uint32Array && out.depth instanceof Float64Array,
            scene.name + ': returns Uint32 color and Float64 depth');
        ok(out.color.length === W * Hh && out.depth.length === W * Hh,
            scene.name + ': buffer lengths match width*height');

        for (let py = 0; py < Hh; py++) {
            for (let px = 0; px < W; px++) {
                const k = px + W * py;
                const d = referenceRay(scene.camera, px, py, W, Hh);
                const ref = bruteTrace(scene.map, scene.camera.position, d,
                    scene.camera.near, scene.camera.far);
                totalPixels++;

                if (ref === null) {
                    ok(out.depth[k] === Infinity, scene.name + ' (' + px + ',' + py + '): reference sky -> Infinity depth');
                    depthChecks++;
                    ok(out.color[k] === (scene.camera.background >>> 0),
                        scene.name + ' (' + px + ',' + py + '): sky uses camera.background');
                    colorChecks++;
                } else {
                    ok(near(out.depth[k], ref.t, 1e-6),
                        scene.name + ' (' + px + ',' + py + '): depth ' + out.depth[k] + ' vs ' + ref.t);
                    depthChecks++;
                    const ccx = mod(ref.cx, scene.map.width);
                    const ccy = mod(ref.cy, scene.map.height);
                    const expectedColor = scene.map.color[ccx + scene.map.width * ccy] >>> 0;
                    ok(out.color[k] === expectedColor,
                        scene.name + ' (' + px + ',' + py + '): color 0x' + out.color[k].toString(16) +
                        ' vs 0x' + expectedColor.toString(16));
                    colorChecks++;
                }
            }
        }
    });

    ok(totalPixels === (14 * 10 + 16 * 11 + 15 * 10 + 13 * 9),
        'reference walked every pixel of all small frames: ' + totalPixels);
    ok(depthChecks === totalPixels && colorChecks === totalPixels,
        'one depth and one color assertion per pixel');

    // Explicit occlusion statement for the tall pillar scene: the unique
    // pillar color must appear, and some pixel must have a finite near hit
    // while rays just beside it reach farther / sky.
    const pillarOut = api.renderPerspective(pillar, scenes[1].camera, scenes[1].W, scenes[1].H);
    let pillarPixels = 0;
    for (let i = 0; i < pillarOut.color.length; i++) {
        if (pillarOut.color[i] === 0xffff4040) pillarPixels++;
    }
    ok(pillarPixels > 0, 'tall pillar is visible in rolled frame (' + pillarPixels + ' px)');

    // Re-render the same scene without the pillar: at least one formerly
    // occluded pixel must change, proving the pillar actually occludes.
    const noPillarMap = {
        width: pillar.width, height: pillar.height,
        altitude: pillar.altitude.slice(), color: pillar.color.slice()
    };
    noPillarMap.altitude[3 + 7 * 3] = 0;
    const noPillarOut = api.renderPerspective(noPillarMap, scenes[1].camera, scenes[1].W, scenes[1].H);
    let occludedChanges = 0;
    for (let i = 0; i < pillarOut.color.length; i++) {
        if (pillarOut.color[i] === 0xffff4040 && noPillarOut.color[i] !== 0xffff4040) occludedChanges++;
    }
    ok(occludedChanges > 0, 'pillar pixels reveal geometry behind when removed (occlusion)');

    return c.n();
}

module.exports = {
    projection: projection,
    traversal: traversal,
    integration: integration
};

// Allow `node test/perspective.js` for direct diagnostics.
if (require.main === module) {
    const assert = require('node:assert/strict');
    const api = require('../terrain-renderer.js');
    for (const name of ['projection', 'traversal', 'integration']) {
        let calls = 0;
        const n = module.exports[name](api, function (condition, message) {
            assert(condition, message);
            calls++;
        });
        assert(Number.isInteger(n) && n === calls && calls >= 5, name + ' count mismatch');
        console.log(name + ' checks=' + calls);
    }
    console.log('PERSPECTIVE_TESTS_PASS');
}
