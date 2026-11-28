'use strict';

// Procedural terrain source + ring-buffer clipmap.
// Works as a browser classic script (global TerrainSource/TerrainClipmap)
// and as a Node CommonJS module. JavaScript builtins only.

(function (global, factory) {
    const api = factory();
    if (typeof module !== 'undefined' && module.exports) {
        module.exports = api;
    } else {
        global.TerrainSource = api.TerrainSource;
        global.TerrainClipmap = api.TerrainClipmap;
    }
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {

const COORD_LIMIT = 1 << 30;          // inclusive world coordinate bound
const EMPTY_SLOT = -2147483648;      // Int32 sentinel, outside +/-2^30

function isUint32(v) {
    return Number.isInteger(v) && v >= 0 && v <= 0xFFFFFFFF;
}

function isSafeCoord(v) {
    return Number.isInteger(v) && v >= -COORD_LIMIT && v <= COORD_LIMIT;
}

function mod(n, m) {
    return ((n % m) + m) % m;
}

function pointKey(x, y) {
    return x + ',' + y;
}

// ---------------------------------------------------------------------------
// TerrainSource: deterministic read-only base function plus a sparse override
// cover indexed by raw (unwrapped) world coordinates.
// ---------------------------------------------------------------------------

class TerrainSource {
    constructor(sampleBase) {
        if (typeof sampleBase !== 'function') {
            throw new RangeError('TerrainSource requires a sampleBase(x,y) function');
        }
        this._base = sampleBase;
        this._overrides = new Map();
        this._subscribers = new Set();
    }

    // One effective value read. Override hits count exactly like base reads.
    read(x, y) {
        const hit = this._overrides.get(pointKey(x, y));
        if (hit !== undefined) {
            return {height: hit.height, color: hit.color};
        }
        return this._base(x, y);
    }

    // Validate the whole batch first, commit only if every item is legal.
    // Duplicate coordinates: the last item wins. Empty batch changes nothing.
    edit(batch) {
        if (!Array.isArray(batch)) {
            throw new RangeError('edit batch must be an array');
        }
        if (batch.length === 0) {
            return;
        }
        const pending = new Map();
        for (const item of batch) {
            if (item === null || typeof item !== 'object') {
                throw new RangeError('every edit item must be an object {x,y,height,color}');
            }
            const x = item.x;
            const y = item.y;
            const height = item.height;
            const color = item.color;
            if (!isSafeCoord(x) || !isSafeCoord(y)) {
                throw new RangeError('edit coordinates must be integers in [-2^30, 2^30]');
            }
            if (!Number.isFinite(height)) {
                throw new RangeError('edit height must be finite');
            }
            if (!isUint32(color)) {
                throw new RangeError('edit color must be a uint32 (0xAABBGGRR)');
            }
            pending.set(pointKey(x, y), {x: x, y: y, height: height, color: color});
        }

        for (const [key, v] of pending) {
            this._overrides.set(key, {height: v.height, color: v.color});
        }

        const points = Array.from(pending.values());
        for (const view of Array.from(this._subscribers)) {
            view._notifyEdit(points);
        }
    }

    _subscribe(view) {
        this._subscribers.add(view);
    }

    _unsubscribe(view) {
        this._subscribers.delete(view);
    }

    subscriberCount() {
        return this._subscribers.size;
    }
}

// ---------------------------------------------------------------------------
// TerrainClipmap: one fixed side*side ring buffer per level. Level l uses a
// grid spacing of 2^l and stores source values at absolute world grid points
// (no regional averaging). Slots are never moved or re-copied; a slot keeps
// the absolute grid identity of the sample currently held in it.
// ---------------------------------------------------------------------------

class TerrainClipmap {
    constructor(source, options) {
        if (source === null || typeof source !== 'object' ||
            typeof source.read !== 'function' ||
            typeof source._subscribe !== 'function') {
            throw new RangeError('TerrainClipmap requires a TerrainSource');
        }
        options = options || {};
        const side = options.side;
        const levels = options.levels;
        if (!Number.isInteger(side) || side < 9 || (side & 1) !== 1) {
            throw new RangeError('side must be an odd integer >= 9');
        }
        if (!Number.isInteger(levels) || levels < 1 || levels > 16) {
            throw new RangeError('levels must be an integer in [1,16]');
        }

        this.source = source;
        this.side = side;
        this.levels = levels;
        this.R = (side - 1) >> 1;
        // The ring is one cell larger than the window on each axis. The
        // side x side residents never occupy all ring slots, so an incoming
        // strip wraps into a free/ghost slot instead of evicting the strip
        // that just left: stepping back into a previous window needs no
        // source reads. Slot injection over any side consecutive grid
        // indices is still guaranteed (side < ring), so every resident cell
        // always owns a distinct slot. Space stays O(side^2 * levels).
        this._ring = side + 1;
        this._cells = this._ring * this._ring;
        this._destroyed = false;
        this._initialized = new Array(levels).fill(false);
        this.originX = new Array(levels).fill(0);
        this.originY = new Array(levels).fill(0);

        this.heights = [];
        this.colors = [];
        this.gridX = [];
        this.gridY = [];
        for (let l = 0; l < levels; l++) {
            this.heights.push(new Float64Array(this._cells));
            this.colors.push(new Uint32Array(this._cells));
            this.gridX.push(new Int32Array(this._cells).fill(EMPTY_SLOT));
            this.gridY.push(new Int32Array(this._cells).fill(EMPTY_SLOT));
        }

        this._camX = 0;
        this._camY = 0;
        this._hasMoved = false;

        this.reads = 0;
        this.fillsByLevel = new Array(levels).fill(0);
        this.lastReads = 0;
        this.lastFillsByLevel = new Array(levels).fill(0);

        source._subscribe(this);
    }

    _alive() {
        if (this._destroyed) {
            throw new Error('TerrainClipmap view has been destroyed');
        }
    }

    _spacing(level) {
        return 1 << level;
    }

    _slot(i, j) {
        return mod(j, this._ring) * this._ring + mod(i, this._ring);
    }

    // The single place where a source value enters the cache, so every
    // effective source read is counted exactly once.
    _fill(level, i, j) {
        const s = this._spacing(level);
        const x = i * s;
        const y = j * s;
        const value = this.source.read(x, y);
        this.reads++;
        this.lastReads++;
        const slot = this._slot(i, j);
        this.gridX[level][slot] = i;
        this.gridY[level][slot] = j;
        this.heights[level][slot] = value.height;
        this.colors[level][slot] = value.color;
        this.fillsByLevel[level]++;
        this.lastFillsByLevel[level]++;
    }

    moveTo(cx, cy) {
        this._alive();
        if (!Number.isFinite(cx) || !Number.isFinite(cy)) {
            throw new RangeError('camera coordinates must be finite');
        }

        // Validate every level's window before touching any cache.
        const origins = [];
        for (let l = 0; l < this.levels; l++) {
            const s = this._spacing(l);
            const ox = Math.floor(cx / s) - this.R;
            const oy = Math.floor(cy / s) - this.R;
            origins.push([ox, oy]);
            const minX = ox * s;
            const minY = oy * s;
            const maxX = (ox + this.side - 1) * s;
            const maxY = (oy + this.side - 1) * s;
            if (minX < -COORD_LIMIT || maxX > COORD_LIMIT ||
                minY < -COORD_LIMIT || maxY > COORD_LIMIT) {
                throw new RangeError('clipmap window exceeds coordinate limit [-2^30, 2^30]');
            }
        }

        this.lastReads = 0;
        this.lastFillsByLevel.fill(0);

        for (let l = 0; l < this.levels; l++) {
            const s = this._spacing(l);
            const ox = origins[l][0];
            const oy = origins[l][1];
            if (!this._initialized[l]) {
                for (let j = oy; j < oy + this.side; j++) {
                    for (let i = ox; i < ox + this.side; i++) {
                        this._fill(l, i, j);
                    }
                }
                this._initialized[l] = true;
            } else {
                // Identity check: slots already holding this exact grid point
                // are skipped, so only the new/old window set difference is
                // read. A slot reused by a different point is overwritten.
                for (let j = oy; j < oy + this.side; j++) {
                    for (let i = ox; i < ox + this.side; i++) {
                        const slot = this._slot(i, j);
                        if (this.gridX[l][slot] === i &&
                            this.gridY[l][slot] === j) {
                            continue;
                        }
                        this._fill(l, i, j);
                    }
                }
            }
            this.originX[l] = ox;
            this.originY[l] = oy;
        }

        this._camX = cx;
        this._camY = cy;
        this._hasMoved = true;
    }

    // Source edit notification: refresh only resident slots whose absolute
    // world coordinate sits on this level's grid. Non-resident edits do not
    // trigger any source read here.
    _notifyEdit(points) {
        this._alive();
        this.lastReads = 0;
        this.lastFillsByLevel.fill(0);
        for (let p = 0; p < points.length; p++) {
            const point = points[p];
            for (let l = 0; l < this.levels; l++) {
                const s = this._spacing(l);
                if ((point.x % s) !== 0 || (point.y % s) !== 0) {
                    continue;
                }
                const i = point.x / s;
                const j = point.y / s;
                if (i < this.originX[l] || i >= this.originX[l] + this.side ||
                    j < this.originY[l] || j >= this.originY[l] + this.side) {
                    continue;
                }
                const slot = this._slot(i, j);
                if (this.gridX[l][slot] !== i || this.gridY[l][slot] !== j) {
                    continue;
                }
                const value = this.source.read(point.x, point.y);
                this.reads++;
                this.lastReads++;
                this.heights[l][slot] = value.height;
                this.colors[l][slot] = value.color;
                this.fillsByLevel[l]++;
                this.lastFillsByLevel[l]++;
            }
        }
    }

    _cell(level, i, j) {
        const slot = this._slot(i, j);
        const c = this.colors[level][slot] >>> 0;
        return {
            h: this.heights[level][slot],
            r: c & 255,
            g: (c >>> 8) & 255,
            b: (c >>> 16) & 255,
            a: (c >>> 24) & 255
        };
    }

    // Bilinear reconstruction on one level, all channels kept as floats.
    _bilinear(level, x, y) {
        const s = this._spacing(level);
        const i0 = Math.floor(x / s);
        const j0 = Math.floor(y / s);
        const fx = x / s - i0;
        const fy = y / s - j0;
        const c00 = this._cell(level, i0, j0);
        const c10 = this._cell(level, i0 + 1, j0);
        const c01 = this._cell(level, i0, j0 + 1);
        const c11 = this._cell(level, i0 + 1, j0 + 1);
        const w00 = (1 - fx) * (1 - fy);
        const w10 = fx * (1 - fy);
        const w01 = (1 - fx) * fy;
        const w11 = fx * fy;
        const mix = function (name) {
            return c00[name] * w00 + c10[name] * w10 +
                   c01[name] * w01 + c11[name] * w11;
        };
        return {
            h: mix('h'),
            r: mix('r'),
            g: mix('g'),
            b: mix('b'),
            a: mix('a')
        };
    }

    static _blend(a, b, w) {
        const u = 1 - w;
        return {
            h: u * a.h + w * b.h,
            r: u * a.r + w * b.r,
            g: u * a.g + w * b.g,
            b: u * a.b + w * b.b,
            a: u * a.a + w * b.a
        };
    }

    // Reconstruct around the last moveTo center using Chebyshev radius r.
    // b_l = (R-2) * 2^l.
    //   r <= b0/2          -> level 0 only
    //   b_l/2 < r < b_l    -> blend level l and l+1, w = 2r/b_l - 1
    //   r == b_l           -> the coarser level (w = 1)
    //   r > b_last         -> null
    sample(x, y) {
        this._alive();
        if (!this._hasMoved) {
            throw new Error('sample requires a moveTo call first');
        }
        const r = Math.max(Math.abs(x - this._camX),
                           Math.abs(y - this._camY));
        const margin = this.R - 2;
        const bLast = margin * this._spacing(this.levels - 1);
        if (r > bLast) {
            return null;
        }

        let v;
        if (r <= margin / 2) {
            v = this._bilinear(0, x, y);
        } else {
            let chosen = -1;
            for (let l = 0; l < this.levels - 1; l++) {
                const bl = margin * this._spacing(l);
                if (r <= bl) {
                    chosen = l;
                    break;
                }
            }
            if (chosen === -1) {
                // Single level, or the last level has no coarser neighbour.
                v = this._bilinear(this.levels - 1, x, y);
            } else {
                const bl = margin * this._spacing(chosen);
                const w = 2 * r / bl - 1;
                const fine = this._bilinear(chosen, x, y);
                const coarse = this._bilinear(chosen + 1, x, y);
                v = TerrainClipmap._blend(fine, coarse, w);
            }
        }

        const quantize = function (c) {
            c = Math.round(c);
            if (c < 0) c = 0;
            else if (c > 255) c = 255;
            return c;
        };
        const color = ((quantize(v.a) << 24) |
                       (quantize(v.b) << 16) |
                       (quantize(v.g) << 8) |
                       quantize(v.r)) >>> 0;
        return {height: v.h, color: color};
    }

    stats() {
        this._alive();
        let resident = 0;
        for (let l = 0; l < this.levels; l++) {
            if (this._initialized[l]) resident += this.side * this.side;
        }
        return {
            reads: this.reads,
            fillsByLevel: this.fillsByLevel.slice(),
            resident: resident,
            lastReads: this.lastReads,
            lastFillsByLevel: this.lastFillsByLevel.slice()
        };
    }

    destroy() {
        this._alive();
        this.source._unsubscribe(this);
        this._destroyed = true;
    }
}

return {TerrainSource: TerrainSource, TerrainClipmap: TerrainClipmap};
});
