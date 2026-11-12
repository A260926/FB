
const EPSILON = Math.pow(2, -52);
const EDGE_STACK = new Uint32Array(512);

import {orient2d, incircle} from 'robust-predicates';

/** @template {ArrayLike<number>} T */
export default class Delaunator {

    /**
     * Constructs a delaunay triangulation object given an array of points (`[x, y]` by default).
     * `getX` and `getY` are optional functions of the form `(point) => value` for custom point formats.
     *
     * @template P
     * @param {P[]} points
     * @param {(p: P) => number} [getX]
     * @param {(p: P) => number} [getY]
     */
    // @ts-expect-error TS2322
    static from(points, getX = defaultGetX, getY = defaultGetY) {
        const n = points.length;
        const coords = new Float64Array(n * 2);

        for (let i = 0; i < n; i++) {
            const p = points[i];
            coords[2 * i] = getX(p);
            coords[2 * i + 1] = getY(p);
        }

        return new Delaunator(coords);
    }

    /**
     * Constructs a delaunay triangulation object given an array of point coordinates of the form:
     * `[x0, y0, x1, y1, ...]` (use a typed array for best performance). Duplicate points are skipped.
     *
     * @param {T} coords
     */
    constructor(coords) {
        const n = coords.length >> 1;
        if (n > 0 && typeof coords[0] !== 'number') throw new Error('Expected coords to contain numbers.');

        this.coords = coords;

        // arrays that will store the triangulation graph
        const maxTriangles = Math.max(2 * n - 5, 0);
        /** @private */ this._triangles = new Uint32Array(maxTriangles * 3);
        /** @private */ this._halfedges = new Int32Array(maxTriangles * 3);

        // temporary arrays for tracking the edges of the advancing convex hull
        /** @private */ this._hashSize = Math.ceil(Math.sqrt(n));
        /** @private */ this._hullPrev = new Uint32Array(n); // edge to prev edge
        /** @private */ this._hullNext = new Uint32Array(n); // edge to next edge
        /** @private */ this._hullTri = new Uint32Array(n); // edge to adjacent triangle
        /** @private */ this._hullHash = new Int32Array(this._hashSize); // angular edge hash

        // temporary arrays for sorting points
        /** @private */ this._ids = new Uint32Array(n);
        /** @private */ this._dists = new Float64Array(n);

        /** @private */ this.trianglesLen = 0;
        /** @private */ this._cx = 0;
        /** @private */ this._cy = 0;
        /** @private */ this._hullStart = 0;

        // point indices removed through `removePoint` (a full `update()` restores them)
        /** @private */ this._removed = new Set();


        /** A `Uint32Array` array of indices that reference points on the convex hull of the input data, counterclockwise (assuming the y axis points down). */
        this.hull = this._triangles;
        /** A `Uint32Array` array of triangle vertex indices (each group of three numbers forms a triangle). All triangles are directed counterclockwise, assuming the y axis points down. */
        this.triangles = this._triangles;
        /**
         * A `Int32Array` array of triangle half-edge indices that allows you to traverse the triangulation.
         * `i`-th half-edge in the array corresponds to vertex `triangles[i]` the half-edge is coming from.
         * `halfedges[i]` is the index of a twin half-edge in an adjacent triangle (or `-1` for outer half-edges on the convex hull).
         */
        this.halfedges = this._halfedges;

        this.update();
    }

