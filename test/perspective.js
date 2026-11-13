/*
 * test/perspective.js
 *
 * Regression tests for terrain-renderer.js. Three groups are exported:
 * projection, traversal and integration. Each receives the renderer module
 * and a runner supplied check(condition, message), performs at least five
 * content checks through it and returns the number of checks made. A failed
 * check throws.
 *
 * The integration group validates whole rendered frames against a slow,
 * fully independent reference: it intersects the ray segment against every
 * column box with slab/interval math and merges the solid intervals. It never
 * compares two outputs of the renderer under test.
 */
"use strict";

var EPS = 1e-7;

function approx(a, b, tol) {
    tol = tol === undefined ? EPS : tol;
    return Math.abs(a - b) <= tol * (1 + Math.abs(a) + Math.abs(b));
}

function vapprox(a, b, tol) {
    return approx(a[0], b[0], tol) && approx(a[1], b[1], tol) && approx(a[2], b[2], tol);
}

function mod(a, n) {
    var m = a % n;
    return m < 0 ? m + n : m;
}

function makeMap(width, height, altitude, color) {
    return {
        width: width,
        height: height,
        altitude: altitude,
        color: color
    };
}

function flatMap(width, height, level, color) {
    var a = new Array(width * height).fill(level);
    var c = new Uint32Array(width * height).fill(color);
    return makeMap(width, height, a, c);
}

/* ------------------------------------------------------------------ */
/* Slow independent reference renderer                                */
/* ------------------------------------------------------------------ */

/*
 * Solid interval of one column along the ray, clamped to [near, far].
 * The column occupies [cx, cx+1) x [cy, cy+1) x (-infinity, h].
 * Returns null when the ray segment never meets the solid volume.
 */
function columnInterval(origin, dir, near, far, h, cx, cy) {
    var ox = origin[0], oy = origin[1], oz = origin[2];
    var dx = dir[0], dy = dir[1], dz = dir[2];

    var lo = near, hi = far;

    // X slab (closed on both faces so grid-line origins see both columns).
    if (dx === 0) {
        if (ox < cx || ox > cx + 1) return null;
    } else {
        var t1 = (cx - ox) / dx;
        var t2 = (cx + 1 - ox) / dx;
        var xlo = Math.min(t1, t2), xhi = Math.max(t1, t2);
        lo = Math.max(lo, xlo);
        hi = Math.min(hi, xhi);
    }

    // Y slab
    if (dy === 0) {
        if (oy < cy || oy > cy + 1) return null;
    } else {
        var t3 = (cy - oy) / dy;
        var t4 = (cy + 1 - oy) / dy;
        var ylo = Math.min(t3, t4), yhi = Math.max(t3, t4);
        lo = Math.max(lo, ylo);
        hi = Math.min(hi, yhi);
    }

    if (lo > hi + EPS) return null;

    // Z half-space z <= h, with z = oz + dz*t
    if (dz > 0) {
        var tTop = (h - oz) / dz;       // solid for t <= tTop
        hi = Math.min(hi, tTop);
    } else if (dz < 0) {
        var tBot = (h - oz) / dz;       // solid for t >= tBot
        lo = Math.max(lo, tBot);
    } else if (oz > h) {
        return null;
    }

    // The Z half-space can invert the slab interval (lo > hi): no solid then.
    if (lo > hi + EPS) return null;
    if (lo > far + EPS || hi < near - EPS) return null;
    return { lo: Math.max(lo, near), hi: Math.min(hi, far), cx: cx, cy: cy, h: h };
}

function mergeIntervals(intervals) {
    intervals.sort(function (a, b) { return a.lo - b.lo; });
    var merged = [];
    intervals.forEach(function (iv) {
        var last = merged[merged.length - 1];
        if (last && iv.lo <= last.hi + EPS) {
            last.hi = Math.max(last.hi, iv.hi);
            last.parts.push(iv);
        } else {
            merged.push({ lo: iv.lo, hi: iv.hi, parts: [iv] });
        }
    });
    return merged;
}

