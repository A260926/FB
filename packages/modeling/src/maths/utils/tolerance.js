/*
 * Scale aware numerical tolerance for the 3D boolean / retessellation pipeline.
 *
 * A single absolute epsilon cannot both resolve features that are tiny compared
 * to the working coordinates (for example a 2^-20 thick slab next to a 2^20
 * solid) and keep two distinct surfaces separated.  Instead, every comparison
 * derives a tolerance from the magnitude of the coordinates taking part in
 * that comparison.  A few tens of Number ulps is enough to absorb the rounding
 * of the arithmetic performed there, while remaining far below the smallest
 * feature the boolean operations promise to resolve.
 *
 * NOTE: this is internal to the boolean pipeline. The global modelling epsilon
 * (maths/constants.js) is intentionally left untouched.
 */

// Number ulps allowed by a comparison. Large enough for a handful of
// multiplies / adds / normalizations, small enough (2^7) that two surfaces
// 2^-20 apart at coordinate magnitude 2^20 are still distinct.
const ULPS = 128

// Largeness of the doubles involved in a comparison.
const scaleOfVertex = (vertex) => Math.max(Math.abs(vertex[0]), Math.abs(vertex[1]), Math.abs(vertex[2]))

/*
 * Tolerance for coordinates of the given magnitude.
 */
const toleranceForScale = (scale) => ULPS * Number.EPSILON * scale

/*
 * Tolerance for a set of vertices, optionally taking a plane offset into account.
 */
const toleranceForPoints = (vertices, plane) => {
  let scale = 0
  for (let i = 0; i < vertices.length; i++) {
    const vertexScale = scaleOfVertex(vertices[i])
    if (vertexScale > scale) scale = vertexScale
  }
  if (plane) {
    const planeScale = Math.abs(plane[3])
    if (planeScale > scale) scale = planeScale
  }
  return toleranceForScale(scale)
}

/*
 * Tolerance for a comparison between two vertex sets.
 */
const toleranceForVertexSets = (verticesA, verticesB, planeA, planeB) => {
  let scale = 0
  for (let i = 0; i < verticesA.length; i++) {
    const vertexScale = scaleOfVertex(verticesA[i])
    if (vertexScale > scale) scale = vertexScale
  }
  for (let i = 0; i < verticesB.length; i++) {
    const vertexScale = scaleOfVertex(verticesB[i])
    if (vertexScale > scale) scale = vertexScale
  }
  if (planeA) {
    const planeScale = Math.abs(planeA[3])
    if (planeScale > scale) scale = planeScale
  }
  if (planeB) {
    const planeScale = Math.abs(planeB[3])
    if (planeScale > scale) scale = planeScale
  }
  return toleranceForScale(scale)
}

/*
 * Tolerance for a set of 2D vertices.
 */
const toleranceForPoint2s = (points) => {
  let scale = 0
  for (let i = 0; i < points.length; i++) {
    const pointScale = Math.max(Math.abs(points[i][0]), Math.abs(points[i][1]))
    if (pointScale > scale) scale = pointScale
  }
  return toleranceForScale(scale)
}

/*
 * Largest distance of the given vertices from the given plane.
 *
 * For a polygon with a large aspect ratio (a thin face) the computed normal
 * carries a relative error of order eps * aspect, so the polygon's own vertices
 * do not sit exactly on its computed plane. That residual is the actual noise
 * floor of the plane, and comparisons against it must not be finer than it.
 */
const planeResidual = (plane, vertices) => {
  let residual = 0
  for (let i = 0; i < vertices.length; i++) {
    const distance = Math.abs(plane[0] * vertices[i][0] + plane[1] * vertices[i][1] + plane[2] * vertices[i][2] - plane[3])
    if (distance > residual) residual = distance
  }
  return residual
}

module.exports = {
  ULPS,
  toleranceForScale,
  toleranceForPoints,
  toleranceForVertexSets,
  toleranceForPoint2s,
  planeResidual
}