    /**
     * Updates the triangulation if you modified `delaunay.coords` values in place, avoiding expensive memory allocations.
     * Useful for iterative relaxation algorithms such as Lloyd's.
     */
    update() {
        const {coords, _hullPrev: hullPrev, _hullNext: hullNext, _hullTri: hullTri, _hullHash: hullHash} =  this;
        const n = coords.length >> 1;

        // a full rebuild works from the complete coords, so it brings back any removed points
        this._removed.clear();

        // populate an array of point indices; calculate input data bbox
        let minX = Infinity;
        let minY = Infinity;
        let maxX = -Infinity;
        let maxY = -Infinity;

        for (let i = 0; i < n; i++) {
            const x = coords[2 * i];
            const y = coords[2 * i + 1];
            if (x < minX) minX = x;
            if (y < minY) minY = y;
            if (x > maxX) maxX = x;
            if (y > maxY) maxY = y;
            this._ids[i] = i;
        }
        const cx = (minX + maxX) / 2;
        const cy = (minY + maxY) / 2;

        let i0 = 0, i1 = 0, i2 = 0;

        // pick a seed point close to the center
        for (let i = 0, minDist = Infinity; i < n; i++) {
            const d = dist(cx, cy, coords[2 * i], coords[2 * i + 1]);
            if (d < minDist) {
                i0 = i;
                minDist = d;
            }
        }
        const i0x = coords[2 * i0];
        const i0y = coords[2 * i0 + 1];

        // find the point closest to the seed
        for (let i = 0, minDist = Infinity; i < n; i++) {
            if (i === i0) continue;
            const d = dist(i0x, i0y, coords[2 * i], coords[2 * i + 1]);
            if (d < minDist && d > 0) {
                i1 = i;
                minDist = d;
            }
        }
        let i1x = coords[2 * i1];
        let i1y = coords[2 * i1 + 1];

        let minRadius = Infinity;

        // find the third point which forms the smallest circumcircle with the first two
        for (let i = 0; i < n; i++) {
            if (i === i0 || i === i1) continue;
            const r = circumradius(i0x, i0y, i1x, i1y, coords[2 * i], coords[2 * i + 1]);
            if (r < minRadius) {
                i2 = i;
                minRadius = r;
            }
        }
        let i2x = coords[2 * i2];
        let i2y = coords[2 * i2 + 1];

        if (minRadius === Infinity) {
            // order collinear points by dx (or dy if all x are identical)
            // and return the list as a hull
            for (let i = 0; i < n; i++) {
                this._dists[i] = (coords[2 * i] - coords[0]) || (coords[2 * i + 1] - coords[1]);
            }
            quicksort(this._ids, this._dists, 0, n - 1);
            const hull = new Uint32Array(n);
            let j = 0;
            for (let i = 0, d0 = -Infinity; i < n; i++) {
                const id = this._ids[i];
                const d = this._dists[id];
                if (d > d0) {
                    hull[j++] = id;
                    d0 = d;
                }
            }
            this.hull = hull.subarray(0, j);
            this.triangles = new Uint32Array(0);
            this.halfedges = new Int32Array(0);
            return;
        }

        // swap the order of the seed points for counter-clockwise orientation
        if (orient2d(i0x, i0y, i1x, i1y, i2x, i2y) < 0) {
            const i = i1;
            const x = i1x;
            const y = i1y;
            i1 = i2;
            i1x = i2x;
            i1y = i2y;
            i2 = i;
            i2x = x;
            i2y = y;
        }

        const center = circumcenter(i0x, i0y, i1x, i1y, i2x, i2y);
        this._cx = center.x;
        this._cy = center.y;

        for (let i = 0; i < n; i++) {
            this._dists[i] = dist(coords[2 * i], coords[2 * i + 1], center.x, center.y);
        }

        // sort the points by distance from the seed triangle circumcenter
        quicksort(this._ids, this._dists, 0, n - 1);

        // set up the seed triangle as the starting hull
        this._hullStart = i0;
        let hullSize = 3;

        hullNext[i0] = hullPrev[i2] = i1;
        hullNext[i1] = hullPrev[i0] = i2;
        hullNext[i2] = hullPrev[i1] = i0;

        hullTri[i0] = 0;
        hullTri[i1] = 1;
        hullTri[i2] = 2;

        hullHash.fill(-1);
        hullHash[this._hashKey(i0x, i0y)] = i0;
        hullHash[this._hashKey(i1x, i1y)] = i1;
        hullHash[this._hashKey(i2x, i2y)] = i2;

        this.trianglesLen = 0;
        this._addTriangle(i0, i1, i2, -1, -1, -1);

        for (let k = 0, xp = 0, yp = 0; k < this._ids.length; k++) {
            const i = this._ids[k];
            const x = coords[2 * i];
            const y = coords[2 * i + 1];

            // skip near-duplicate points
            if (k > 0 && Math.abs(x - xp) <= EPSILON && Math.abs(y - yp) <= EPSILON) continue;
            xp = x;
            yp = y;

            // skip seed triangle points
            if (i === i0 || i === i1 || i === i2) continue;

            // find a visible edge on the convex hull using edge hash
            let start = 0;
            for (let j = 0, key = this._hashKey(x, y); j < this._hashSize; j++) {
                start = hullHash[(key + j) % this._hashSize];
                if (start !== -1 && start !== hullNext[start]) break;
            }

            start = hullPrev[start];
            let e = start, q;
            while (q = hullNext[e], orient2d(x, y, coords[2 * e], coords[2 * e + 1], coords[2 * q], coords[2 * q + 1]) >= 0) {
                e = q;
                if (e === start) {
                    e = -1;
                    break;
                }
            }
            if (e === -1) continue; // likely a near-duplicate point; skip it

            // add the first triangle from the point
            let t = this._addTriangle(e, i, hullNext[e], -1, -1, hullTri[e]);

            // recursively flip triangles from the point until they satisfy the Delaunay condition
            hullTri[i] = this._legalize(t + 2);
            hullTri[e] = t; // keep track of boundary triangles on the hull
            hullSize++;

            // walk forward through the hull, adding more triangles and flipping recursively
            let n = hullNext[e];
            while (q = hullNext[n], orient2d(x, y, coords[2 * n], coords[2 * n + 1], coords[2 * q], coords[2 * q + 1]) < 0) {
                t = this._addTriangle(n, i, q, hullTri[i], -1, hullTri[n]);
                hullTri[i] = this._legalize(t + 2);
                hullNext[n] = n; // mark as removed
                hullSize--;
                n = q;
            }

            // walk backward from the other side, adding more triangles and flipping
            if (e === start) {
                while (q = hullPrev[e], orient2d(x, y, coords[2 * q], coords[2 * q + 1], coords[2 * e], coords[2 * e + 1]) < 0) {
                    t = this._addTriangle(q, i, e, -1, hullTri[e], hullTri[q]);
                    this._legalize(t + 2);
                    hullTri[q] = t;
                    hullNext[e] = e; // mark as removed
                    hullSize--;
                    e = q;
                }
            }

            // update the hull indices
            this._hullStart = hullPrev[i] = e;
            hullNext[e] = hullPrev[n] = i;
            hullNext[i] = n;

            // save the two new edges in the hash table
            hullHash[this._hashKey(x, y)] = i;
            hullHash[this._hashKey(coords[2 * e], coords[2 * e + 1])] = e;
        }

        this.hull = new Uint32Array(hullSize);
        for (let i = 0, e = this._hullStart; i < hullSize; i++) {
            this.hull[i] = e;
            e = hullNext[e];
        }

        // trim typed triangle mesh arrays
        this.triangles = this._triangles.subarray(0, this.trianglesLen);
        this.halfedges = this._halfedges.subarray(0, this.trianglesLen);
    }