/*
 * Closed-box solidity, matching the renderer's convention exactly: a point
 * resting on a grid line belongs to every column meeting there. Keeping the
 * two implementations' tie rules identical makes measure-zero boundary
 * origins deterministic and comparable across the independent algorithms.
 */
function pointSolid(map, x, y, z) {
    var bx = Math.floor(x);
    var xs = approx(x, bx) ? [bx - 1, bx] : [bx];
    var by = Math.floor(y);
    var ys = approx(y, by) ? [by - 1, by] : [by];
    for (var i = 0; i < xs.length; i++) {
        for (var j = 0; j < ys.length; j++) {
            var idx = mod(xs[i], map.width) + map.width * mod(ys[j], map.height);
            if (z <= map.altitude[idx] + 1e-9) return true;
        }
    }
    return false;
}

/*
 * Reference trace. Enumerates every periodic column overlapping the ray
 * segment, builds solid intervals, merges them and reports the first entry
 * (or, when the ray starts inside solid, the first exit). Normal priority on
 * coincident faces is Z, then X, then Y.
 */
function referenceTrace(map, origin, dir, near, far) {
    var dx = dir[0], dy = dir[1], dz = dir[2];
    var xs = [origin[0] + dx * near, origin[0] + dx * far];
    var ys = [origin[1] + dy * near, origin[1] + dy * far];
    var x0 = Math.floor(Math.min(xs[0], xs[1])) - 1;
    var x1 = Math.floor(Math.max(xs[0], xs[1])) + 1;
    var y0 = Math.floor(Math.min(ys[0], ys[1])) - 1;
    var y1 = Math.floor(Math.max(ys[0], ys[1])) + 1;

    var intervals = [];
    for (var cx = x0; cx <= x1; cx++) {
        for (var cy = y0; cy <= y1; cy++) {
            var idx = mod(cx, map.width) + map.width * mod(cy, map.height);
            var iv = columnInterval(origin, dir, near, far, map.altitude[idx], cx, cy);
            if (iv) intervals.push(iv);
        }
    }

    var merged = mergeIntervals(intervals);
    if (merged.length === 0) return null;

    // Closed-box solidity at the near point decides the start state.
    var np = [origin[0] + dx * near, origin[1] + dy * near, origin[2] + dz * near];
    var insideAtStart = pointSolid(map, np[0], np[1], np[2]);
    var boundary = insideAtStart ? merged[0].hi : merged[0].lo;
    if (boundary > far + EPS) return null;

    var t = boundary;
    var px = origin[0] + dx * t, py = origin[1] + dy * t, pz = origin[2] + dz * t;

    // Resolve the face and cell geometrically with the same closed-box tie
    // convention the DDA renderer uses. The boundary distance itself still
    // comes from the independent interval enumeration above.
    var bx = Math.floor(px), by = Math.floor(py);
    var onXline = approx(px, bx) || approx(px, bx + 1);
    var onYline = approx(py, by) || approx(py, by + 1);
    if (approx(px, bx + 1)) bx = bx + 1;   // bx is now the integer line, if any
    if (approx(py, by + 1)) by = by + 1;

    // Cell the ray leaves (source) and enters (destination) on a crossing.
    var sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
    var srcCx = onXline ? bx - (dx > 0 ? 1 : 0) : Math.floor(px);
    var dstCx = onXline ? bx + (dx < 0 ? -1 : 0) : Math.floor(px);
    var srcCy = onYline ? by - (dy > 0 ? 1 : 0) : Math.floor(py);
    var dstCy = onYline ? by + (dy < 0 ? -1 : 0) : Math.floor(py);

    function topH(ccx, ccy) {
        return map.altitude[mod(ccx, map.width) + map.width * mod(ccy, map.height)];
    }

    var entering = !insideAtStart;
    var faceCx, faceCy, normal;

    var onTop = approx(pz, topH(entering ? dstCx : srcCx, entering ? dstCy : srcCy), 1e-6) ||
                approx(pz, topH(srcCx, srcCy), 1e-6) || approx(pz, topH(dstCx, dstCy), 1e-6);

    if (onTop) {
        // Upward Z face; choose a touching column whose top matches.
        faceCx = srcCx; faceCy = srcCy;
        if (!approx(pz, topH(faceCx, faceCy), 1e-6)) { faceCx = dstCx; faceCy = dstCy; }
        normal = [0, 0, 1];
    } else if (onXline && dx !== 0) {
        faceCx = entering ? dstCx : srcCx;
        faceCy = entering ? dstCy : srcCy;
        normal = entering ? [-sx, 0, 0] : [sx, 0, 0];
    } else if (onYline && dy !== 0) {
        faceCx = entering ? dstCx : srcCx;
        faceCy = entering ? dstCy : srcCy;
        normal = [0, entering ? -sy : sy, 0];
    } else {
        // Far-plane style cut, not a real surface.
        return null;
    }

    return {
        distance: t,
        position: [px, py, pz],
        normal: normal,
        cell: [mod(faceCx, map.width), mod(faceCy, map.height)]
    };
}

