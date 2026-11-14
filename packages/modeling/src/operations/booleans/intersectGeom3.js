const flatten = require('../../utils/flatten')

const retessellate = require('../modifiers/retessellate')
const { isLegacyOptions } = require('./booleanOptions')

const intersectSub = require('./intersectGeom3Sub')

/*
 * Return a new 3D geometry representing space in both the first geometry and
 * in the subsequent geometries. None of the given geometries are modified.
 * @param {...geom3} geometries - list of 3D geometries
 * @returns {geom3} new 3D geometry
 */
const intersect = (...geometries) => {
  // the last argument may be the internal options marker
  const options = isLegacyOptions(geometries[geometries.length - 1]) ? geometries.pop() : undefined
  geometries = flatten(geometries)

  let newgeometry = geometries.shift()
  geometries.forEach((geometry) => {
    newgeometry = intersectSub(newgeometry, geometry, options)
  })

  newgeometry = retessellate(newgeometry, options)
  return newgeometry
}

module.exports = intersect