    /**
     * Removes an interior point from the current triangulation, patching only the ring of
     * triangles around it (its star neighborhood). The `coords` array is left untouched and all
     * remaining triangle vertices keep referencing the original input indices. Removing a convex
     * hull vertex is not supported.
     *
     * Returns `true` when the point was part of the triangulation and got removed, and `false`
     * when the index is valid but the point is not part of the current triangulation (e.g. it was
     * already removed). Throws a `RangeError` on a non-integer, out-of-bounds index, or for a
     * current convex hull vertex; in that case the triangulation is left unchanged and usable.
     *
     * A later `update()` rebuilds from the full `coords` and restores removed points.
     *
     * @param {number} index
     * @returns {boolean}
     */
    removePoint(index) {
        if (!Number.isInteger(index) || index < 0 || index >= (this.coords.length >> 1)) {
            throw new RangeError(`Invalid point index: ${index}`);
        }
        if (this._removed.has(index)) return false;

        const {_triangles: oldTriangles, halfedges: oldHalfedges, hull} = this;

        // locate a half-edge (and its triangle) incident on the target point
        let edge = -1;
        for (let i = 0; i < this.trianglesLen; i++) {
            if (oldTriangles[i] === index) {
                edge = i;
                break;
            }
        }
        if (edge === -1) {
            // not part of the current triangulation (e.g. a duplicate point skipped on construction)
            return false;
        }

        // convex hull vertices can't be removed through this local patch
        for (let i = 0; i < hull.length; i++) {
            if (hull[i] === index) throw new RangeError(`Removing convex hull vertex ${index} is not supported.`);
        }

        // collect the boundary of the star neighborhood as an ordered ring of neighbor points;
        // each boundary edge joins ring[k] and ring[k + 1]. An interior point has no hull edge,
        // so every crossed half-edge is always defined.
        const ring = [];
        const boundary = []; // for each k, the outside half-edge (oriented ring[k + 1] -> ring[k]) in old ids
        const removedTriangles = new Set();
        let e = edge;
        do {
            const t = e - e % 3;
            ring.push(oldTriangles[t + (e + 1) % 3]); // neighbor following the target point
            boundary.push(oldHalfedges[t + (e + 1) % 3]); // twin of the edge opposite the target
            removedTriangles.add(t); // the star triangle itself gets removed
            e = oldHalfedges[t + (e + 2) % 3]; // cross the other edge incident to the target point
        } while (e !== edge);

        const m = ring.length;

        // build a compacted copy of the mesh without the star triangles, remapping half-edge ids.
        // the backing arrays keep their original capacity (update() relies on it); only
        // `trianglesLen` marks the active prefix.
        const oldLen = this.trianglesLen;
        const capacity = oldTriangles.length;
        const newTriangles = new Uint32Array(capacity);
        const newHalfedges = new Int32Array(capacity);
        const edgeMap = new Int32Array(oldLen).fill(-2);
        const oldIndex = new Uint32Array(oldLen);
        let pos = 0;
        for (let t = 0; t < oldLen; t += 3) {
            if (removedTriangles.has(t)) continue;
            for (let j = 0; j < 3; j++) {
                edgeMap[t + j] = pos + j;
                oldIndex[pos + j] = t + j;
            }
            newTriangles[pos] = oldTriangles[t];
            newTriangles[pos + 1] = oldTriangles[t + 1];
            newTriangles[pos + 2] = oldTriangles[t + 2];
            pos += 3;
        }
        for (let i = 0; i < pos; i++) {
            const twin = oldHalfedges[oldIndex[i]];
            newHalfedges[i] = twin === -1 ? -1 : edgeMap[twin];
        }
        // remap the cavity boundary half-edges to the compacted mesh (-1 marks a hull edge:
        // a star triangle's opposite edge can itself lie on the convex hull)
        for (let i = 0; i < m; i++) {
            const b = boundary[i];
            boundary[i] = b === -1 ? -1 : edgeMap[b];
        }

        // retriangulate the cavity polygon locally: ear clipping for arbitrary (possibly
        // non-convex) rings, then edge flips to restore the local Delaunay condition
        this._triangles = newTriangles;
        this._halfedges = newHalfedges;
        this.trianglesLen = pos;
        this.triangles = newTriangles;
        this.halfedges = newHalfedges;

        this._fillCavity(ring, boundary, pos);

        this.triangles = this._triangles.subarray(0, this.trianglesLen);
        this.halfedges = this._halfedges.subarray(0, this.trianglesLen);
        this._removed.add(index);

        return true;
    }

