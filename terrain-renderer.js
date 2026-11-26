/*!
 * terrain-renderer.js
 *
 * Perspective ray tracer for the VoxelSpace column height field.
 *
 * Works as a browser global (`TerrainRenderer`) and as a CommonJS module.
 * Only JavaScript built-ins are used.
 *
 * Map convention
 * --------------
 * `map = { width, height, altitude, color }`.
 * `width`/`height` are arbitrary positive integers and cells are stored at
 * index `x + width * y`. Every integer XY cell is a solid column stretching
 * from -infinity up to `altitude[index]`; heights are never interpolated.
 * The XY grid repeats periodically, so negative coordinates wrap with a
 * proper positive modulo and address the same `width*height` entries.
 *
 * Public entry points
 * -------------------
 * traceTerrain(map, origin, direction, near, far)
 * renderPerspective(map, camera, width, height)
 */
(function (root, factory) {
    const api = factory();
    if (typeof module === 'object' && module.exports) {
        module.exports = api;
    }
    if (root) {
        root.TerrainRenderer = api;
    }
})(typeof globalThis !== 'undefined' ? globalThis : (typeof self !== 'undefined' ? self : this), function () {
    'use strict';

    // Geometry comparison tolerance. Cell coordinates are unit sized.
    const GEOM_EPS = 1e-9;

    // Proper positive modulo for periodic negative coordinates.
    function mod(n, m) {
        return ((n % m) + m) % m;
    }

    function read3(p) {
        if (Array.isArray(p)) {
            return [p[0], p[1], p[2]];
        }
        if (p == null) {
            return [0, 0, 0];
        }
        return [p.x, p.y, p.z];
    }

    // If v is within GEOM_EPS of an integer return that integer, else null.
    function nearInteger(v) {
        const r = Math.round(v);
        return Math.abs(v - r) <= GEOM_EPS ? r : null;
    }

    // Attach non-enumerable x/y/z aliases so the same vector behaves as both
    // an [x, y, z] tuple and an {x, y, z} object without breaking deep equals
    // against plain arrays.
    function withAxes(vec) {
        Object.defineProperty(vec, 'x', { value: vec[0], enumerable: false, configurable: true, writable: true });
        Object.defineProperty(vec, 'y', { value: vec[1], enumerable: false, configurable: true, writable: true });
        Object.defineProperty(vec, 'z', { value: vec[2], enumerable: false, configurable: true, writable: true });
        return vec;
    }

    function makeHitFactory(map, ox, oy, oz, dx, dy, dz) {
        return function makeHit(t, nx, ny, nz, rawCellX, rawCellY) {
            const mw = map.width | 0;
            const mh = map.height | 0;
            const cx = mod(rawCellX, mw);
            const cy = mod(rawCellY, mh);
            const cell = [cx, cy];
            Object.defineProperty(cell, 'x', { value: cx, enumerable: false, configurable: true });
            Object.defineProperty(cell, 'y', { value: cy, enumerable: false, configurable: true });

            const position = withAxes([ox + dx * t, oy + dy * t, oz + dz * t]);
            const normal = withAxes([nx, ny, nz]);

            const hit = {
                distance: t,
                position: position,
                normal: normal,
                cell: cell
            };

            // Convenience: the color of the struck periodic column. Kept
            // non-enumerable so the documented {distance,position,normal,cell}
            // shape stays exactly enumerable.
            let color;
            if (map.color) {
                color = map.color[cx + mw * cy];
            }
            Object.defineProperty(hit, 'color', {
                value: color,
                enumerable: false,
                configurable: true,
                writable: true
            });
            return hit;
        };
    }

    function computeMaxAltitude(map, mw, mh) {
        const alt = map.altitude;
        let maxH = -Infinity;
        const n = mw * mh;
        for (let i = 0; i < n; i++) {
            const h = +alt[i];
            if (h > maxH) {
                maxH = h;
            }
        }
        return maxH;
    }

    // First boundary of the solid union along the unit ray, with all the
    // context scalarised. Returned object follows the public hit shape.
    function traceCore(map, origin, direction, near, far, maxH) {
        const mw = Math.max(1, map.width | 0);
        const mh = Math.max(1, map.height | 0);
        const alt = map.altitude;

        const o = read3(origin);
        const d = read3(direction);
        const ox = +o[0], oy = +o[1], oz = +o[2];
        const dx = +d[0], dy = +d[1], dz = +d[2];

        let tNear = near == null ? 0 : +near;
        let tFar = far == null ? Infinity : +far;
        if (Number.isNaN(tNear)) {
            tNear = 0;
        }
        if (Number.isNaN(tFar)) {
            tFar = Infinity;
        }
        if (tNear > tFar + GEOM_EPS) {
            return null;
        }

        if (maxH == null || Number.isNaN(maxH)) {
            maxH = computeMaxAltitude(map, mw, mh);
        }

        const makeHit = makeHitFactory(map, ox, oy, oz, dx, dy, dz);

        function heightAt(cx, cy) {
            const ix = mod(cx, mw);
            const iy = mod(cy, mh);
            return +alt[ix + mw * iy];
        }

        // Evaluate every face passing through the point at ray time t.
        // Priority at equal distance: Z face, then X face, then Y face.
        // Returns a hit object or null.
        function faceAt(t) {
            const x = ox + dx * t;
            const y = oy + dy * t;
            const z = oz + dz * t;

            const kx = nearInteger(x);
            const ky = nearInteger(y);

            // Integer cells whose closed XY square contains (x, y).
            const xA = kx !== null ? kx - 1 : Math.floor(x);
            const xB = kx !== null ? kx : xA;
            const yA = ky !== null ? ky - 1 : Math.floor(y);
            const yB = ky !== null ? ky : yA;

            // ---- Z faces (highest tie priority) ---------------------------
            for (let cx = xA; cx <= xB; cx++) {
                for (let cy = yA; cy <= yB; cy++) {
                    const h = heightAt(cx, cy);
                    if (Math.abs(z - h) <= GEOM_EPS) {
                        return makeHit(t, 0, 0, 1, cx, cy);
                    }
                }
            }

            // ---- X faces --------------------------------------------------
            if (kx !== null) {
                for (let cy = yA; cy <= yB; cy++) {
                    const hL = heightAt(kx - 1, cy); // cell on the -X side
                    const hR = heightAt(kx, cy);     // cell on the +X side
                    if (hL !== hR) {
                        const hi = Math.max(hL, hR);
                        const lo = Math.min(hL, hR);
                        if (z <= hi + GEOM_EPS && z >= lo - GEOM_EPS) {
                            if (hL > hR) {
                                // Left column owns the wall: outward normal +X
                                return makeHit(t, 1, 0, 0, kx - 1, cy);
                            }
                            // Right column owns the wall: outward normal -X
                            return makeHit(t, -1, 0, 0, kx, cy);
                        }
                    }
                }
            }

            // ---- Y faces (lowest tie priority) ----------------------------
            if (ky !== null) {
                for (let cx = xA; cx <= xB; cx++) {
                    const hD = heightAt(cx, ky - 1); // cell on the -Y side
                    const hU = heightAt(cx, ky);     // cell on the +Y side
                    if (hD !== hU) {
                        const hi = Math.max(hD, hU);
                        const lo = Math.min(hD, hU);
                        if (z <= hi + GEOM_EPS && z >= lo - GEOM_EPS) {
                            if (hD > hU) {
                                // Lower-Y column owns the wall: normal +Y
                                return makeHit(t, 0, 1, 0, cx, ky - 1);
                            }
                            // Upper-Y column owns the wall: normal -Y
                            return makeHit(t, 0, -1, 0, cx, ky);
                        }
                    }
                }
            }

            return null;
        }

        // True once the ray is above every column and non-descending.
        function aboveAll(t) {
            return dz >= 0 && (oz + dz * t) > maxH + GEOM_EPS;
        }

        // Evaluate the closed interval start: a boundary exactly on `near`
        // is a valid hit. Strictly interior points yield no face and the
        // march continues towards the first boundary leaving the solid.
        const startHit = faceAt(tNear);
        if (startHit) {
            return startHit;
        }
        if (aboveAll(tNear)) {
            return null;
        }

        // Axis crossing stepper for XY grid lines. `b` is the next integer
        // boundary to cross and `at` its absolute ray time.
        function makeAxis(originCoord, comp, t0) {
            if (comp === 0) {
                return { d: 0, sign: 0, b: 0, at: Infinity };
            }
            const p = originCoord + comp * t0;
            let b;
            const k = nearInteger(p);
            if (comp > 0) {
                b = k !== null ? k + 1 : Math.floor(p) + 1;
            } else {
                b = k !== null ? k - 1 : Math.floor(p);
            }
            let at = (b - originCoord) / comp;
            let guard = 0;
            while (!(at > t0) && guard++ < 8) {
                b += comp > 0 ? 1 : -1;
                at = (b - originCoord) / comp;
            }
            return { d: comp, sign: comp > 0 ? 1 : -1, b: b, at: at };
        }

        function advanceAxis(a, originCoord, tNow) {
            a.b += a.sign;
            let at = (a.b - originCoord) / a.d;
            let guard = 0;
            while (!(at > tNow) && guard++ < 8) {
                a.b += a.sign;
                at = (a.b - originCoord) / a.d;
            }
            a.at = at;
        }

        const ax = makeAxis(ox, dx, tNear);
        const ay = makeAxis(oy, dy, tNear);

        // Integer XY cells describing the open interval immediately after t.
        function futureXCells(t) {
            if (dx === 0) {
                const x = ox; // dx == 0 means x is constant from the origin
                const k = nearInteger(x);
                if (k !== null) {
                    return [k - 1, k]; // travelling exactly along an X line
                }
                return [Math.floor(x)];
            }
            const x = ox + dx * t;
            const k = nearInteger(x);
            if (k !== null) {
                return dx > 0 ? [k] : [k - 1];
            }
            return [Math.floor(x)];
        }

        function futureYCells(t) {
            if (dy === 0) {
                const y = oy;
                const k = nearInteger(y);
                if (k !== null) {
                    return [k - 1, k];
                }
                return [Math.floor(y)];
            }
            const y = oy + dy * t;
            const k = nearInteger(y);
            if (k !== null) {
                return dy > 0 ? [k] : [k - 1];
            }
            return [Math.floor(y)];
        }

        // Unit direction => XY line events per distance are bounded; this is
        // only a safety net so degenerate periodic rays always terminate.
        const span = Number.isFinite(tFar) ? Math.max(0, tFar - tNear) : 0;
        const maxIter = Number.isFinite(tFar)
            ? Math.ceil(16 * span + 64)
            : 200000;

        let t = tNear;
        let iter = 0;

        while (iter++ < maxIter) {
            const tXY = Math.min(ax.at, ay.at);
            let tNext = tXY;

            // Top-plane (Z) events inside the current XY interval.
            if (dz !== 0) {
                const xs = futureXCells(t);
                const ys = futureYCells(t);
                for (let xi = 0; xi < xs.length; xi++) {
                    for (let yi = 0; yi < ys.length; yi++) {
                        const cx = xs[xi];
                        const cy = ys[yi];
                        const h = heightAt(cx, cy);
                        const tz = (h - oz) / dz;
                        if (tz > t && tz <= tXY && tz < tNext) {
                            tNext = tz;
                        }
                    }
                }
            }

            if (!Number.isFinite(tNext)) {
                return null;
            }
            if (tNext > tFar + GEOM_EPS) {
                return null;
            }

            const hit = faceAt(tNext);
            if (hit) {
                return hit;
            }

            // Step any axis whose grid line this event sits on.
            if (ax.d !== 0) {
                const px = ox + dx * tNext;
                if (ax.at <= tNext || Math.abs(px - ax.b) <= GEOM_EPS) {
                    advanceAxis(ax, ox, tNext);
                }
            }
            if (ay.d !== 0) {
                const py = oy + dy * tNext;
                if (ay.at <= tNext || Math.abs(py - ay.b) <= GEOM_EPS) {
                    advanceAxis(ay, oy, tNext);
                }
            }

            t = tNext;
            if (aboveAll(t)) {
                return null;
            }
        }

        return null;
    }

    function traceTerrain(map, origin, direction, near, far) {
        if (!map || !map.altitude) {
            return null;
        }
        return traceCore(map, origin, direction, near, far, undefined);
    }

    // ---------------------------------------------------------------------
    // Camera / projection
    // ---------------------------------------------------------------------

    function rotateZ(v, a) {
        const c = Math.cos(a);
        const s = Math.sin(a);
        return [
            c * v[0] - s * v[1],
            s * v[0] + c * v[1],
            v[2]
        ];
    }

    // Right-hand positive rotation of v around unit axis k.
    function rotateAxis(v, k, a) {
        const c = Math.cos(a);
        const s = Math.sin(a);
        const oc = 1 - c;
        return [
            v[0] * (c + k[0] * k[0] * oc) +
                v[1] * (k[0] * k[1] * oc - k[2] * s) +
                v[2] * (k[0] * k[2] * oc + k[1] * s),
            v[0] * (k[1] * k[0] * oc + k[2] * s) +
                v[1] * (c + k[1] * k[1] * oc) +
                v[2] * (k[1] * k[2] * oc - k[0] * s),
            v[0] * (k[2] * k[0] * oc - k[1] * s) +
                v[1] * (k[2] * k[1] * oc + k[0] * s) +
                v[2] * (c + k[2] * k[2] * oc)
        ];
    }

    function cross(a, b) {
        return [
            a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]
        ];
    }

    function normalize(v) {
        const l = Math.hypot(v[0], v[1], v[2]) || 1;
        return [v[0] / l, v[1] / l, v[2] / l];
    }

    // Build {forward, right, up} from the documented rotation convention:
    // yaw around world Z first, pitch around the yawed right vector, roll
    // around the final forward vector. All angles are right-hand positive.
    function cameraBasis(camera) {
        camera = camera || {};
        const yaw = +(camera.yaw || 0);
        const pitch = +(camera.pitch || 0);
        const roll = +(camera.roll || 0);

        let forward = rotateZ([0, -1, 0], yaw);
        let right = rotateZ([1, 0, 0], yaw);

        forward = rotateAxis(forward, right, pitch);
        forward = normalize(forward);

        let up = cross(forward, right);
        up = normalize(up);
        right = normalize(right);

        right = rotateAxis(right, forward, roll);
        up = rotateAxis(up, forward, roll);

        return {
            forward: forward,
            right: right,
            up: up
        };
    }

    // Unit ray through the centre of integer pixel (px, py) of a
    // width*height image with the given vertical field of view.
    function rayDirection(camera, px, py, width, height) {
        const basis = cameraBasis(camera);
        const fovY = camera && camera.fovY != null ? +camera.fovY : Math.PI / 3;
        const w = width > 0 ? width : 1;
        const h = height > 0 ? height : 1;
        const tanY = Math.tan(fovY / 2);
        const tanX = tanY * (w / h);

        const sx = (((px + 0.5) / w) * 2) - 1;
        const sy = 1 - (((py + 0.5) / h) * 2);

        const d = [
            basis.forward[0] + sx * tanX * basis.right[0] + sy * tanY * basis.up[0],
            basis.forward[1] + sx * tanX * basis.right[1] + sy * tanY * basis.up[1],
            basis.forward[2] + sx * tanX * basis.right[2] + sy * tanY * basis.up[2]
        ];
        return normalize(d);
    }

    function renderPerspective(map, camera, width, height) {
        camera = camera || {};
        if (width == null) width = camera.width;
        if (height == null) height = camera.height;
        const W = Math.max(1, width | 0);
        const H = Math.max(1, height | 0);
        const color = new Uint32Array(W * H);
        const depth = new Float64Array(W * H);

        const position = camera.position != null ? camera.position : [0, 0, 0];
        const near = camera.near == null ? 0 : +camera.near;
        const far = camera.far == null ? Infinity : +camera.far;
        const background = camera.background == null ? 0 : camera.background >>> 0;

        const mw = Math.max(1, map.width | 0);
        const mh = Math.max(1, map.height | 0);
        const maxH = computeMaxAltitude(map, mw, mh);

        const basis = cameraBasis(camera);
        const fovY = camera.fovY != null ? +camera.fovY : Math.PI / 3;
        const tanY = Math.tan(fovY / 2);
        const tanX = tanY * (W / H);

        const px0 = read3(position);
        const ox = +px0[0], oy = +px0[1], oz = +px0[2];

        let idx = 0;
        for (let py = 0; py < H; py++) {
            const sy = 1 - (((py + 0.5) / H) * 2);
            for (let px = 0; px < W; px++, idx++) {
                const sx = (((px + 0.5) / W) * 2) - 1;

                let dx = basis.forward[0] + sx * tanX * basis.right[0] + sy * tanY * basis.up[0];
                let dy = basis.forward[1] + sx * tanX * basis.right[1] + sy * tanY * basis.up[1];
                let dz = basis.forward[2] + sx * tanX * basis.right[2] + sy * tanY * basis.up[2];
                const invLen = 1 / Math.hypot(dx, dy, dz);
                dx *= invLen;
                dy *= invLen;
                dz *= invLen;

                const hit = traceCore(
                    map,
                    [ox, oy, oz],
                    [dx, dy, dz],
                    near,
                    far,
                    maxH
                );

                if (hit) {
                    color[idx] = hit.color == null ? 0 : hit.color >>> 0;
                    depth[idx] = hit.distance;
                } else {
                    color[idx] = background;
                    depth[idx] = Infinity;
                }
            }
        }

        return { color: color, depth: depth };
    }

    return {
        traceTerrain: traceTerrain,
        renderPerspective: renderPerspective,
        cameraBasis: cameraBasis,
        rayDirection: rayDirection
    };
});