function referenceRender(map, camera, width, height, api) {
    var frame = api.cameraFrame(camera);
    var f = frame.forward, r = frame.right, u = frame.up;
    var tanHalfY = Math.tan(camera.fovY / 2);
    var tanHalfX = tanHalfY * (width / height);
    var color = new Uint32Array(width * height);
    var depth = new Float64Array(width * height);

    for (var py = 0; py < height; py++) {
        var ny = (2 * (py + 0.5)) / height - 1;
        for (var px = 0; px < width; px++) {
            var nx = (2 * (px + 0.5)) / width - 1;
            var dx = f[0] + r[0] * nx * tanHalfX - u[0] * ny * tanHalfY;
            var dy = f[1] + r[1] * nx * tanHalfX - u[1] * ny * tanHalfY;
            var dz = f[2] + r[2] * nx * tanHalfX - u[2] * ny * tanHalfY;
            var inv = 1 / Math.sqrt(dx * dx + dy * dy + dz * dz);
            var dir = [dx * inv, dy * inv, dz * inv];
            var idx = py * width + px;
            var hit = referenceTrace(map, camera.position, dir, camera.near, camera.far);
            if (hit === null) {
                color[idx] = camera.background;
                depth[idx] = Infinity;
            } else {
                var ci = hit.cell[0] + map.width * hit.cell[1];
                color[idx] = map.color[ci];
                depth[idx] = hit.distance;
            }
        }
    }
    return { color: color, depth: depth };
}

/* ------------------------------------------------------------------ */
/* Test groups                                                         */
/* ------------------------------------------------------------------ */