    /**
     * Retriangulate the cavity polygon left by a removed point.
     *
     * @param {number[]} ring cavity vertices in counterclockwise order
     * @param {number[]} boundary for each k, the half-edge outside the cavity on ring edge
     * `ring[k] -> ring[k + 1]` (it is oriented `ring[k + 1] -> ring[k]`), in compacted ids
     * @param {number} prefix length of the compacted mesh before cavity triangles get appended
     * @private
     */
    _fillCavity(ring, boundary, prefix) {
        const {coords} = this;
        const m = ring.length;

        // active polygon as a singly linked list over ring positions
        const next = new Int32Array(m);
        for (let i = 0; i < m; i++) {
            next[i] = (i + 1) % m;
        }
        let active = m;

        const orient = (i, j, k) => orient2d(
            coords[2 * ring[i]], coords[2 * ring[i] + 1],
            coords[2 * ring[j]], coords[2 * ring[j] + 1],
            coords[2 * ring[k]], coords[2 * ring[k] + 1]);

        // clip ears until only one triangle remains
        let guard = 0;
        let i = 0;
        while (active > 3 && guard++ < m * m) {
            const j = next[i];
            const k = next[j];

            if (this._isEar(ring, next, active, orient, i, j, k)) {
                this._addCavityTriangle(ring[i], ring[j], ring[k],
                    boundary[i], boundary[j], -1);
                // slot 2 is the new diagonal (ring[k] -> ring[i]) bounding the remaining polygon
                boundary[i] = this.trianglesLen - 1;
                next[i] = k;
                active--;
                i = k;
            } else {
                i = j;
            }
        }
        if (active === 3) {
            const j = next[i];
            const k = next[j];
            this._addCavityTriangle(ring[i], ring[j], ring[k],
                boundary[i], boundary[j], boundary[k]);
        }

        // restore the Delaunay condition on the new diagonals; cavity boundary edges can't flip
        this._legalizeCavity(prefix);
    }

