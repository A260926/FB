'use strict'

/**
 * Independent spatial reference for the JSCAD boolean regression tests.
 *
 * Everything in this module is computed from first principles over plain
 * axis-aligned boxes. It never calls into the library under test, never uses
 * measureVolume / BSP / retessellate, and never shells out to another CSG
 * implementation.
 *
 * Reference method
 * ----------------
 * 1. A model is a finite boolean expression (union / intersect / subtract)
 *    whose leaves are axis-aligned cuboids.
 * 2. The boundary planes (one per box face, six coordinate values per box)
 *    split R^3 into open axis-aligned grid cells. Inside a cell the truth
 *    value of the expression is constant, so the occupied volume is obtained
 *    by summing the volume of every cell whose center evaluates to true.
 *    Grid cells are open regions; pieces of solid that lie exactly on a grid
 *    plane have zero volume and contribute nothing, so face-contact cases
 *    (sharing a whole face, touching edges, identical boxes) are handled
 *    without special casing.
 * 3. Point inside/outside answers come from evaluating the same expression at
 *    an arbitrary point (also defined everywhere except on box boundaries).
 *
 * Deliberately excluded
 * ---------------------
 * - probes and cells on box boundary planes (the predicate there is
 *   convention-dependent; such sets have zero volume anyway)
 * - curved primitives, rotated boxes and non-affine transforms
 * - singular / non-invertible transforms
 * - polygon winding and tessellation: the reference only describes occupied
 *   space; winding is checked separately from the library output.
 */

const OPS = Object.freeze({ UNION: 'union', INTERSECT: 'intersect', SUBTRACT: 'subtract' })

/**
 * Define an axis-aligned box.
 * @param {Array} min - [xmin, ymin, zmin]
 * @param {Array} max - [xmax, ymax, zmax]
 */
const box = (min, max) => ({ min: min.slice(), max: max.slice() })

/** Build a boolean expression node. Leaves are plain boxes. */
const expr = (op, left, right) => ({ op, left, right })

const pointInBox = (box, p) =>
  p[0] > box.min[0] && p[0] < box.max[0] &&
  p[1] > box.min[1] && p[1] < box.max[1] &&
  p[2] > box.min[2] && p[2] < box.max[2]

/** Evaluate the boolean expression at an arbitrary point (not on a boundary). */
const evaluate = (node, p) => {
  if (node.op === undefined) return pointInBox(node, p)
  const l = evaluate(node.left, p)
  if (node.op === OPS.UNION) return l || evaluate(node.right, p)
  if (node.op === OPS.INTERSECT) return l && evaluate(node.right, p)
  if (node.op === OPS.SUBTRACT) return l && !evaluate(node.right, p)
  throw new Error(`unknown boolean op ${node.op}`)
}

const collectBoxes = (node, out = []) => {
  if (node.op === undefined) { out.push(node); return out }
  collectBoxes(node.left, out)
  collectBoxes(node.right, out)
  return out
}

const sortedUnique = (values) => {
  const sorted = values.slice().sort((a, b) => a - b)
  const out = []
  for (const v of sorted) {
    if (out.length === 0 || Math.abs(v - out[out.length - 1]) > 1e-12) out.push(v)
  }
  return out
}

/**
 * Reference occupied volume of the expression, computed by exact grid
 * decomposition: the box boundary planes partition space into open cells and
 * each cell is classified once at its center.
 */
const referenceVolume = (root) => {
  const boxes = collectBoxes(root)
  const cuts = [[], [], []]
  for (const b of boxes) {
    for (let axis = 0; axis < 3; axis++) {
      cuts[axis].push(b.min[axis], b.max[axis])
    }
  }
  const grids = cuts.map(sortedUnique)
  let volume = 0
  for (let xi = 0; xi < grids[0].length - 1; xi++) {
    const cx = (grids[0][xi] + grids[0][xi + 1]) / 2
    const dx = grids[0][xi + 1] - grids[0][xi]
    for (let yi = 0; yi < grids[1].length - 1; yi++) {
      const cy = (grids[1][yi] + grids[1][yi + 1]) / 2
      const dy = grids[1][yi + 1] - grids[1][yi]
      for (let zi = 0; zi < grids[2].length - 1; zi++) {
        const cz = (grids[2][zi] + grids[2][zi + 1]) / 2
        if (evaluate(root, [cx, cy, cz])) {
          volume += dx * dy * (grids[2][zi + 1] - grids[2][zi])
        }
      }
    }
  }
  return volume
}

/**
 * Throw if the point lands on any box boundary plane or at a box corner;
 * reference predicates are only defined for open interiors / exteriors.
 */