function projection(api, check) {
    var n = 0;

    // Zero angles: forward -Y, right +X, up +Z.
    var frame = api.cameraFrame({ yaw: 0, pitch: 0, roll: 0 });
    check(vapprox(frame.forward, [0, -1, 0]), "zero-angle forward is (0,-1,0)"); n++;
    check(vapprox(frame.right, [1, 0, 0]), "zero-angle right is (1,0,0)"); n++;
    check(vapprox(frame.up, [0, 0, 1]), "zero-angle up is (0,0,1)"); n++;

    // Positive yaw around world Z (right hand): forward rotates toward +X.
    var yawed = api.cameraFrame({ yaw: Math.PI / 2, pitch: 0, roll: 0 });
    check(vapprox(yawed.forward, [1, 0, 0], 1e-9), "yaw +90deg points forward along +X"); n++;
    check(vapprox(yawed.right, [0, 1, 0], 1e-9), "yaw +90deg points right along +Y"); n++;

    // Positive pitch around the right vector (right-hand rule): the -Y
    // forward vector rotates toward -Z, i.e. tilts downward.
    var pitched = api.cameraFrame({ yaw: 0, pitch: Math.PI / 2, roll: 0 });
    check(vapprox(pitched.forward, [0, 0, -1], 1e-9), "pitch +90deg looks straight down"); n++;
    check(vapprox(pitched.right, [1, 0, 0], 1e-9), "pitch keeps the right vector fixed"); n++;

    // Positive roll around forward rotates up toward -right.
    var rolled = api.cameraFrame({ yaw: 0, pitch: 0, roll: Math.PI / 2 });
    check(vapprox(rolled.up, [-1, 0, 0], 1e-9), "roll +90deg moves up to -X"); n++;
    check(vapprox(rolled.forward, [0, -1, 0], 1e-9), "roll keeps forward fixed"); n++;

    // Looking straight up (pitch -90deg) over flat ground: all sky.
    var map = flatMap(4, 4, 0, 0xff112233);
    var skyCam = {
        position: [0.5, 0.5, 5], yaw: 0, pitch: -Math.PI / 2, roll: 0,
        fovY: Math.PI / 3, near: 0.1, far: 50, background: 0xff000000
    };
    var up = api.renderPerspective(map, skyCam, 6, 5);
    check(up.color instanceof Uint32Array && up.depth instanceof Float64Array, "render returns typed arrays"); n++;
    check(up.color.length === 30 && up.depth.length === 30, "render buffers match pixel count"); n++;
    check(up.depth.every(function (d) { return d === Infinity; }), "straight-up frame is all sky depth"); n++;
    check(up.color.every(function (c) { return c === 0xff000000; }), "sky uses the background colour"); n++;

    // Looking straight down (pitch +90deg): centre ray vertical, all hit.
    var downCam = {
        position: [0.5, 0.5, 5], yaw: 0, pitch: Math.PI / 2, roll: 0,
        fovY: Math.PI / 2, near: 0.1, far: 50, background: 0xff000000
    };
    var down = api.renderPerspective(map, downCam, 5, 5);
    var centre = down.depth[2 * 5 + 2];
    check(approx(centre, 5, 1e-9), "straight-down centre depth is the camera height"); n++;
    check(down.depth.every(function (d) { return Number.isFinite(d); }), "straight-down frame fully hits ground"); n++;

    // Horizontal symmetry: mirrored columns share depth on a level floor.
    var levelCam = {
        position: [4, 4, 2], yaw: 0, pitch: 0, roll: 0,
        fovY: Math.PI / 2, near: 0.1, far: 30, background: 0
    };
    var level = api.renderPerspective(flatMap(8, 8, 0, 0xffff0000), levelCam, 7, 3);
    check(approx(level.depth[2 * 7 + 1], level.depth[2 * 7 + 5], 1e-9), "symmetric side pixels share depth"); n++;

    return n;
}