    /**
     * Whether the triangle (i, j, k) is an ear of the active polygon: strictly convex at j and
     * empty of other active vertices.
     *
     * @private
     */
    _isEar(ring, next, active, orient, i, j, k) {
        if (orient(i, j, k) <= 0) return false; // reflex or degenerate corner

        let p = next[k];
        for (let count = 0; count < active - 3; count++, p = next[p]) {
            if (pointInTriangle(this.coords, ring[p], ring[i], ring[j], ring[k])) return false;
        }
        return true;
    }

    /**
     * Append a cavity triangle, growing the compacted mesh arrays as needed.
     *
     * @private
     */
    _addCavityTriangle(i0, i1, i2, a, b, c) {
        if (this.trianglesLen + 3 > this._triangles.length) {
            const newLength = Math.max(this._triangles.length * 2, this.trianglesLen + 3);
            const triangles = new Uint32Array(newLength);
            const halfedges = new Int32Array(newLength);
            triangles.set(this._triangles.subarray(0, this.trianglesLen));
            halfedges.set(this._halfedges.subarray(0, this.trianglesLen));
            this._triangles = triangles;
            this._halfedges = halfedges;
        }
        const t = this.trianglesLen;
        this._triangles[t] = i0;
        this._triangles[t + 1] = i1;
        this._triangles[t + 2] = i2;
        this._link(t, a);
        this._link(t + 1, b);
        this._link(t + 2, c);
        this.trianglesLen += 3;
    }

    /**
     * Flip interior diagonals of the retriangulated cavity until every shared edge satisfies the
     * local Delaunay condition. Cavity triangles are everything appended after the compacted
     * surviving prefix; an edge whose twin leaves the cavity is a boundary edge and never flips,
     * so the hull bookkeeping in `_legalize` is never needed.
     *
     * @param {number} prefix length of the compacted mesh before cavity triangles were appended
     * @private
     */
    _legalizeCavity(prefix) {
        const {coords, _triangles: triangles, _halfedges: halfedges} = this;

        const stack = [];
        for (let e = prefix; e < this.trianglesLen; e++) {
            const twin = halfedges[e];
            if (twin !== -1 && twin >= prefix) stack.push(e); // interior diagonal only
        }
        const queued = new Set(stack);

        while (stack.length) {
            const a = stack.pop();
            queued.delete(a);
            const b = halfedges[a];
            if (b === -1 || b < prefix) continue; // cavity boundary edge: never flip

            const a0 = a - a % 3;
            const ar = a0 + (a + 2) % 3;
            const b0 = b - b % 3;
            const al = a0 + (a + 1) % 3;
            const bl = b0 + (b + 2) % 3;

            const p0 = triangles[ar];
            const pr = triangles[a];
            const pl = triangles[al];
            const p1 = triangles[bl];

            const illegal = incircle(
                coords[2 * p0], coords[2 * p0 + 1],
                coords[2 * pr], coords[2 * pr + 1],
                coords[2 * pl], coords[2 * pl + 1],
                coords[2 * p1], coords[2 * p1 + 1]) < 0;

            if (!illegal) continue;

            triangles[a] = p1;
            triangles[b] = p0;

            const hbl = halfedges[bl];
            this._link(a, hbl);
            this._link(b, halfedges[ar]);
            this._link(ar, bl);

            // a flip changes the opposite vertex on each edge of the quad, so re-check every
            // edge of the two flipped triangles (the Lawson queue always terminates because
            // each strict flip improves the global angle ordering)
            for (let q = 0; q < 3; q++) {
                for (const e of [a0 + q, b0 + q]) {
                    const twin = halfedges[e];
                    if (twin !== -1 && twin >= prefix && !queued.has(e)) {
                        stack.push(e);
                        queued.add(e);
                    }
                }
            }
        }
    }

    /**
     * Calculate an angle-based key for the edge hash used for advancing convex hull.
     *
     * @param {number} x
     * @param {number} y
     * @private
     */
    _hashKey(x, y) {
        return Math.floor(pseudoAngle(x - this._cx, y - this._cy) * this._hashSize) % this._hashSize;
    }