const assertOffBoundaries = (root, p) => {
  const boxes = collectBoxes(root)
  for (const b of boxes) {
    for (let axis = 0; axis < 3; axis++) {
      if (Math.abs(p[axis] - b.min[axis]) < 1e-9 || Math.abs(p[axis] - b.max[axis]) < 1e-9) {
        throw new Error(`probe ${p.join(',')} sits on a boundary plane of box ${JSON.stringify(b)}`)
      }
    }
  }
}

/** Reference inside/outside answer for a probe given in model space. */
const referenceContains = (root, p) => {
  assertOffBoundaries(root, p)
  return evaluate(root, p)
}

/**
 * Determinant of a column-major 4x4 affine matrix (upper-left 3x3 block;
 * the translation terms never enter a determinant).
 */
const mat4Determinant = (m) => {
  const a00 = m[0], a01 = m[1], a02 = m[2]
  const a10 = m[4], a11 = m[5], a12 = m[6]
  const a20 = m[8], a21 = m[9], a22 = m[10]
  return (
    a00 * (a11 * a22 - a12 * a21) -
    a01 * (a10 * a22 - a12 * a20) +
    a02 * (a10 * a21 - a11 * a20)
  )
}

/** Apply a column-major affine mat4 to a point. */
const transformPoint = (m, p) => [
  m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
  m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
  m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]
]

const sub3 = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
const cross3 = (a, b) => [
  a[1] * b[2] - a[2] * b[1],
  a[2] * b[0] - a[0] * b[2],
  a[0] * b[1] - a[1] * b[0]
]
const dot3 = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
const len3 = (a) => Math.hypot(a[0], a[1], a[2])

/**
 * Fan triangulation of a convex polygon's vertex ring. The library's output
 * polygons are convex, so a fan around vertex 0 is always valid.
 */
const triangulate = (vertices) => {
  const triangles = []
  for (let i = 1; i < vertices.length - 1; i++) {
    triangles.push([vertices[0], vertices[i], vertices[i + 1]])
  }
  return triangles
}

/**
 * Moller-Trumbore ray/triangle intersection. Returns the positive ray
 * parameter t, or null when the ray misses the triangle (parallel / hits
 * behind the origin / outside its edges).
 */
const rayTriangle = (origin, dir, tri) => {
  const edge1 = sub3(tri[1], tri[0])
  const edge2 = sub3(tri[2], tri[0])
  const pvec = cross3(dir, edge2)
  const det = dot3(edge1, pvec)
  if (Math.abs(det) < 1e-12) return null
  const invDet = 1 / det
  const tvec = sub3(origin, tri[0])
  const u = dot3(tvec, pvec) * invDet
  if (u < -1e-10 || u > 1 + 1e-10) return null
  const qvec = cross3(tvec, edge1)
  const v = dot3(dir, qvec) * invDet
  if (v < -1e-10 || u + v > 1 + 1e-10) return null
  const t = dot3(edge2, qvec) * invDet
  return t > 1e-9 ? t : null
}

/**
 * Point-in-solid query against a list of convex polygons, implemented from
 * scratch with a ray cast over the fan-triangulated polygons.
 *
 * Intersections that share the same ray distance (e.g. the common edge of the
 * two triangles a quad was split into, or coincident coplanar faces) are
 * merged before parity is counted, so shared edges are never double counted.
 */
const pointInPolygons = (polygons, origin, rayDir = [0.371137, 0.871231, 1.0]) => {
  const dirLen = len3(rayDir)
  const dir = rayDir.map((v) => v / dirLen)
  const hits = []
  for (const polygon of polygons) {
    for (const tri of triangulate(polygon.vertices)) {
      const t = rayTriangle(origin, dir, tri)
      if (t !== null) hits.push(t)
    }
  }
  const merged = []
  for (const t of hits.sort((a, b) => a - b)) {
    if (merged.length === 0 || Math.abs(t - merged[merged.length - 1]) > 1e-7) merged.push(t)
  }
  return merged.length % 2 === 1
}

/**
 * Signed volume of a closed polygon soup, accumulated independently from the
 * triangle fan of each convex polygon using the divergence-theorem formula
 * (sum of dot(v0, cross(v1, v2)) / 6). A correctly oriented closed solid
 * returns a positive value regardless of mirroring, because mirrored polygon
 * vertex rings are flipped; a wrong inward-facing cavity wall shows up here
 * even when the axis-aligned bounding box still looks right.
 */
const signedVolumeOfPolygons = (polygons) => {
  let volume = 0
  for (const polygon of polygons) {
    for (const tri of triangulate(polygon.vertices)) {
      volume += dot3(tri[0], cross3(tri[1], tri[2])) / 6
    }
  }
  return volume
}

module.exports = {
  OPS,
  box,
  expr,
  evaluate,
  referenceVolume,
  referenceContains,
  mat4Determinant,
  transformPoint,
  triangulate,
  pointInPolygons,
  signedVolumeOfPolygons
}