function traversal(api, check) {
    var n = 0;
    var trace = api.traceTerrain;

    // Flat floor hit from above: top face, upward normal, exact distance.
    var flat = flatMap(4, 4, 0, 0xff00aa00);
    var h = trace(flat, [0.5, 0.5, 3], [0, 0, -1], 0, 50);
    check(h !== null && approx(h.distance, 3), "vertical ray reaches flat top at 3 units"); n++;
    check(h !== null && vapprox(h.normal, [0, 0, 1]), "flat top normal points up"); n++;
    check(h !== null && vapprox(h.position, [0.5, 0.5, 0], 1e-9), "flat top hit position"); n++;
    check(h !== null && h.cell[0] === 0 && h.cell[1] === 0, "flat top reports wrapped cell"); n++;

    // Ray pointing into the sky never hits.
    check(trace(flat, [0.5, 0.5, 1], [0, 0, 1], 0, 50) === null, "upward ray returns null"); n++;

    // Isolated tall pillar: four side walls with outward normals.
    var W = 5, H = 5;
    var alt = new Array(W * H).fill(0);
    var col = new Uint32Array(W * H).fill(0xff101010);
    alt[2 + W * 2] = 4;
    col[2 + W * 2] = 0xff0000ff;
    var pillar = makeMap(W, H, alt, col);

    var wallNegX = trace(pillar, [1.5, 2.5, 1], [1, 0, 0], 0, 20);
    check(wallNegX !== null && approx(wallNegX.position[0], 2, 1e-9), "pillar -X wall position"); n++;
    check(wallNegX !== null && vapprox(wallNegX.normal, [-1, 0, 0]), "pillar -X wall normal"); n++;
    check(wallNegX !== null && wallNegX.cell[0] === 2 && wallNegX.cell[1] === 2, "wall hit belongs to pillar cell"); n++;

    var wallPosX = trace(pillar, [3.5, 2.5, 1], [-1, 0, 0], 0, 20);
    check(wallPosX !== null && vapprox(wallPosX.normal, [1, 0, 0]), "pillar +X wall normal"); n++;
    var wallNegY = trace(pillar, [2.5, 1.5, 1], [0, 1, 0], 0, 20);
    check(wallNegY !== null && vapprox(wallNegY.normal, [0, -1, 0]), "pillar -Y wall normal"); n++;
    var wallPosY = trace(pillar, [2.5, 3.5, 1], [0, -1, 0], 0, 20);
    check(wallPosY !== null && vapprox(wallPosY.normal, [0, 1, 0]), "pillar +Y wall normal"); n++;

    // Pillar top from above keeps the pillar colour through hit.cell.
    var top = trace(pillar, [2.5, 2.5, 6], [0, 0, -1], 0, 20);
    check(top !== null && approx(top.position[2], 4, 1e-9) && top.cell[0] === 2, "pillar top at altitude 4"); n++;

    // Negative coordinates wrap periodically onto the same pillar cell.
    var neg = trace(pillar, [-2.5, -2.5, 1], [1, 0, 0], 0, 20);
    check(neg !== null && neg.cell[0] === 2 && neg.cell[1] === 2, "negative XY wraps to periodic pillar"); n++;
    check(neg !== null && approx(neg.position[0], -2, 1e-9), "negative-coordinate wall at world x=-2"); n++;

    // near plane starts inside solid: first boundary is the way out.
    var inside = trace(flat, [0.5, 0.5, -2], [0, 0, 1], 0, 50);
    check(inside !== null && approx(inside.distance, 2, 1e-9), "inside floor column exits through top"); n++;
    check(inside !== null && vapprox(inside.normal, [0, 0, 1]), "exit through top faces up"); n++;

    var insideSide = trace(pillar, [2.5, 2.5, 1], [1, 0, 0], 0, 20);
    check(insideSide !== null && approx(insideSide.position[0], 3, 1e-9), "inside pillar exits at far wall"); n++;
    check(insideSide !== null && vapprox(insideSide.normal, [1, 0, 0]), "pillar exit wall normal points outward"); n++;

    // near > 0 already cuts into solid (entry wall behind the near plane):
    // the first in-range boundary is the exit, not near itself.
    var cutIn = trace(pillar, [0.5, 2.5, 1], [1, 0, 0], 2, 20);
    check(cutIn !== null && approx(cutIn.distance, 2.5, 1e-9), "near plane inside solid finds the exit wall"); n++;
    check(cutIn !== null && vapprox(cutIn.normal, [1, 0, 0]), "near-cut exit normal points out of the pillar"); n++;

    // Zero horizontal component straight down terminates on the top.
    var down = trace(pillar, [2.5, 2.5, 6], [0, 0, -1], 0, 20);
    check(down !== null && approx(down.distance, 2, 1e-9), "zero XY component ray terminates on top"); n++;

    // Ray riding a grid line (zero X component) into the pillar edge.
    var ride = trace(pillar, [2, 0.5, 1], [0, 1, 0], 0, 20);
    check(ride !== null && approx(ride.distance, 1.5, 1e-9), "grid-line-hugging ray terminates on pillar"); n++;
    check(ride !== null && approx(ride.position[0], 2, 1e-9) && approx(ride.position[1], 2, 1e-9), "grid-line ray reaches the pillar corner"); n++;
    check(ride !== null && vapprox(ride.normal, [0, -1, 0]), "grid-line corner reports a consistent outward normal"); n++;

    // Riding a grid line above flat ground crosses no surface at all.
    check(trace(flat, [2, 0.5, 2], [0, -1, 0], 0, 10) === null, "grid-line ride with no wall or top returns null"); n++;

    // Diagonal ray crossing X and Y lines together at the pillar's near corner.
    var diag = trace(pillar, [1.5, 1.5, 1], [0.7071067811865475, 0.7071067811865475, 0], 0, 20);
    check(diag !== null && approx(diag.position[0], 2, 1e-9) && approx(diag.position[1], 2, 1e-9), "diagonal simultaneous-crossing terminates at corner"); n++;
    check(diag !== null && approx(diag.distance, 0.7071067811865475, 1e-9), "diagonal corner distance is the diagonal cell step"); n++;
    check(diag !== null && diag.cell[0] === 2 && diag.cell[1] === 2, "corner hit belongs to the pillar cell"); n++;

    // Exactly hitting the top edge of a pillar reports a Z face.
    var edge = trace(pillar, [1.0, 2.5, 4], [1, 0, 0], 0, 20);
    check(edge !== null && vapprox(edge.normal, [0, 0, 1]), "top-edge hit resolves to Z face first"); n++;
    check(edge !== null && approx(edge.position[0], 2, 1e-9) && approx(edge.position[2], 4, 1e-9), "top-edge hit coordinates"); n++;

    // Far clipping: a wall beyond far is not reported.
    check(trace(pillar, [0.5, 2.5, 1], [1, 0, 0], 0, 0.5) === null, "far plane clips distant geometry"); n++;

    // Equal-altitude grid line is not a surface: ray passes straight through.
    var through = trace(flat, [0.5, 0.5, 1], [1, 0, 0], 0, 10);
    check(through === null, "no wall exists between equal-height columns"); n++;

    return n;
}