    /**
     * Flip an edge in a pair of triangles if it doesn't satisfy the Delaunay condition.
     *
     * @param {number} a
     * @private
     */
    _legalize(a) {
        const {_triangles: triangles, _halfedges: halfedges, coords} = this;

        let i = 0;
        let ar = 0;

        // recursion eliminated with a fixed-size stack
        while (true) {
            const b = halfedges[a];

            /* if the pair of triangles doesn't satisfy the Delaunay condition
             * (p1 is inside the circumcircle of [p0, pl, pr]), flip them,
             * then do the same check/flip recursively for the new pair of triangles
             *
             *           pl                    pl
             *          /||\                  /  \
             *       al/ || \bl            al/    \a
             *        /  ||  \              /      \
             *       /  a||b  \    flip    /___ar___\
             *     p0\   ||   /p1   =>   p0\---bl---/p1
             *        \  ||  /              \      /
             *       ar\ || /br             b\    /br
             *          \||/                  \  /
             *           pr                    pr
             */
            const a0 = a - a % 3;
            ar = a0 + (a + 2) % 3;

            if (b === -1) { // convex hull edge
                if (i === 0) break;
                a = EDGE_STACK[--i];
                continue;
            }

            const b0 = b - b % 3;
            const al = a0 + (a + 1) % 3;
            const bl = b0 + (b + 2) % 3;

            const p0 = triangles[ar];
            const pr = triangles[a];
            const pl = triangles[al];
            const p1 = triangles[bl];

            const illegal = inCircle(
                coords[2 * p0], coords[2 * p0 + 1],
                coords[2 * pr], coords[2 * pr + 1],
                coords[2 * pl], coords[2 * pl + 1],
                coords[2 * p1], coords[2 * p1 + 1]);

            if (illegal) {
                triangles[a] = p1;
                triangles[b] = p0;

                const hbl = halfedges[bl];

                // edge swapped on the other side of the hull (rare); fix the half-edge reference
                if (hbl === -1) {
                    let e = this._hullStart;
                    do {
                        if (this._hullTri[e] === bl) {
                            this._hullTri[e] = a;
                            break;
                        }
                        e = this._hullPrev[e];
                    } while (e !== this._hullStart);
                }
                this._link(a, hbl);
                this._link(b, halfedges[ar]);
                this._link(ar, bl);

                const br = b0 + (b + 1) % 3;

                // don't worry about hitting the cap: it can only happen on extremely degenerate input
                if (i < EDGE_STACK.length) {
                    EDGE_STACK[i++] = br;
                }
            } else {
                if (i === 0) break;
                a = EDGE_STACK[--i];
            }
        }

        return ar;
    }

    /**
     * Link two half-edges to each other.
     * @param {number} a
     * @param {number} b
     * @private
     */
    _link(a, b) {
        this._halfedges[a] = b;
        if (b !== -1) this._halfedges[b] = a;
    }

    /**
     * Add a new triangle given vertex indices and adjacent half-edge ids.
     *
     * @param {number} i0
     * @param {number} i1
     * @param {number} i2
     * @param {number} a
     * @param {number} b
     * @param {number} c
     * @private
     */
    _addTriangle(i0, i1, i2, a, b, c) {
        const t = this.trianglesLen;

        this._triangles[t] = i0;
        this._triangles[t + 1] = i1;
        this._triangles[t + 2] = i2;

        this._link(t, a);
        this._link(t + 1, b);
        this._link(t + 2, c);

        this.trianglesLen += 3;

        return t;
    }
}

/**
 * Monotonically increases with real angle, but doesn't need expensive trigonometry.
 *
 * @param {number} dx
 * @param {number} dy
 */
function pseudoAngle(dx, dy) {
    const p = dx / (Math.abs(dx) + Math.abs(dy) || 1);
    return (dy > 0 ? 3 - p : 1 + p) / 4; // [0..1]
}

/**
 * Check whether point P is strictly inside the triangle A, B, C
 * (points on the boundary are considered outside). Independent of the triangle winding.
 *
 * @param {ArrayLike<number>} coords
 * @param {number} p
 * @param {number} a
 * @param {number} b
 * @param {number} c
 */
function pointInTriangle(coords, p, a, b, c) {
    const px = coords[2 * p];
    const py = coords[2 * p + 1];
    const o1 = orient2d(coords[2 * a], coords[2 * a + 1], coords[2 * b], coords[2 * b + 1], px, py);
    const o2 = orient2d(coords[2 * b], coords[2 * b + 1], coords[2 * c], coords[2 * c + 1], px, py);
    const o3 = orient2d(coords[2 * c], coords[2 * c + 1], coords[2 * a], coords[2 * a + 1], px, py);
    return o1 > 0 && o2 > 0 && o3 > 0 || o1 < 0 && o2 < 0 && o3 < 0;
}

