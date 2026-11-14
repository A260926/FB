const vec3 = require('../../../maths/vec3')

const poly3 = require('../../../geometries/poly3')

const { toleranceForPoints, toleranceForVertexSets, planeResidual } = require('../../../maths/utils/tolerance')

const splitLineSegmentByPlane = require('./splitLineSegmentByPlane')

const { isLegacyOptions } = require('../booleanOptions')

const { EPS } = require('../../../maths/constants')

// Remove consecutive duplicate vertices from a polygon vertex list.
// Compares last vertex to first to handle wraparound.
// Returns a new array (does not modify input).
// IMPORTANT: Caller must ensure vertices.length >= 3 before calling.
const removeConsecutiveDuplicates = (vertices, tolerance) => {
  const toleranceSquared = tolerance * tolerance
  const result = []
  let prevvertex = vertices[vertices.length - 1]
  for (let i = 0; i < vertices.length; i++) {
    const vertex = vertices[i]
    if (vec3.squaredDistance(vertex, prevvertex) >= toleranceSquared) {
      result.push(vertex)
    }
    prevvertex = vertex
  }
  return result
}

// Returns object:
// .type:
//   0: coplanar-front
//   1: coplanar-back
//   2: front
//   3: back
//   4: spanning
// In case the polygon is spanning, returns:
// .front: a Polygon3 of the front part
// .back: a Polygon3 of the back part
const splitPolygonByPlane = (splane, polygon, splittingPlaneResidual, options) => {
  const legacy = isLegacyOptions(options)
  const result = {
    type: null,
    front: null,
    back: null
  }
  // cache in local lets (speedup):
  const vertices = polygon.vertices
  const numvertices = vertices.length
  const pplane = poly3.plane(polygon)
  if (planeEquals(pplane, splane)) {
    result.type = 0
  } else {
    // All comparisons are done relative to the magnitude of the coordinates
    // involved, so that features which are tiny compared to the working
    // coordinates are still resolved. The legacy mode keeps the historical
    // absolute epsilon, which the 2D boolean path still relies upon.
    //
    // A polygon's own plane is only as accurate as its geometry allows: for a
    // very thin face (large aspect ratio) the computed normal carries a
    // relative error of order eps / aspect, so its own vertices sit a little
    // off the plane. That residual is the noise level of this polygon, and
    // comparing its vertices to any other plane must not be finer than it,
    // otherwise vertices which are meant to be coplanar get classified on
    // different sides and produce zero area slivers.
    const scaleTolerance = toleranceForVertexSets(vertices, vertices, pplane, splane)
    // The comparison can be no finer than the noise of either plane involved:
    // the polygon's own plane fit, and the splitting plane's fit.
    const tolerance = legacy
      ? EPS
      : Math.max(scaleTolerance, planeResidual(pplane, vertices), splittingPlaneResidual || 0)
    let hasfront = false
    let hasback = false
    const vertexIsBack = []
    const MINEPS = -tolerance
    for (let i = 0; i < numvertices; i++) {
      const t = vec3.dot(splane, vertices[i]) - splane[3]
      const isback = (t < MINEPS)
      vertexIsBack.push(isback)
      if (t > tolerance) hasfront = true
      if (t < MINEPS) hasback = true
    }
    if ((!hasfront) && (!hasback)) {
      // all points coplanar
      const t = vec3.dot(splane, pplane)
      result.type = (t >= 0) ? 0 : 1
    } else if (!hasback) {
      result.type = 2
    } else if (!hasfront) {
      result.type = 3
    } else {
      // spanning
      result.type = 4
      const frontvertices = []
      const backvertices = []
      let isback = vertexIsBack[0]
      for (let vertexindex = 0; vertexindex < numvertices; vertexindex++) {
        const vertex = vertices[vertexindex]
        let nextvertexindex = vertexindex + 1
        if (nextvertexindex >= numvertices) nextvertexindex = 0
        const nextisback = vertexIsBack[nextvertexindex]
        if (isback === nextisback) {
          // line segment is on one side of the plane:
          if (isback) {
            backvertices.push(vertex)
          } else {
            frontvertices.push(vertex)
          }
        } else {
          // line segment intersects plane:
          const nextpoint = vertices[nextvertexindex]
          const intersectionpoint = splitLineSegmentByPlane(splane, vertex, nextpoint)
          if (isback) {
            backvertices.push(vertex)
            backvertices.push(intersectionpoint)
            frontvertices.push(intersectionpoint)
          } else {
            frontvertices.push(vertex)
            frontvertices.push(intersectionpoint)
            backvertices.push(intersectionpoint)
          }
        }
        isback = nextisback
      } // for vertexindex
      // remove consecutive duplicate vertices (check length before calling to avoid function overhead)
      if (frontvertices.length >= 3) {
        const frontFiltered = removeConsecutiveDuplicates(frontvertices, legacy ? EPS : toleranceForPoints(frontvertices, pplane))
        if (frontFiltered.length >= 3) {
          result.front = poly3.fromPointsAndPlane(frontFiltered, pplane)
        }
      }
      if (backvertices.length >= 3) {
        const backFiltered = removeConsecutiveDuplicates(backvertices, legacy ? EPS : toleranceForPoints(backvertices, pplane))
        if (backFiltered.length >= 3) {
          result.back = poly3.fromPointsAndPlane(backFiltered, pplane)
        }
      }
    }
  }
  return result
}

// Exact comparison of planes, as before. Kept local so the tolerance of the
// geometric predicates above stays explicit and scale aware.
const planeEquals = (a, b) => ((a[0] === b[0]) && (a[1] === b[1]) && (a[2] === b[2]) && (a[3] === b[3]))

module.exports = splitPolygonByPlane
