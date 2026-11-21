/*
 * terrain-clipmap.js
 *
 * Procedural clipmap terrain for VoxelSpace.
 *
 * TerrainSource wraps a deterministic sampleBase(x, y) callback and keeps a
 * sparse set of world-point edits on top of it.  TerrainClipmap keeps one
 * fixed-size ring buffer per detail level; only the side x side grid points of
 * the current window ever reside in memory.
 *
 * Level l has grid spacing s = 2^l.  A node identified by grid indices
 * (ix, iy) sits on the world point (ix * s, iy * s); source values are taken
 * at that exact world point, never averaged over an area.
 *
 * Pure JavaScript builtins.  Works as a Node CommonJS module and as a classic
 * browser script (the two constructors become globals).
 */
(function (root, factory) {
    'use strict';
    var api = factory();
    if (typeof module === 'object' && module && module.exports) {
        module.exports = api;
    }
    if (root) {
        root.TerrainSource = api.TerrainSource;
        root.TerrainClipmap = api.TerrainClipmap;
    }
})(typeof globalThis !== 'undefined' ? globalThis : (typeof self !== 'undefined' ? self : this), function () {
    'use strict';

    var COORD_LIMIT = 1 << 30;            // 2^30, inclusive coordinate bound

    function isFiniteNumber(v) {
        return typeof v === 'number' && Number.isFinite(v);
    }

    function validCoord(v) {
        return Number.isInteger(v) && v >= -COORD_LIMIT && v <= COORD_LIMIT;
    }

    // Encode an integer pair into one Map key.  Numeric packing would exceed
    // Number.MAX_SAFE_INTEGER for the full [-2^30, 2^30] range and collide,
    // so the key is a string.
    function keyOf(x, y) {
        return x + ',' + y;
    }

    // -------------------------------------------------------------------------
    // TerrainSource
    // -------------------------------------------------------------------------

    function TerrainSource(sampleBase) {
        if (typeof sampleBase !== 'function') {
            throw new TypeError('TerrainSource requires a sampleBase function');
        }
        this._sampleBase = sampleBase;
        this._overrides = new Map();   // key -> {height, color}
        this._views = new Set();
    }

    // Effective value at an integer world point.  Reading through the sparse
    // coverage counts as a source read at the call site that uses it.
    TerrainSource.prototype._read = function (x, y) {
        var hit = this._overrides.get(keyOf(x, y));
        if (hit !== undefined) {
            return { height: hit.height, color: hit.color };
        }
        return this._sampleBase(x, y);
    };

    // Validate the whole batch first; only commit when every entry is legal.
    TerrainSource.prototype.edit = function (batch) {
        if (!Array.isArray(batch)) {
            throw new RangeError('edit batch must be an array');
        }
        var prepared = new Array(batch.length);
        for (var i = 0; i < batch.length; i++) {
            var e = batch[i];
            if (e === null || typeof e !== 'object') {
                throw new RangeError('edit entry ' + i + ' is not an object');
            }
            if (!validCoord(e.x) || !validCoord(e.y)) {
                throw new RangeError('edit entry ' + i + ' has out of range coordinates');
            }
            if (!isFiniteNumber(e.height)) {
                throw new RangeError('edit entry ' + i + ' has a non-finite height');
            }
            // Colors are 0xAABBGGRR uint32 values.
            if (!Number.isInteger(e.color) || e.color < 0 || e.color > 0xFFFFFFFF) {
                throw new RangeError('edit entry ' + i + ' has an invalid uint32 color');
            }
            prepared[i] = { key: keyOf(e.x, e.y), x: e.x, y: e.y,
                            height: e.height, color: e.color };
        }
        if (prepared.length === 0) {
            return; // empty batch changes nothing and notifies nobody
        }
        // Commit: later entries for the same coordinate win ("last wins").
        // Keep one change record per coordinate for the view notifications.
        var changed = new Map();
        for (var j = 0; j < prepared.length; j++) {
            var p = prepared[j];
            this._overrides.set(p.key, { height: p.height, color: p.color });
            changed.set(p.key, { x: p.x, y: p.y });
        }
        var changes = Array.from(changed.values());
        var views = Array.from(this._views);
        for (var v = 0; v < views.length; v++) {
            views[v]._onSourceEdit(changes);
        }
    };

    TerrainSource.prototype._subscribe = function (view) {
        this._views.add(view);
    };

    TerrainSource.prototype._unsubscribe = function (view) {
        this._views.delete(view);
    };

    TerrainSource.prototype.subscriberCount = function () {
        return this._views.size;
    };

    // -------------------------------------------------------------------------
    // One clipmap level: fixed ring of side*side slots plus absolute identity.
    // -------------------------------------------------------------------------

    function Level(index, side, scale, R) {
        this.index = index;
        this.side = side;
        // The ring keeps one extra slot per axis.  A one-cell window move then
        // lands its incoming L-shape in the free row/column without evicting
        // the departing edge, so stepping back into the previous window needs
        // zero source reads.  Resident nodes stay exactly side*side; the ring
        // is still O(side^2).
        this.ringSide = side + 1;
        this.scale = scale;             // 2^level
        this.R = R;
        this.n = this.ringSide * this.ringSide;
        this.heights = new Float64Array(this.n);
        this.colors = new Uint32Array(this.n);
        this.gx = new Int32Array(this.n);   // grid-index identity of each slot
        this.gy = new Int32Array(this.n);
        this.used = new Uint8Array(this.n); // slot holds a known point
        this.originX = 0;               // window lower-left grid index
        this.originY = 0;
        this.initialized = false;
    }

    // Ring slot for grid indices; negative indices wrap correctly.
    Level.prototype.slot = function (ix, iy) {
        var m = this.ringSide;
        var sx = ix % m;
        if (sx < 0) sx += m;
        var sy = iy % m;
        if (sy < 0) sy += m;
        return sy * m + sx;
    };

    // -------------------------------------------------------------------------
    // TerrainClipmap
    // -------------------------------------------------------------------------

    function TerrainClipmap(source, options) {
        if (!(source instanceof TerrainSource)) {
            throw new TypeError('TerrainClipmap requires a TerrainSource');
        }
        options = options || {};
        var side = options.side;
        var levels = options.levels;
        if (!Number.isInteger(side) || side < 9 || side % 2 !== 1) {
            throw new RangeError('side must be an odd integer not smaller than 9');
        }
        if (!Number.isInteger(levels) || levels < 1 || levels > 16) {
            throw new RangeError('levels must be an integer between 1 and 16');
        }
        this.source = source;
        this.side = side;
        this.levels = levels;
        this.R = (side - 1) / 2;
        this.destroyed = false;
        this.hasCamera = false;
        this.cx = 0;
        this.cy = 0;

        this.levelsData = [];
        for (var l = 0; l < levels; l++) {
            this.levelsData.push(new Level(l, side, 1 << l, this.R));
        }

        // Cumulative statistics; reads counts every effective source read,
        // including reads served by the sparse override coverage.
        this.reads = 0;
        this.fillsByLevel = new Uint32Array(levels);
        // Statistics of the latest moveTo / edit notification.
        this.lastReads = 0;
        this.lastFillsByLevel = new Uint32Array(levels);

        source._subscribe(this);
    }

    TerrainClipmap.prototype._checkAlive = function () {
        if (this.destroyed) {
            throw new Error('TerrainClipmap view has been destroyed');
        }
    };

    TerrainClipmap.prototype._windowOrigin = function (cx, cy, scale) {
        return {
            ox: Math.floor(cx / scale) - this.R,
            oy: Math.floor(cy / scale) - this.R
        };
    };

    // Check every level's full window before mutating anything: grid nodes
    // map to world coordinates (ix * s, iy * s), which must stay in range.
    TerrainClipmap.prototype._validateWindows = function (cx, cy) {
        if (!isFiniteNumber(cx) || !isFiniteNumber(cy)) {
            throw new RangeError('camera coordinates must be finite');
        }
        for (var l = 0; l < this.levels; l++) {
            var scale = 1 << l;
            var w = this._windowOrigin(cx, cy, scale);
            var minX = w.ox * scale;
            var minY = w.oy * scale;
            var maxX = (w.ox + this.side - 1) * scale;
            var maxY = (w.oy + this.side - 1) * scale;
            if (!validCoord(minX) || !validCoord(minY) ||
                !validCoord(maxX) || !validCoord(maxY)) {
                throw new RangeError('level ' + l + ' window leaves the supported coordinate range');
            }
        }
    };

    // Fill one node, identified by grid indices.  Keeps the ring slot if it
    // already holds this exact world point, so walking back costs zero reads.
    TerrainClipmap.prototype._fillPoint = function (level, ix, iy) {
        var s = level.slot(ix, iy);
        if (level.used[s] === 1 && level.gx[s] === ix && level.gy[s] === iy) {
            return;
        }
        var v = this.source._read(ix * level.scale, iy * level.scale);
        this.reads++;
        this.lastReads++;
        level.heights[s] = v.height;
        level.colors[s] = v.color >>> 0;
        level.gx[s] = ix;
        level.gy[s] = iy;
        level.used[s] = 1;
        this.fillsByLevel[level.index]++;
        this.lastFillsByLevel[level.index]++;
    };

    TerrainClipmap.prototype._fillAll = function (level, nOx, nOy) {
        var side = this.side;
        for (var iy = nOy; iy < nOy + side; iy++) {
            for (var ix = nOx; ix < nOx + side; ix++) {
                this._fillPoint(level, ix, iy);
            }
        }
    };

    TerrainClipmap.prototype._fillLevel = function (level, nOx, nOy) {
        var side = this.side;
        if (!level.initialized) {
            this._fillAll(level, nOx, nOy);
            level.originX = nOx;
            level.originY = nOy;
            level.initialized = true;
            return;
        }

        var dX = nOx - level.originX;
        var dY = nOy - level.originY;
        if (dX === 0 && dY === 0) return;

        if (Math.abs(dX) >= side || Math.abs(dY) >= side) {
            // Disjoint windows: the whole level is overwritten in one pass.
            this._fillAll(level, nOx, nOy);
            level.originX = nOx;
            level.originY = nOy;
            return;
        }

        // Incoming grid-index columns [x0, x1) and rows [y0, y1): the set
        // difference between the new window [nOx, nOx+side) and the old one
        // [nOx-dX, nOx+side-dX).
        var x0, x1, y0, y1;
        if (dX > 0) { x0 = nOx + side - dX; x1 = nOx + side; }
        else if (dX < 0) { x0 = nOx; x1 = nOx - dX; }
        else { x0 = nOx; x1 = nOx; }
        if (dY > 0) { y0 = nOy + side - dY; y1 = nOy + side; }
        else if (dY < 0) { y0 = nOy; y1 = nOy - dY; }
        else { y0 = nOy; y1 = nOy; }

        var ix, iy;
        // Vertical strips span the full new window height.
        for (iy = nOy; iy < nOy + side; iy++) {
            for (ix = x0; ix < x1; ix++) {
                this._fillPoint(level, ix, iy);
            }
        }
        // Horizontal strips exclude the incoming columns, so the corner where
        // the two strips intersect is read a single time.
        for (iy = y0; iy < y1; iy++) {
            for (ix = nOx; ix < x0; ix++) {
                this._fillPoint(level, ix, iy);
            }
            for (ix = x1; ix < nOx + side; ix++) {
                this._fillPoint(level, ix, iy);
            }
        }

        level.originX = nOx;
        level.originY = nOy;
    };

    TerrainClipmap.prototype.moveTo = function (cx, cy) {
        this._checkAlive();
        this._validateWindows(cx, cy);   // throw before touching any view state

        // Per-operation counters; nothing is mutated above this line.
        this.lastReads = 0;
        this.lastFillsByLevel = new Uint32Array(this.levels);

        for (var l = 0; l < this.levels; l++) {
            var level = this.levelsData[l];
            var w = this._windowOrigin(cx, cy, level.scale);
            this._fillLevel(level, w.ox, w.oy);
        }
        this.cx = cx;
        this.cy = cy;
        this.hasCamera = true;
    };

    // Source edit callback: refresh only resident nodes whose world point is
    // one of the coordinates changed by this batch.  Earlier edits and edits
    // outside the windows never trigger a source read.
    TerrainClipmap.prototype._onSourceEdit = function (changes) {
        if (this.destroyed) return;
        this.lastReads = 0;
        this.lastFillsByLevel = new Uint32Array(this.levels);
        if (!this.hasCamera) return;
        for (var c = 0; c < changes.length; c++) {
            var x = changes[c].x;
            var y = changes[c].y;
            for (var l = 0; l < this.levels; l++) {
                var level = this.levelsData[l];
                if (!level.initialized) continue;
                var scale = level.scale;
                // A world point can land on this level's grid only when it is
                // a multiple of the level spacing.
                if (x % scale !== 0 || y % scale !== 0) continue;
                var gx = x / scale;
                var gy = y / scale;
                if (gx < level.originX || gx >= level.originX + level.side ||
                    gy < level.originY || gy >= level.originY + level.side) {
                    continue; // not resident: no source read
                }
                this._refreshPoint(level, gx, gy);
            }
        }
    };

    TerrainClipmap.prototype._refreshPoint = function (level, gx, gy) {
        var s = level.slot(gx, gy);
        if (level.used[s] !== 1 || level.gx[s] !== gx || level.gy[s] !== gy) {
            return; // not resident: no source read
        }
        var v = this.source._read(gx * level.scale, gy * level.scale);
        this.reads++;
        this.lastReads++;
        level.heights[s] = v.height;
        level.colors[s] = v.color >>> 0;
        this.fillsByLevel[level.index]++;
        this.lastFillsByLevel[level.index]++;
    };

    TerrainClipmap.prototype.destroy = function () {
        if (this.destroyed) return;
        this.destroyed = true;
        this.source._unsubscribe(this);
    };

    TerrainClipmap.prototype.stats = function () {
        this._checkAlive();
        var resident = 0;
        for (var l = 0; l < this.levels; l++) {
            var level = this.levelsData[l];
            if (!level.initialized) continue;
            // Each resident window node owns exactly one identity-matched slot.
            for (var s = 0; s < level.n; s++) {
                if (level.used[s] === 1 &&
                    level.gx[s] >= level.originX &&
                    level.gx[s] < level.originX + level.side &&
                    level.gy[s] >= level.originY &&
                    level.gy[s] < level.originY + level.side) {
                    resident++;
                }
            }
        }
        return {
            reads: this.reads,
            fillsByLevel: Array.from(this.fillsByLevel),
            resident: resident,
            lastReads: this.lastReads,
            lastFillsByLevel: Array.from(this.lastFillsByLevel)
        };
    };

    // ---- sampling ----------------------------------------------------------

    TerrainClipmap.prototype._resident = function (level, gx, gy) {
        var s = level.slot(gx, gy);
        if (level.used[s] !== 1 || level.gx[s] !== gx || level.gy[s] !== gy) {
            return null;
        }
        return {
            height: level.heights[s],
            color: level.colors[s]
        };
    };

    // Bilinear reconstruction on one level using floor(x/s), floor(y/s) and
    // their four neighbors.  Returns null if any neighbor is not resident.
    TerrainClipmap.prototype._bilinear = function (l, x, y) {
        var level = this.levelsData[l];
        var s = level.scale;
        var gx = Math.floor(x / s);
        var gy = Math.floor(y / s);
        var fx = x / s - gx;
        var fy = y / s - gy;

        var p00 = this._resident(level, gx, gy);
        var p10 = this._resident(level, gx + 1, gy);
        var p01 = this._resident(level, gx, gy + 1);
        var p11 = this._resident(level, gx + 1, gy + 1);
        if (!p00 || !p10 || !p01 || !p11) return null;

        var w00 = (1 - fx) * (1 - fy);
        var w10 = fx * (1 - fy);
        var w01 = (1 - fx) * fy;
        var w11 = fx * fy;

        var height = p00.height * w00 + p10.height * w10 +
                     p01.height * w01 + p11.height * w11;

        // Interpolate the four RGBA channels as floats; quantization happens
        // only after any cross-level blending.
        var c00 = p00.color, c10 = p10.color, c01 = p01.color, c11 = p11.color;
        var channels = new Float64Array(4);
        for (var b = 0; b < 4; b++) {
            var shift = b * 8;
            channels[b] =
                ((c00 >>> shift) & 255) * w00 +
                ((c10 >>> shift) & 255) * w10 +
                ((c01 >>> shift) & 255) * w01 +
                ((c11 >>> shift) & 255) * w11;
        }
        return { height: height, channels: channels };
    };

    function blendSamples(a, b, w) {
        var channels = new Float64Array(4);
        for (var i = 0; i < 4; i++) {
            channels[i] = a.channels[i] * (1 - w) + b.channels[i] * w;
        }
        return {
            height: a.height * (1 - w) + b.height * w,
            channels: channels
        };
    }

    function quantize(channels) {
        var color = 0;
        for (var i = 0; i < 4; i++) {
            var v = Math.round(channels[i]);
            if (v < 0) v = 0;
            else if (v > 255) v = 255;
            color = (color | (v << (i * 8))) >>> 0;
        }
        return color >>> 0;
    }

    TerrainClipmap.prototype.sample = function (x, y) {
        this._checkAlive();
        if (!this.hasCamera) {
            throw new Error('moveTo must be called before sample');
        }
        if (!isFiniteNumber(x) || !isFiniteNumber(y)) return null;

        var dx = x - this.cx;
        var dy = y - this.cy;
        var r = Math.max(Math.abs(dx), Math.abs(dy)); // Chebyshev distance

        var base = this.R - 2;
        var last = this.levels - 1;
        var bLast = base * (1 << last);
        if (r > bLast) return null;

        var b0 = base;
        var chosen;
        if (r <= b0 / 2) {
            // Finest level only.
            chosen = this._bilinear(0, x, y);
            if (!chosen) return null;
        } else {
            // Locate the transition band.  b_{l+1} = 2 b_l, so the bands tile
            // the plane; r == b_l belongs to the coarser level.
            var l = 0;
            while (l < last && r >= base * (1 << l)) {
                l++;
            }
            if (l >= last) {
                // No coarser level exists: use the last level directly.
                chosen = this._bilinear(last, x, y);
                if (!chosen) return null;
            } else {
                var bl = base * (1 << l);
                var fine = this._bilinear(l, x, y);
                var coarse = this._bilinear(l + 1, x, y);
                if (!fine || !coarse) return null;
                var w = 2 * r / bl - 1; // 0 at r = bl/2, 1 at r = bl
                chosen = blendSamples(fine, coarse, w);
            }
        }

        return { height: chosen.height, color: quantize(chosen.channels) };
    };

    // Current resident window of a level (grid indices and spacing).
    TerrainClipmap.prototype.levelWindow = function (l) {
        this._checkAlive();
        var level = this.levelsData[l];
        if (!level.initialized) return null;
        return {
            scale: level.scale,
            originX: level.originX,
            originY: level.originY,
            side: level.side
        };
    };

    return {
        TerrainSource: TerrainSource,
        TerrainClipmap: TerrainClipmap,
        TERRAIN_COORD_LIMIT: COORD_LIMIT
    };
});