/**
 * Squared distance between two points.
 *
 * @param {number} ax
 * @param {number} ay
 * @param {number} bx
 * @param {number} by
 */
function dist(ax, ay, bx, by) {
    const dx = ax - bx;
    const dy = ay - by;
    return dx * dx + dy * dy;
}

/**
 * Check whether point P is inside a circle formed by points A, B, C.
 *
 * @param {number} ax
 * @param {number} ay
 * @param {number} bx
 * @param {number} by
 * @param {number} cx
 * @param {number} cy
 * @param {number} px
 * @param {number} py
 */
function inCircle(ax, ay, bx, by, cx, cy, px, py) {
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
           ap * (ex * fy - ey * fx) < 0;
}

/**
 * Squared radius of the circle formed by points A, B, C.
 *
 * @param {number} ax
 * @param {number} ay
 * @param {number} bx
 * @param {number} by
 * @param {number} cx
 * @param {number} cy
 */
function circumradius(ax, ay, bx, by, cx, cy) {
    const dx = bx - ax;
    const dy = by - ay;
    const ex = cx - ax;
    const ey = cy - ay;

    const bl = dx * dx + dy * dy;
    const cl = ex * ex + ey * ey;
    const d = 0.5 / (dx * ey - dy * ex);

    const x = (ey * bl - dy * cl) * d;
    const y = (dx * cl - ex * bl) * d;

    return x * x + y * y;
}

/**
 * Get coordinates of a circumcenter for points A, B, C.
 *
 * @param {number} ax
 * @param {number} ay
 * @param {number} bx
 * @param {number} by
 * @param {number} cx
 * @param {number} cy
 */
function circumcenter(ax, ay, bx, by, cx, cy) {
    const dx = bx - ax;
    const dy = by - ay;
    const ex = cx - ax;
    const ey = cy - ay;

    const bl = dx * dx + dy * dy;
    const cl = ex * ex + ey * ey;
    const d = 0.5 / (dx * ey - dy * ex);

    const x = ax + (ey * bl - dy * cl) * d;
    const y = ay + (dx * cl - ex * bl) * d;

    return {x, y};
}

/**
 * Sort points by distance via an array of point indices and an array of calculated distances.
 *
 * @param {Uint32Array} ids
 * @param {Float64Array} dists
 * @param {number} left
 * @param {number} right
 */
function quicksort(ids, dists, left, right) {
    if (right - left <= 20) {
        for (let i = left + 1; i <= right; i++) {
            const temp = ids[i];
            const tempDist = dists[temp];
            let j = i - 1;
            while (j >= left && dists[ids[j]] > tempDist) ids[j + 1] = ids[j--];
            ids[j + 1] = temp;
        }
    } else {
        const median = (left + right) >> 1;
        let i = left + 1;
        let j = right;
        swap(ids, median, i);
        if (dists[ids[left]] > dists[ids[right]]) swap(ids, left, right);
        if (dists[ids[i]] > dists[ids[right]]) swap(ids, i, right);
        if (dists[ids[left]] > dists[ids[i]]) swap(ids, left, i);

        const temp = ids[i];
        const tempDist = dists[temp];
        while (true) {
            do i++; while (dists[ids[i]] < tempDist);
            do j--; while (dists[ids[j]] > tempDist);
            if (j < i) break;
            swap(ids, i, j);
        }
        ids[left + 1] = ids[j];
        ids[j] = temp;

        if (right - i + 1 >= j - left) {
            quicksort(ids, dists, i, right);
            quicksort(ids, dists, left, j - 1);
        } else {
            quicksort(ids, dists, left, j - 1);
            quicksort(ids, dists, i, right);
        }
    }
}

/**
 * @param {Uint32Array} arr
 * @param {number} i
 * @param {number} j
 */
function swap(arr, i, j) {
    const tmp = arr[i];
    arr[i] = arr[j];
    arr[j] = tmp;
}

/** @param {[number, number]} p */
function defaultGetX(p) {
    return p[0];
}
/** @param {[number, number]} p */
function defaultGetY(p) {
    return p[1];
}
