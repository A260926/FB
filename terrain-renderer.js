/*
 * terrain-renderer.js
 *
 * Perspective ray-casting renderer for a height field made of solid columns.
 *
 * Terrain model
 * -------------
 * Every integer XY cell (cx, cy) is one solid column filling
 *     [cx, cx + 1) x [cy, cy + 1) x (-infinity, altitude(cx, cy)] .
 * The columns repeat periodically in X and Y, including for negative
 * coordinates. A cell is addressed with
 *     index = mod(cx, width) + width * mod(cy, height)
 * so altitude / color are plain numeric arrays, no image files required.
 * Heights are never interpolated: a column keeps a constant altitude over
 * its whole footprint. A grid line between two columns of equal altitude is
 * not a surface; only column tops and the vertical walls created by an
 * altitude difference between neighbours can be hit.
 *
 * The module works both as a browser script (global `TerrainRenderer`) and
 * as a CommonJS module (module.exports).
 */
(function (global, factory) {
    "use strict";
    var api = factory();
    if (typeof module !== "undefined" && module.exports) {
        module.exports = api;
    } else {
        global.TerrainRenderer = api;
    }
})(typeof self !== "undefined" ? self : this, function () {
    "use strict";

    var EPS = 1e-9;

    function floorMod(a, n) {
        var m = a % n;
        return m < 0 ? m + n : m;
    }

    function cellIndex(map, cx, cy) {
        return floorMod(cx, map.width) + map.width * floorMod(cy, map.height);
    }

    function altitudeAt(map, cx, cy) {
        return map.altitude[cellIndex(map, cx, cy)];
    }

    function colorAt(map, cx, cy) {
        return map.color[cellIndex(map, cx, cy)];
    }

    function isNear(a, b) {
        if (!isFinite(a) || !isFinite(b)) {
            // Two same-signed infinities coincide; a finite value never does.
            return a === b;
        }
        return Math.abs(a - b) <= EPS * (1 + Math.abs(a) + Math.abs(b));
    }

    /*
     * DDA setup for one axis. Returns the absolute ray parameter of the next
     * integer grid line strictly ahead, the cell step direction and the
     * parameter delta between two consecutive lines. A zero direction
     * component simply produces Infinity, i.e. no events on that axis.
     */
    function initAxis(originComponent, dirComponent, near) {
        var p = originComponent + dirComponent * near;
        if (dirComponent === 0) {
            return { next: Infinity, step: 0, delta: Infinity };
        }
        var cell = Math.floor(p);
        var boundary, step;
        if (dirComponent > 0) {
            boundary = cell + 1;
            step = 1;
        } else {
            boundary = cell;
            step = -1;
        }
        var t = near + (boundary - p) / dirComponent;
        if (isNear(t, near)) t = near;
        return { next: t, step: step, delta: Math.abs(1 / dirComponent) };
    }

    /*
     * Trace a single ray through the column field.
     *
     * direction is a unit 3d vector; near / far are world distances measured
     * along the ray and the search interval is closed. The walk advances only
     * on XY grid-line crossing events and on the current column's top plane,
     * never on a fixed step size. Ties at one distance are resolved with the
     * priority Z face, X face, Y face and every reported normal points out of
     * the solid column.
     *
     * If the ray starts (or enters at `near`) inside a column, the first
     * boundary that leaves that solid volume is reported instead of `near`.
     * Returns {distance, position, normal, cell} or null when nothing is hit.
     */
    function traceTerrain(map, origin, direction, near, far) {
        if (!Number.isInteger(map.width) || map.width <= 0 ||
            !Number.isInteger(map.height) || map.height <= 0) {
            throw new RangeError("map.width and map.height must be positive integers");
        }
        if (!(far >= near)) {
            throw new RangeError("far must be greater than or equal to near");
        }

        var ox = origin[0], oy = origin[1], oz = origin[2];
        var dx = direction[0], dy = direction[1], dz = direction[2];
        var len = Math.sqrt(dx * dx + dy * dy + dz * dz);
        if (!(len > 0)) {
            throw new RangeError("direction must be a non-zero unit vector");
        }
        dx /= len; dy /= len; dz /= len;

        /*
         * Solidity of the field at a point. Columns are treated as closed
         * boxes, so a point sitting on a grid line belongs to every column
         * meeting there. This makes origins exactly on a wall or the near
         * plane unambiguous.
         */
        function pointSolid(x, y, z) {
            var bx = Math.floor(x);
            var xs = isNear(x, bx) ? [bx - 1, bx] : [bx];
            var by = Math.floor(y);
            var ys = isNear(y, by) ? [by - 1, by] : [by];
            for (var i = 0; i < xs.length; i++) {
                for (var j = 0; j < ys.length; j++) {
                    if (z <= altitudeAt(map, xs[i], ys[j]) + EPS) return true;
                }
            }
            return false;
        }

        var t = near;
        var cx = Math.floor(ox + dx * near);
        var cy = Math.floor(oy + dy * near);

        var axisX = initAxis(ox, dx, near);
        var axisY = initAxis(oy, dy, near);

        // Solidity just inside the near plane: boundary contact counts as in.
        var inside = pointSolid(ox + dx * near, oy + dy * near, oz + dz * near);

        // The ray can cross at most this many grid lines inside [near, far],
        // plus one top-plane event per visited column.
        var maxEvents = 32 + 2 * Math.ceil((far - near + 2) * (Math.abs(dx) + Math.abs(dy)) + 4);

        for (var iter = 0; iter < maxEvents; iter++) {
            var h = altitudeAt(map, cx, cy);

            // Top plane of the column currently occupied by the ray.
            var tZ = dz !== 0 ? (h - oz) / dz : Infinity;
            if (tZ < t - EPS) tZ = Infinity;

            var tX = axisX.next;
            var tY = axisY.next;
            var tMin = Math.min(tZ, tX, tY);

            if (!isFinite(tMin) || tMin > far + EPS) return null;
            if (tMin < near) tMin = near;

            var crossedZ = isNear(tZ, tMin);
            var crossedX = isNear(tX, tMin);
            var crossedY = isNear(tY, tMin);

            // Remember where the ray was before this event, then move the DDA
            // into the cell occupied just after it.
            var beforeCx = cx, beforeCy = cy;
            if (crossedX) {
                cx += axisX.step;
                axisX.next += axisX.delta;
            }
            if (crossedY) {
                cy += axisY.step;
                axisY.next += axisY.delta;
            }

            var zEv = oz + dz * tMin;
            var afterInside;
            if (crossedZ && isNear(zEv, altitudeAt(map, cx, cy))) {
                // Exactly on a top edge: the ray's vertical sign decides which
                // side it is on after the crossing.
                afterInside = dz < 0;
            } else {
                afterInside = zEv <= altitudeAt(map, cx, cy) + EPS;
            }

            if (inside === afterInside) {
                // No solidity change: grid line between equal columns, or a
                // plane crossed without entering/leaving solid.
                t = tMin;
                continue;
            }

            var entering = afterInside;
            var faceCell, normal;

            // Priority Z > X > Y.
            if (crossedZ) {
                faceCell = entering ? [cx, cy] : [beforeCx, beforeCy];
                normal = [0, 0, 1];
            } else if (crossedX && crossedY) {
                // Corner: use the edge neighbours to assign the face while
                // keeping X ahead of Y.
                var solidB = zEv <= altitudeAt(map, beforeCx + axisX.step, beforeCy) + EPS;
                var solidC = zEv <= altitudeAt(map, beforeCx, beforeCy + axisY.step) + EPS;
                if (entering) {
                    if (solidB) {
                        faceCell = [beforeCx + axisX.step, beforeCy];
                        normal = [-axisX.step, 0, 0];
                    } else if (solidC) {
                        faceCell = [beforeCx, beforeCy + axisY.step];
                        normal = [0, -axisY.step, 0];
                    } else {
                        faceCell = [cx, cy];
                        normal = [-axisX.step, 0, 0];
                    }
                } else {
                    if (!solidB) {
                        faceCell = [beforeCx, beforeCy];
                        normal = [axisX.step, 0, 0];
                    } else {
                        faceCell = [beforeCx, beforeCy];
                        normal = [0, axisY.step, 0];
                    }
                }
            } else if (crossedX) {
                faceCell = entering ? [cx, cy] : [beforeCx, beforeCy];
                normal = entering ? [-axisX.step, 0, 0] : [axisX.step, 0, 0];
            } else {
                faceCell = entering ? [cx, cy] : [beforeCx, beforeCy];
                normal = entering ? [0, -axisY.step, 0] : [0, axisY.step, 0];
            }

            // Coincident top edge always resolves to the upward Z face, even
            // for a horizontal ray whose dz is zero.
            if (isNear(zEv, altitudeAt(map, faceCell[0], faceCell[1]))) {
                normal = [0, 0, 1];
            }

            inside = afterInside;
            return {
                distance: tMin,
                position: [ox + dx * tMin, oy + dy * tMin, zEv],
                normal: normal,
                cell: [floorMod(faceCell[0], map.width), floorMod(faceCell[1], map.height)]
            };
        }

        return null;
    }

    function rotateVector(v, axis, angle) {
        var c = Math.cos(angle), s = Math.sin(angle);
        var dot = v[0] * axis[0] + v[1] * axis[1] + v[2] * axis[2];
        var cross = [
            axis[1] * v[2] - axis[2] * v[1],
            axis[2] * v[0] - axis[0] * v[2],
            axis[0] * v[1] - axis[1] * v[0]
        ];
        return [
            v[0] * c + cross[0] * s + axis[0] * dot * (1 - c),
            v[1] * c + cross[1] * s + axis[1] * dot * (1 - c),
            v[2] * c + cross[2] * s + axis[2] * dot * (1 - c)
        ];
    }

    /*
     * Build the camera frame.
     *
     * World Z is up. With every angle zero the forward vector is (0,-1,0) and
     * the right vector is (1,0,0). Yaw rotates around world Z first, pitch
     * then rotates around the resulting right vector, and roll finally
     * rotates around the forward vector. All rotations use the right-hand
     * rule with positive angles, angles in radians.
     */
    function cameraFrame(camera) {
        var forward = [0, -1, 0];
        var right = [1, 0, 0];
        var up = [0, 0, 1];

        forward = rotateVector(forward, [0, 0, 1], camera.yaw || 0);
        right = rotateVector(right, [0, 0, 1], camera.yaw || 0);

        forward = rotateVector(forward, right, camera.pitch || 0);
        up = rotateVector(up, right, camera.pitch || 0);

        right = rotateVector(right, forward, camera.roll || 0);
        up = rotateVector(up, forward, camera.roll || 0);

        return { forward: forward, right: right, up: up };
    }

    /*
     * Render a full perspective frame. Rays go through pixel centres and are
     * shaped by the vertical field of view fovY; each ray is normalized to a
     * unit vector. Returns {color: Uint32Array, depth: Float64Array} where
     * depth is the world distance to the first intersection (Infinity for the
     * background colour).
     */
    function renderPerspective(map, camera, width, height) {
        if (!Number.isInteger(width) || width <= 0 ||
            !Number.isInteger(height) || height <= 0) {
            throw new RangeError("width and height must be positive integers");
        }

        var frame = cameraFrame(camera);
        var f = frame.forward, r = frame.right, u = frame.up;
        var tanHalfY = Math.tan(camera.fovY / 2);
        var tanHalfX = tanHalfY * (width / height);
        var background = camera.background || 0;
        var origin = camera.position;

        var color = new Uint32Array(width * height);
        var depth = new Float64Array(width * height);

        for (var py = 0; py < height; py++) {
            // ny is -1 at the top row and +1 at the bottom row.
            var ny = (2 * (py + 0.5)) / height - 1;
            for (var px = 0; px < width; px++) {
                var nx = (2 * (px + 0.5)) / width - 1;
                var dx = f[0] + r[0] * nx * tanHalfX - u[0] * ny * tanHalfY;
                var dy = f[1] + r[1] * nx * tanHalfX - u[1] * ny * tanHalfY;
                var dz = f[2] + r[2] * nx * tanHalfX - u[2] * ny * tanHalfY;
                var inv = 1 / Math.sqrt(dx * dx + dy * dy + dz * dz);
                var dir = [dx * inv, dy * inv, dz * inv];

                var idx = py * width + px;
                var hit = traceTerrain(map, origin, dir, camera.near, camera.far);
                if (hit === null) {
                    color[idx] = background;
                    depth[idx] = Infinity;
                } else {
                    color[idx] = colorAt(map, hit.cell[0], hit.cell[1]);
                    depth[idx] = hit.distance;
                }
            }
        }

        return { color: color, depth: depth };
    }

    return {
        traceTerrain: traceTerrain,
        renderPerspective: renderPerspective,
        cameraFrame: cameraFrame
    };
});