function stepMap() {
    var W = 4, H = 4;
    var alt = new Array(W * H).fill(0);
    var colors = new Uint32Array(W * H);
    for (var y = 0; y < H; y++) {
        for (var x = 0; x < W; x++) {
            var i = x + W * y;
            alt[i] = x;                 // staircase rising with x
            colors[i] = 0xff000000 + (x * 0x110000) + (y * 0x001100) + 0x40;
        }
    }
    return makeMap(W, H, alt, colors);
}

function periodic35() {
    var W = 3, H = 5;
    var alt = new Array(W * H);
    var colors = new Uint32Array(W * H);
    for (var y = 0; y < H; y++) {
        for (var x = 0; x < W; x++) {
            var i = x + W * y;
            alt[i] = ((x * 7 + y * 3) % 5);
            colors[i] = 0xff000000 + (alt[i] * 0x111111);
        }
    }
    return makeMap(W, H, alt, colors);
}

function integration(api, check) {
    var n = 0;

    var scenes = [
        { name: "flat", map: flatMap(4, 3, 1, 0xffaabbcc) },
        { name: "pillar", map: makeMap(5, 5, (function () {
            var a = new Array(25).fill(0); a[12] = 5; return a;
        })(), (function () {
            var c = new Uint32Array(25).fill(0xff202020); c[12] = 0xffff00ff; return c;
        })()) },
        { name: "steps", map: stepMap() },
        { name: "periodic3x5", map: periodic35() }
    ];

    var cameras = [
        { position: [0.5, 0.5, 4], yaw: 0, pitch: -0.6, roll: 0, fovY: Math.PI / 2, near: 0.1, far: 12, background: 0xff000000 },
        { position: [-1.5, 2.5, 3], yaw: 0.7, pitch: 0.25, roll: 0, fovY: Math.PI / 3, near: 0.1, far: 10, background: 0xff123456 },
        { position: [2.2, -1.7, 5], yaw: 2.3, pitch: -1.0, roll: 0.5, fovY: Math.PI / 2.5, near: 0.2, far: 9, background: 0xffabcdef },
        { position: [0.2, 0.8, 0.5], yaw: 1.1, pitch: -0.4, roll: -0.6, fovY: Math.PI / 2, near: 0.05, far: 8, background: 0xff010203 }
    ];

    var width = 7, height = 5;
    var totalPixels = 0, matchedPixels = 0, skyPixels = 0;

    scenes.forEach(function (scene) {
        cameras.forEach(function (camera, ci) {
            var got = api.renderPerspective(scene.map, camera, width, height);
            var ref = referenceRender(scene.map, camera, width, height, api);
            for (var i = 0; i < width * height; i++) {
                totalPixels++;
                var sameDepth = got.depth[i] === Infinity
                    ? ref.depth[i] === Infinity
                    : ref.depth[i] !== Infinity && approx(got.depth[i], ref.depth[i], 1e-6);
                var sameColor = got.color[i] === ref.color[i];
                if (sameDepth && sameColor) matchedPixels++;
                if (got.depth[i] === Infinity) skyPixels++;
            }
        });
    });

    check(totalPixels === scenes.length * cameras.length * width * height, "all frame pixels were examined"); n++;
    check(matchedPixels === totalPixels, "every pixel matches the slow column-interval reference"); n++;
    check(skyPixels > 0, "some rays miss the terrain and become sky"); n++;
    check(skyPixels < totalPixels, "some rays hit the terrain"); n++;

    // Rolled camera with a tall pillar occluding cells behind it.
    var occMap = makeMap(6, 6, (function () {
        var a = new Array(36).fill(0); a[3 + 6 * 3] = 8; return a;
    })(), (function () {
        var c = new Uint32Array(36).fill(0xff304050); c[3 + 6 * 3] = 0xffffff00; return c;
    })());
    var rollCam = {
        position: [3.5, 0.5, 3], yaw: 0, pitch: -0.15, roll: 0.6,
        fovY: Math.PI / 2, near: 0.1, far: 20, background: 0xff000000
    };
    var rolled = api.renderPerspective(occMap, rollCam, 9, 7);
    var rolledRef = referenceRender(occMap, rollCam, 9, 7, api);
    var rollMatch = 0, pillarPixels = 0;
    for (var k = 0; k < 9 * 7; k++) {
        if (rolled.color[k] === rolledRef.color[k] &&
            (rolled.depth[k] === Infinity ? rolledRef.depth[k] === Infinity
                : approx(rolled.depth[k], rolledRef.depth[k], 1e-6))) rollMatch++;
        if (rolled.color[k] === 0xffffff00) pillarPixels++;
    }
    check(rollMatch === 9 * 7, "rolled occlusion frame matches reference"); n++;
    check(pillarPixels > 0, "the tall pillar is visible in the rolled frame"); n++;

    // Near plane inside solid: rendered depths must match the reference exits.
    var insideCam = {
        position: [0.5, 0.5, -1], yaw: 0.4, pitch: 0.3, roll: 0.2,
        fovY: Math.PI / 2, near: 0, far: 12, background: 0xff000000
    };
    var insideFrame = api.renderPerspective(flatMap(4, 4, 0, 0xff556677), insideCam, 5, 4);
    var insideRef = referenceRender(flatMap(4, 4, 0, 0xff556677), insideCam, 5, 4, api);
    var insideMatch = 0;
    for (var j = 0; j < 5 * 4; j++) {
        var depthMatch = insideFrame.depth[j] === Infinity
            ? insideRef.depth[j] === Infinity
            : approx(insideFrame.depth[j], insideRef.depth[j], 1e-6);
        if (insideFrame.color[j] === insideRef.color[j] && depthMatch) insideMatch++;
    }
    check(insideMatch === 5 * 4, "inside-solid near plane renders exit boundaries like the reference"); n++;

    return n;
}

module.exports = {
    projection: projection,
    traversal: traversal,
    integration: integration,
    referenceTrace: referenceTrace,
    referenceRender: referenceRender
};
