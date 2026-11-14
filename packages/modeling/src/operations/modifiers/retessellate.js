const geom3 = require('../../geometries/geom3')
const poly3 = require('../../geometries/poly3')
const { NEPS } = require('../../maths/constants')
const { toleranceForPoints } = require('../../maths/utils/tolerance')
const reTesselateCoplanarPolygons = require('./reTesselateCoplanarPolygons')

/*
  After boolean operations all coplanar polygon fragments are joined by a retesselating
  operation. geom3.reTesselate(geom).
  Retesselation is done through a linear sweep over the polygon surface.
  The sweep line passes over the y coordinates of all vertices in the polygon.
  Polygons are split at each sweep line, and the fragments are joined horizontally and vertically into larger polygons
  (making sure that we will end up with convex polygons).
*/
const retessellate = (geometry, options) => {
  if (geometry.isRetesselated) {
    return geometry
  }

  // Legacy mode keeps the historical absolute tolerances, which the 2D
  // boolean path relies upon (it still runs at the original spatial resolution).
  const legacy = options !== undefined && options !== null && options.legacy === true // internal marker, see booleans/booleanOptions.js

  const polygons = geom3.toPolygons(geometry).map((polygon, index) => ({ vertices: polygon.vertices, plane: poly3.plane(polygon), index: index }))
  const classified = classifyPolygons(polygons, legacy)

  const destPolygons = []
  classified.forEach((group) => {
    if (Array.isArray(group)) {
      const coplanarPolygons = legacy
        ? reTesselateCoplanarPolygons(group, { legacy: true })
        : reTesselateCoplanarPolygons(group)
      // Use loop instead of spread to avoid stack overflow with large arrays
      for (let i = 0; i < coplanarPolygons.length; i++) {
        destPolygons.push(coplanarPolygons[i])
      }
    } else {
      destPolygons.push(group)
    }
  })

  const result = geom3.create(destPolygons)
  result.isRetesselated = true

  return result
}

// Tolerance used while comparing a plane component. The legacy mode reproduces
// the historical constants exactly; otherwise the tolerance is derived from the
// magnitude of the plane components being compared, so that coincident planes
// of a model translated far from the origin are still recognised while two
// distinct surfaces a small feature apart are kept separate.
const componentTolerance = (component, scale, legacy) => {
  if (legacy) {
    return component === 3 ? 0.000000015 : NEPS
  }
  if (component === 3) {
    // plane[3] is a distance, compare it relative to the coordinate magnitude
    return Math.max(NEPS, toleranceForPoints([], [0, 0, 0, scale]))
  }
  // normals are unit vectors, their comparison is dimensionless
  return NEPS
}

const classifyPolygons = (polygons, legacy) => {
  let clusters = [polygons] // a cluster is an array of potentially coplanar polygons
  const nonCoplanar = [] // polygons that are known to be non-coplanar
  // go through each component of the plane starting with the last one (the distance from origin)
  for (let component = 3; component >= 0; component--) {
    const maybeCoplanar = []
    clusters.forEach((cluster) => {
      // the tolerance has to cover every plane in the cluster
      let scale = 0
      cluster.forEach((polygon) => {
        const planeScale = Math.abs(polygon.plane[3])
        if (planeScale > scale) scale = planeScale
        const vertices = polygon.vertices
        for (let i = 0; i < vertices.length; i++) {
          const vertex = vertices[i]
          const vertexScale = Math.max(Math.abs(vertex[0]), Math.abs(vertex[1]), Math.abs(vertex[2]))
          if (vertexScale > scale) scale = vertexScale
        }
      })
      const tolerance = componentTolerance(component, scale, legacy)
      // sort the cluster by the current component
      cluster.sort(byPlaneComponent(component, tolerance))
      // iterate through the cluster and check if there are polygons which are not coplanar with the others
      // or if there are sub-clusters of coplanar polygons
      let startIndex = 0
      for (let i = 1; i < cluster.length; i++) {
        // if there's a difference larger than the tolerance, split the cluster
        if (cluster[i].plane[component] - cluster[startIndex].plane[component] > tolerance) {
          // if there's a single polygon it's definitely not coplanar with any others
          if (i - startIndex === 1) {
            nonCoplanar.push(cluster[startIndex])
          } else { // we have a new sub cluster of potentially coplanar polygons
            maybeCoplanar.push(cluster.slice(startIndex, i))
          }
          startIndex = i
        }
      }
      // handle the last elements of the cluster
      if (cluster.length - startIndex === 1) {
        nonCoplanar.push(cluster[startIndex])
      } else {
        maybeCoplanar.push(cluster.slice(startIndex))
      }
    })
    // replace previous clusters with the new ones
    clusters = maybeCoplanar
  }
  // restore the original order of the polygons
  const result = []
  // polygons inside the cluster should already be sorted by index
  clusters.forEach((cluster) => {
    if (cluster[0]) result[cluster[0].index] = cluster
  })
  nonCoplanar.forEach((polygon) => { result[polygon.index] = polygon })

  return result
}

const byPlaneComponent = (component, tolerance) => (a, b) => {
  if (a.plane[component] - b.plane[component] > tolerance) {
    return 1
  } else if (b.plane[component] - a.plane[component] > tolerance) {
    return -1
  }
  return 0
}

module.exports = retessellate
